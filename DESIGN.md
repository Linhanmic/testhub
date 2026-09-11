# TestHub 设计文档

本文档描述 TestHub **当前实现**的架构、模块、数据模型与协议。使用方法见 [README.md](README.md)，开发流程见 [QUICKSTART.md](QUICKSTART.md)，未来规划见 [TODO.md](TODO.md)。

## 1. 系统概述

TestHub 是一个常驻内存的自动化测试守护进程。它接收 HTTP 请求，把测试任务放入优先级队列，由工作线程解析 Gauge 风格的 Markdown 规范并逐步驱动外部 Runner 进程执行步骤实现，执行过程通过事件总线实时推送到 WebSocket 客户端与内嵌 Web UI。

### 1.1 设计目标

| 目标 | 落地方式 |
|------|----------|
| 零第三方依赖、单一可执行文件 | 自带 JSON、HTTP/1.1、WebSocket、SHA-1、Base64、规范解析器；Web 资源在构建期内嵌 |
| 长运行、可观测 | 常驻队列 + 结果历史（JSON 落盘，重启回放）；事件总线 + WebSocket；健康/状态端点；结构化日志 |
| 语言无关的步骤实现 | Runner 以子进程运行，通过 stdin/stdout JSON-lines 通信；任何语言均可实现 |
| 可预测的并发 | 测试级并发由 worker 数控制；有状态 Runner 以场景为粒度独占 |
| 可测试 | 全部核心模块可在进程内实例化（端口 0、mock Runner），单元/集成测试无外部依赖 |

### 1.2 与 Gauge 的关系

TestHub 复用 Gauge 的规范语法（`.spec` / `.cpt`），便于迁移已有规范；但不使用 Gauge 的 CLI、gRPC/Protobuf 协议或插件。

| 特性 | Gauge | TestHub |
|------|-------|---------|
| 运行模式 | 每次执行启动新进程 | 守护进程持续运行 |
| 触发方式 | CLI | HTTP API / Web UI |
| 结果获取 | 控制台 / 报告文件 | REST 查询 + WebSocket 推送 + Web UI |
| Runner 协议 | gRPC + Protobuf | JSON-lines over stdio |
| 状态 | 无状态 | 内存中维护队列与进度；结果持久化到磁盘 |

## 2. 系统架构

```
                 ┌──────────────────────── TestHub 进程 ─────────────────────────┐
  浏览器 ──HTTP──►│ HttpServer ──► ApiRoutes (REST)                                │
  curl/CI ───────►│    │            WebUi (内嵌静态资源 / --web-dir, SPA 回退)      │
  WS 客户端 ──WS─►│    └─升级──► WebSocketServer ◄──subscribe── EventBus            │
                 │                                                ▲  publish     │
                 │  ExecutionEngine                               │              │
                 │    ├─ TestQueue (优先级 + FIFO)                 │              │
                 │    ├─ worker 线程 ×N ── executeTest/Spec/Scenario/Step ─────────┤
                 │    ├─ SpecRepository ── SpecParser / ConceptDictionary          │
                 │    │     └─ SpecWatcher (轮询目录，变更 → specs.reloaded) ────────┤
                 │    ├─ Scheduler (UTC cron 轮询，data/schedules/*.json) ───────────┤
                 │    ├─ records_ (状态 + 结果历史，环形上限)                        │
                 │    └─ ResultStore (data/results/<id>.json，启动时回放)             │
                 │  CallbackNotifier ◄──subscribe(test.completed)── EventBus        │
                 │    └─ HttpClient ── POST callback_url（指数退避重试）──► 外部系统   │
                 │  RunnerBridge (Runner 池：槽位 ×N、会话/并行流、逐槽自愈、步骤缓存)  │
                 │    ├─ MockRunner  (进程内，并发安全 → 池收缩为 1 个共享槽位)        │
                 │    └─ ProcessRunner ×N (POSIX fork/exec 或 Windows CreateProcess)│
                 └────────────────────────────┼───────────────────────────────────┘
                                              │ stdin/stdout JSON-lines（每进程一对管道）
                                              ▼
                       runners/python/testhub_runner.py ×N → step_impl/*.py
                       或 runners/node/testhub_runner.js ×N → step_impl/*.js
```

### 2.1 线程模型

| 线程 | 数量 | 职责 |
|------|------|------|
| accept 线程 | 1 | `poll()` 监听 socket，接入连接后交给工作池 |
| HTTP 工作线程 | `server.worker_threads`（默认 8） | 解析请求（keep-alive、流水线）、路由、响应；WebSocket 升级后该线程转为该连接的读循环 |
| 引擎 worker | `execution.max_concurrent_tests`（默认 1） | 从队列取测试并执行；执行前向 Runner 池申请一个槽位（会话），测试期间独占 |
| EventBus 派发线程 | 1 | 把事件异步投递给订阅者（WS 连接、引擎内部、日志），避免阻塞发布方 |
| Runner 读线程 | 每个 ProcessRunner 2（stdout / stderr） | 读取子进程输出，按 `id` 匹配响应；`log` 消息转为 `runner.log` 事件 |
| Runner 启动线程 | 临时，池大小 −1 | 池启动/重启时并行拉起其余 Runner 进程，随后 join |
| 回调投递线程 | 1（CallbackNotifier） | 按到期时间取任务 POST `callback_url`，失败指数退避重试 |
| 规范监控线程 | 1（SpecWatcher，可禁用） | 每 `specs.watch_interval_ms` 扫描一次目录快照，变更时重载概念并发布 `specs.reloaded` |
| 调度线程 | 1（Scheduler，可禁用） | 每 `scheduler.interval_ms` 扫描到期的 UTC cron 计划并提交测试 |

## 3. 核心模块

### 3.1 HttpServer（`src/server/http_server.*`）

- HTTP/1.1：`Content-Length` 正文、keep-alive、同一连接内流水线（剩余字节跨请求保留）、首字节/读超时、最大正文限制。
- 路由：`get/post/put/del(path, handler)`；路径支持 `{param}` 与末尾 `*` 通配；HEAD 自动回退到 GET；路径匹配但方法不匹配时返回 405；`OPTIONS` 与 CORS 头统一处理。
- 静态资源：`addStaticAsset(path, content, mime)` 内存资源，带 ETag / `If-None-Match` → 304；`serveDirectory()` 从磁盘提供；`setFallback()` 实现 SPA 回退。
- 升级钩子：`addUpgradeHandler(path, handler)`，WebSocketServer 借此接管 `/ws/v1/events` 连接。
- 端口 0 表示自动分配，`port()` 返回实际端口，供测试使用。

### 3.2 ApiRoutes（`src/server/api_routes.cpp`）

`TestHub::registerApiRoutes()` 注册全部 REST 端点（清单见 README）。约定：

- 成功响应为 JSON 对象；错误统一为 `{"error": "...", "status": <code>}`。
- 提交测试返回 `202 Accepted` + `Location: /api/v1/tests/{id}`。
- 结果未就绪时 `GET /tests/{id}/result` 返回 202，`GET /tests/{id}/report` 返回 409；取消已结束测试返回 409。
- `spec_files` 为相对规范目录的路径，可为目录；空数组表示全部规范。
- 规范写接口拒绝 `..` 与非 `.spec/.md/.cpt` 扩展名。
- `POST /projects/{id}/select` 切换当前规范根目录（找不到 404；有测试排队或运行中 409）。未配置 `specs.projects` 时合成 id=`default` 的单项。

### 3.2a AuthPolicy（`src/server/auth.h`）

- 配置 `server.auth_token` 非空即启用；通过 `HttpServer::setRequestFilter()` 在路由之前统一校验（WebSocket 升级同样经过过滤器）。
- 默认只保护写操作（POST/PUT/PATCH/DELETE）；`server.auth_protect_reads=true` 时 GET/HEAD 与 WebSocket 也需要 token。静态 UI、`/api/v1/health` 与 CORS 预检始终放行。
- 凭据来源：`Authorization: Bearer <token>`、`X-Auth-Token`；GET/HEAD/WebSocket 额外接受查询参数 `access_token`（浏览器下载链接与 WebSocket 无法自定义头）。比较使用常量时间算法。
- 失败返回 `401` + `WWW-Authenticate: Bearer realm="TestHub"`；`GET /health` 暴露 `auth_required` / `auth_protect_reads` 供 UI 决定是否提示输入；`GET /config` 与 `--print-config` 把 token 掩码为 `***`。
- Web UI 把 token 存于 `localStorage`，请求自动附加 `Authorization`；收到 401 弹出输入框，保存后重连 WebSocket 并重新加载当前页面。

### 3.3 ExecutionEngine（`src/engine/execution_engine.*`）

```
submit(request)
  ├─ 解析并校验 spec_files → resolvedSpecs（找不到即 400）
  ├─ 生成 test_id（test-YYYYMMDD-HHMMSS-NNN）
  ├─ records_[id] = {request, status=QUEUED}
  ├─ queue_.push(id, priority)  →  publish(test.submitted, queue.updated)
worker 线程
  ├─ pop → status=RUNNING → publish(test.started)
  ├─ for spec in resolvedSpecs:
  │     load + expand concepts → publish(spec.started)
  │     runHook(before_spec)
  │     for row in dataTable (或 1 次):
  │        for scenario (标签/场景名过滤):
  │           session = runner.acquireSession()        ← 有状态 Runner 场景级独占
  │           publish(scenario.started)
  │           before_scenario → contexts → steps → teardowns → after_scenario
  │           每步：resolveArgs(动态参数/文件/表格) → runner.executeStep → publish(step.*)
  │           publish(scenario.completed) → updateStatus → publish(test.progress)
  │     runHook(after_spec) → publish(spec.completed)
  └─ 汇总 TestResult → status=PASSED/FAILED/ERROR/CANCELLED → publish(test.completed)
```

- **过滤**：`TagFilter` 支持 `&`/`|`/`!`/括号/`and`/`or`/`not`/逗号；`tags` 数组中的多个表达式取交集。场景过滤按名称任一匹配。规范级标签与场景级标签合并后参与匹配。
- **超时**：测试级 `timeout_ms`（默认 `execution.default_timeout`）与步骤级 `execution.step_timeout`；剩余时间不足时步骤超时自动收紧；超时后剩余步骤标记跳过，测试状态为 `error`。
- **取消**：`cancel(id)` 对排队任务直接出队；对运行中任务置标志，当前步骤结束后剩余场景/步骤标记为跳过（保留完整步骤列表），状态为 `cancelled`。
- **fail_fast**：首个失败场景后跳过其余场景。
- **步骤重试**：请求字段 `step_retry`（0–5）与规范/场景标签 `retry:N` / `retry-N` 取较大值。仅当步骤结果为 `FAILED`（断言失败）时重试，不重试 `TEST_ERROR`、取消或超时。概念步骤的内层各自独立重试。每次重试发布 `step.retry`（`attempt`、`max_retries`、`error`），最终 `step.completed` 带 `attempts`（含首次）。Mock Runner 步骤文案含 `flaky` 时第一次失败、之后通过。
- **重跑**：`rerun(id, failed_only)` 复制原请求；`failed_only` 时把失败场景名写入 `scenarios` 过滤。
- **历史**：`records_` 保留最近 `execution.history_limit` 条已完成记录；`DELETE /tests` 清空。
- **持久化**：`ResultStore`（`src/engine/result_store.*`）在测试进入终态时把 `{request, status, result, resolved_specs}` 写入 `<results_dir>/<test_id>.json`（临时文件 + rename 原子替换）；`start()` 时回放目录中的记录到 `records_` 与统计，并按 `history_limit` 裁剪；删除/清空历史同步删除文件；损坏文件跳过并告警。`results_dir` 为空则仅保存在内存。
- **零匹配**：没有任何场景被执行的测试状态为 `skipped` 并附带警告，而不是 `passed`。
- **趋势与对比**：`trendRuns()` 按提交顺序收集终态记录（跳过 `cancelled`）。`GET /api/v1/trends?spec=&limit=` 用 `groupTrends` 按规范聚合通过率/耗时（`spec` 为空时第一条序列为 `(all)`）。`GET /api/v1/tests/{id}/compare?with=` 对比两次结果的场景（key = 规范文件 + 场景名 + 数据行号）；`with` 为空则自动选规范集合有交集的上一终态。分类：`regressed` / `improved` / `still_failed` / `unchanged` / `added` / `removed`。纯函数在 `src/engine/trends.h`。
- **报表**：`ReportWriter`（`src/report/report_writer.*`）从 `TestRecord` 按需渲染 JUnit XML（规范 → `testsuite`，场景/数据行 → `testcase`，`failure`/`error`/`skipped`，步骤与 Runner 消息进 `system-out`，标签/环境/元数据进 `properties`）或自包含 HTML；不落盘，历史记录回放后同样可导出。
- **进度**：`progress = (executed + skipped) / total`，`executed_scenarios` 只统计真正运行过的场景。

### 3.4 SpecParser / SpecRepository（`src/spec/`）

支持的语法：

| 语法 | 说明 |
|------|------|
| `# 标题` 或下划 `===` | 规范标题；`tags:` 行为规范标签 |
| `## 标题` 或下划 `---` | 场景；其后的 `tags:` 为场景标签 |
| `* 步骤` | 步骤；规范标题之后、第一个场景之前的步骤为上下文步骤 |
| `___`（三个及以上下划线）之后的步骤 | 清理步骤，每个场景结束后执行 |
| `\|a\|b\|` 表格紧跟规范标题 | 规范级数据表 → 数据驱动，每行执行一遍所有场景 |
| `\|a\|b\|` 表格紧跟步骤 | 内联表格参数（`table` 类型） |
| `"文本"` | 静态参数 |
| `<name>` | 动态参数：从数据表当前行取值；不存在时按原文传递 |
| `<file:path>` | 特殊参数：读取相对规范文件的文本 |
| `<table:path.csv>` | 特殊参数：CSV → 表格 |
| `.cpt` 中 `# 概念 <p>` + 步骤 | 概念定义；规范中同文本步骤会展开为 `concept_steps` |

解析结果携带行号；错误（无标题、场景在标题前等）与警告（无场景）分别收集，`SpecRepository::validate()` 与 `POST /specs/validate` 直接返回。`SpecRepository` 负责扫描目录（`.spec`/`.md`）、读写文件、缓存概念字典（默认 `specs/concepts/`，可配置）。

### 3.4a SpecWatcher（`src/spec/spec_watcher.*`）

- 轮询而非 inotify/FSEvents：零依赖、跨平台，规范目录通常只有几十到几百个文件，秒级轮询开销可忽略。
- 快照为 `绝对路径 → {mtime, size}`，覆盖规范目录与（若在目录外的）概念目录中的 `.spec/.md/.cpt`；每轮 `diff` 得到 created/updated/deleted。
- **稳定窗口**：修改时间距现在不足 `settle_ms`（200 ms）的文件视为仍在写入——已存在的沿用旧签名、新建的暂不纳入，下一轮再报告，避免解析到编辑器写了一半的内容。
- 有 `.cpt` 变化时调用 `SpecRepository::reloadConcepts()`；随后发布一个 `specs.reloaded`（`source=watcher`，含计数与文件清单，最多列 20 个）。规范文件本身不缓存解析结果，无需失效。
- API 的 PUT/DELETE 写完文件后调用 `acknowledge()` 重记快照，避免同一变更被报告两次；`POST /specs/reload` 则主动 `scan()` 并把变更集放进响应。
- `GET /status` 的 `spec_watcher` 暴露 `enabled / interval_ms / tracked_files / scans / changes / reloads / last_change_at`；配置 `specs.watch`、`specs.watch_interval_ms`，CLI `--watch-interval` / `--no-watch`。

### 3.4b Scheduler（`src/engine/scheduler.*`，`src/util/cron.h`）

- 五字段 cron（分 时 日 月 星期）按 **UTC** 求值；支持 `*` `,` `-` `/`、`JAN`–`DEC` / `SUN`–`SAT`，以及 `@hourly` `@daily` `@weekly` `@monthly` `@yearly`。日与星期都不是 `*` 时按 Vixie：二者满足其一即可。
- 后台线程按 `scheduler.interval_ms`（默认 1 s）调用 `tick(now)`；同一 UTC 分钟只触发一次，`last_fired_minute` 随 JSON 落盘，重启不会双发。仅改 `enabled` 等字段不会重置该标记。
- 计划文件：`<schedules_dir>/<id>.json`（临时文件 + rename）。提交时 `submitted_by = "schedule:<id>"`，`metadata.schedule_id`。`skip_if_running`（默认 true）在上一 `last_test_id` 仍 queued/running 时跳过并发布 `schedule.skipped`。
- 选择轮询而非系统 crontab：零依赖、可注入 now、与 SpecWatcher 同一模式。`--no-scheduler` 仍加载计划，可 CRUD / `POST .../run`，但不启线程。
- `GET /status` 的 `scheduler` 暴露 `enabled / interval_ms / count / enabled_count / ticks / fires / skips / errors`。

### 3.5 RunnerBridge / Runner（`src/runner/`）

```cpp
class Runner {                      // 抽象接口
    bool start(); void stop();
    std::vector<StepValue> getAllSteps();
    StepResult executeStep(const StepExecutionRequest&);
    HookResult runHook(HookType, const ExecutionContext&);
    bool isAlive(); std::string version();
    virtual bool isConcurrencySafe() const { return false; }
};
```

- `MockRunner`：进程内实现，所有步骤通过，可配置 `mock_delay_ms`，`isConcurrencySafe() == true`；用于测试与演示。
- `ProcessRunner`：启动子进程（POSIX `fork/exec` 经 `/bin/sh -c`，Windows `CreateProcess`），stdin 写请求、stdout 读响应、stderr 直通日志；请求携带递增 `id`，响应按 `id` 匹配，支持超时。
- `ProcessRunner::stop()` 先发 `kill` 消息礼貌等待，再对**进程组**发 SIGTERM/SIGKILL。命令经 `sh -c` 启动，子进程 `setpgid(0,0)` 自成进程组；若 `sh` 被外部杀死，真正的 Runner 会成为孤儿并继续持有管道，读线程永远等不到 EOF——因此即使直接子进程已退出也要清理进程组（迭代 14 修复的死锁）。
- `RunnerBridge`（Runner 池）：
  - 按 `runner.language` 选择实现：`mock` / `python`（自动定位 `runners/python/testhub_runner.py`）/ `node`/`js`/`javascript`/`nodejs`（自动定位 `runners/node/testhub_runner.js`）/ `custom`（`runner.command`）；查找根为 `$TESTHUB_HOME`、可执行文件所在目录及其上级、`share/testhub`、当前目录。`setRunnerFactory()` 允许测试/嵌入方注入自定义 Runner。
  - **槽位**：`runner.pool_size`（`--runner-pool`，0 = 跟随 `max_concurrent_tests`，上限 64）个 `Slot{runner, state, restartCount, users, stepsExecuted…}`。启动时先拉起槽位 0 探测 `isConcurrencySafe()`：并发安全（mock）→ 池收缩为 1 个共享槽位；否则并行拉起其余进程，每个子进程可通过环境变量 `TESTHUB_RUNNER_INDEX` / `TESTHUB_RUNNER_POOL_SIZE` 得知自己的位置。
  - **会话**：`acquireSession()` 返回 RAII `Session`，占用一个空闲槽位并通过 `thread_local` 绑定到当前线程；此后该线程的 `executeStep/runHook` 都落在绑定的 Runner 上。`reserveSlots(n)` 原子预约 n 个槽位（等到同时有 n 个空闲才一次性占用，避免“先拿 1 再等其余”的死锁），再在目标线程上 `attachReserved()`。默认 `parallel_streams=1`：引擎预约 1 个槽位并持有到测试结束。`parallel_streams>1` 时预约 N 个槽位，把场景按轮询分到 N 条流，每条流在自己的进程上跑 `before_suite` → 所负规范的 `before_spec`/场景/`after_spec` → `after_suite`（Gauge `--parallel` 流语义）；结果按原始规范/场景顺序回填。没有空闲槽位时测试保持 `queued`。会话外的调用（`GET /runner/steps`）临时占用一个空闲槽位。
  - **分配策略**：优先空闲且在线的槽位，其次可重启的槽位；只有所有槽位都永久失效（重启预算耗尽）时才分配失效槽位，让调用方得到明确错误而不是无限等待。
  - **逐槽自愈**：`ensureAlive(slot)` 在槽位被独占的前提下检查进程存活，按 `auto_restart` / `max_restarts`（每槽独立预算）重启；重启只影响该槽位，其他测试不受干扰；重启在锁外进行，不阻塞其他槽位的执行与状态查询。进程在两次使用之间退出时按需重启（下次分配到它时），状态里报告为 `error` + "will be restarted on next use"。
  - **生命周期**：`stopRunner()` / `restartRunner()` 先置 `draining_`（新会话等待），等待所有槽位释放，再并行停止/拉起全部进程；`lifecycleMutex_` 串行化这三种操作。
  - `getStatus()` 聚合：任一槽位在线即 `connected`（有会话占用则 `busy`），全部离线且有失败为 `error`；`pool_size / alive / busy / restart_count（总和）` 与 `runners[]` 明细（`index/state/pid/version/restart_count/steps_executed/busy/last_error`）。`implemented_steps` 只读握手时预热的缓存，**禁止**在 `getStatus` 里向 Runner 发 `get_steps`（JSON-lines 一次一条，步骤实现若再访问本进程 `/health`/`/status` 会互相等待）。Runner 事件 `runner.*` 携带 `slot`。
  - 缓存 `get_steps` 结果（任一在线 Runner 即可回答，不占用槽位），供 `GET /runner/steps` 与 UI 的"未实现步骤"标注使用。
  - **自举环境**：`TestHub::start()` 先 `HttpServer::start()` 绑定端口，再把 `TESTHUB_URL`（`callbacks.public_base_url`，否则 `http://127.0.0.1:<boundPort>`）写入 `execution.environment` 与 Runner 子进程环境（`ProcessRunner` `setenv`）；启用鉴权时同步注入 `TESTHUB_TOKEN`。`GET /status` 与 `server.started` 事件携带 `url`。`specs/selfcheck.spec` 通过 `runners/*/step_impl/api_steps.*` 用这些变量调用本进程 HTTP API；步骤只断言子测试 POST 202，不轮询其结束，以免 `-j 1` 死锁。自举期间当前槽位为 `busy`，「Runner 应在线」同时接受 `connected` / `busy`。

### 3.6 EventBus（`src/event/event_bus.*`）

单例；`publish()` 入队后由派发线程调用订阅者；订阅支持精确类型、`prefix.*` 通配与 `*`；保留最近 N 条历史供 `GET /events` 与 WebSocket 连接时回放。

### 3.6a CallbackNotifier（`src/notify/callback_notifier.*`，`src/util/http_client.*`）

- 订阅 `test.completed` 与 `test.cancelled`（仅 `stage=queued`，运行中取消最终仍会产生 `test.completed`）；请求带 `callback_url` 时把任务放入投递队列。
- 单独的投递线程用零依赖 `HttpClient`（仅 `http://`，阻塞式，连接/读写超时）POST JSON 载荷；头部 `X-TestHub-Event` / `X-TestHub-Test-Id` / `X-TestHub-Attempt`。
- 2xx 视为成功；网络错误、5xx、429 按 `retry_backoff_ms × 2^(n-1)` 退避重试至 `max_attempts`，其余状态码不重试。结果以 `callback.delivered` / `callback.failed` 事件发布，并计入 `GET /status` 的 `callbacks`。
- 载荷：`event`、`test_id`、`name`、`state`、场景计数、时间、`duration`、`errors`/`warnings`、`failed_scenarios_detail[]`（规范/场景/数据行/错误）、`links{status,result,report_junit,report_html,ui}`（前缀为 `callbacks.public_base_url`）、`attempt`、`sent_at`。
- 提交时 `callback_url` 非法（非 `http://`）直接返回 400。

### 3.7 WebSocketServer（`src/server/websocket_server.*`）

- RFC 6455 握手（`Sec-WebSocket-Accept` = Base64(SHA-1)），文本/二进制/ping/pong/close 帧，分片与掩码处理。
- 连接建立后发送 `{"type":"welcome","connection_id":"...","server_time":"..."}`。
- 客户端可发送 `{"action":"subscribe","events":["test.*","scenario.*"],"test_id":"..."}` 缩小范围，`{"action":"ping"}` 得到 `pong`。
- 每条事件封装为 `{"type":"event","event":...,"test_id":...,"timestamp":...,"data":{...}}`。

### 3.8 Web UI（`web/`，`src/server/web_ui.cpp`）

原生 JS 单页应用，hash 路由：

| 路由 | 功能 |
|------|------|
| `#/dashboard` | 统计卡片、通过率环图、最近测试、实时事件摘要、调度器摘要、通过率 sparkline |
| `#/run` | 提交表单（规范多选、标签表达式、场景过滤、优先级、环境、超时、fail_fast、步骤重试、并行流），实时 curl 预览 |
| `#/tests` / `#/tests/{id}` | 列表（状态筛选、取消/重跑/删除）；详情页：状态卡、**与上次对比**（回归/改善表）、**运行中由事件流构建的实时执行树**、完成后的结果树（上下文/步骤/清理、概念展开、数据行参数替换、消息与堆栈、**搜索 / 只看失败**）、请求信息、事件面板（暂停 / 导出 JSON） |
| `#/trends` | 按规范筛选的通过率/耗时 SVG sparkline 与历次点数表；`g` `a` 跳转 |
| `#/schedules` | 测试计划列表（启用开关、立即运行、删除、搜索）；创建/编辑表单（UTC cron、规范、标签、`skip_if_running`）；`g` `c` 跳转 |
| `#/specs` / `#/specs/{file}` | 规范列表（校验全部、重新加载、新建）；详情：结构视图（标出 Runner 未实现的步骤）、源码、**编辑器（叠加语法高亮、基于 `/runner/steps` 与概念的步骤补全）**；侧栏 `<select>` 切换规范项目 |
| `#/runner` | Runner 状态、已实现步骤、重启 |
| `#/events` | 全量事件流，按类型/测试 ID 过滤，可隐藏 step.*，可暂停与导出 JSON |

构建时 `cmake/EmbedResources.cmake` 把 `web/*` 编译进 `web_assets.cpp`；`--web-dir` 允许从磁盘热改。页面切换时替换 `#main` 节点以丢弃旧监听器，避免重复请求。

## 4. 数据模型（`src/model/types.h`）

```cpp
enum class TestState { QUEUED, RUNNING, PASSED, FAILED, SKIPPED, CANCELLED, TEST_ERROR };
enum class Priority  { LOW, NORMAL, HIGH, URGENT };
enum class RunnerState { DISCONNECTED, CONNECTING, CONNECTED, BUSY, RUNNER_ERROR };

struct TestRequest {
    std::string id, name;
    std::vector<std::string> specFiles;   // 相对规范目录；空 = 全部
    std::vector<std::string> tags;        // 标签表达式，全部满足
    std::vector<std::string> scenarios;   // 场景名过滤，任一匹配
    std::string environment = "default";
    Priority priority = Priority::NORMAL;
    int timeoutMs = 0;                    // 0 = 默认
    bool failFast = false;
    int stepRetry = 0;                    // FAILED 步骤最大重试次数，上限 5
    int parallelStreams = 1;
    std::map<std::string, std::string> metadata;
    std::string callbackUrl;              // 完成后 POST 摘要（见 3.6a）
};

struct StepResult     { stepText, parameterizedText, state, errorMessage, stackTrace, duration, attempts,
                        messages[], isConcept, conceptSteps[] };
struct ScenarioResult { scenarioName, tags[], lineNumber, dataRowIndex, dataRow{}, state,
                        contextSteps[], stepResults[], teardownSteps[], errorMessage, duration };
struct SpecResult     { specFile, specName, tags[], state, scenarioResults[], total/passed/failed/skippedScenarios,
                        errorMessage, duration };
struct TestStatus     { testId, state, total/executed/passed/failed/skippedScenarios, progress,
                        currentSpec/Scenario/Step, submitTime, startTime, endTime, errors[], warnings[] };
struct TestResult     { testId, state, specs[], errors[], warnings[], duration, startTime, endTime };
struct Event          { type, testId, timestamp, data{string→string} };
```

JSON 序列化位于 `src/model/json_convert.h`，字段名为 snake_case（`spec_files`、`data_row_index`、`concept_steps` …）。

### 4.1 事件类型

| 类型 | data 关键字段 |
|------|---------------|
| `server.started` / `server.stopping` | `version`, `port` |
| `test.submitted` / `test.started` / `test.completed` / `test.cancelled` | `state`, `duration`, `passed_scenarios`, `failed_scenarios`, `stage` |
| `test.progress` | `progress`, `current_spec`, `current_scenario`, `current_step`, `executed_scenarios`, `total_scenarios` |
| `spec.started` / `spec.completed` | `spec`, `name`, `scenarios`, `tags`, `state`, `duration`, `error` |
| `scenario.started` / `scenario.completed` | `spec`, `scenario`, `tags`, `steps`, `data_row`, `state`, `duration`, `error` |
| `step.started` / `step.retry` / `step.completed` | `spec`, `scenario`, `step`, `parameterized_text`, `is_concept`, `state`, `duration`, `error`, `attempts`, `attempt`, `max_retries` |
| `runner.connecting` / `runner.connected` / `runner.disconnected` / `runner.error` / `runner.log` | `language`, `slot`（池中槽位序号）, `detail` / `message`, `level` |
| `queue.updated` | `queue_size`, `action`（enqueued/dequeued/cancelled） |
| `specs.reloaded` | `source`（manual / api / watcher / project）；api：`file`, `action`（created/updated/deleted）；watcher：`created`, `updated`, `deleted` 计数、`files`、`concepts`、`concepts_reloaded` |
| `project.changed` | `project_id`, `name`, `specs_dir`, `concepts` |
| `callback.delivered` / `callback.failed` | `url`, `status`, `attempts`, `error` |
| `schedule.created` / `updated` / `deleted` | `schedule_id`, `name`, `cron`, `enabled` |
| `schedule.triggered` / `skipped` / `error` | `schedule_id`, `name`, `cron`, `reason`, `error` |

`data` 的值全部为字符串，前端按需解析。

## 5. 通信协议

### 5.1 REST 示例

```http
POST /api/v1/tests
{"spec_files":["login.spec"],"tags":["smoke & !slow"],"priority":"high","fail_fast":true,"timeout_ms":60000,"step_retry":1}

HTTP/1.1 202 Accepted
Location: /api/v1/tests/test-20260910-142940-001
{"test_id":"test-20260910-142940-001","status":"queued","queue_position":0,"message":"Test submitted successfully"}
```

```http
GET /api/v1/tests/test-20260910-142940-001
{"test_id":"...","state":"running","progress":0.5,"total_scenarios":2,"executed_scenarios":1,
 "passed_scenarios":1,"failed_scenarios":0,"skipped_scenarios":0,
 "current_spec":"slow.spec","current_scenario":"报表生成","current_step":"等待 \"3\" 秒",
 "has_result":false,"request":{...},"resolved_specs":["slow.spec"],"submit_time":"...","start_time":"..."}
```

### 5.2 WebSocket

```
← {"type":"welcome","connection_id":3,"server_time":"2026-09-10T14:32:42.101Z"}
→ {"action":"subscribe","events":["test.*","scenario.*"],"test_id":"test-20260910-143242-003"}
← {"type":"subscribed","events":["test.*","scenario.*"],"test_id":"..."}
← {"type":"event","event":"scenario.completed","test_id":"...","timestamp":"...",
   "data":{"spec":"slow.spec","scenario":"后台任务处理","state":"passed","duration":"2.001"}}
```

### 5.3 Runner 通信协议

传输：Runner 进程 stdin 接收请求、stdout 返回响应，**每行一个 JSON 对象**；stderr 为自由日志。步骤实现里的 `print` / `console.log` 应重定向到 stderr（Python 与 Node 参考实现均已处理）。

请求均含 `id`（整数）与 `type`；响应回带同一 `id`。

| 请求 `type` | 请求字段 | 响应 `type` | 响应字段 |
|-------------|----------|-------------|----------|
| `ping` | — | `pong` | `version` |
| `get_steps` | — | `steps` | `steps: [{text, parameterized_text, params[]}]` |
| `execute_step` | `step_text`, `parameterized_text`, `args[]`, `context`, `timeout_ms` | `step_result` | `status`（`passed`/`failed`/`error`/`skipped`）, `message`, `stack_trace`, `duration_ms`, `messages[]` |
| `hook` | `hook`（`before_suite`/`after_suite`/`before_spec`/`after_spec`/`before_scenario`/`after_scenario`/`before_step`/`after_step`）, `context` | `hook_result` | `status`, `message`, `stack_trace`, `messages[]` |
| `kill` | — | （进程退出） | |

Runner 可随时主动发送 `{"type":"log","level":"info","message":"..."}`，TestHub 转为 `runner.log` 事件。

`args[]` 元素（TestHub 在发送前已解析动态/特殊参数，Runner 只需处理下面几种）：

```json
{"type":"static",  "value":"admin"}                       // 静态参数，或已用数据行/文件内容替换后的动态、<file:> 参数
{"type":"dynamic", "value":"a"}                           // 数据行中不存在该列时按原名传递
{"type":"table",   "table":{"headers":["h1","h2"],"rows":[["a","b"]]}}   // 内联表格或 <table:x.csv>
```

`context`：

```json
{"test_id":"...","spec_file":"login.spec","spec_name":"用户登录","scenario_name":"使用正确的用户名和密码登录",
 "tags":["smoke","auth"],"data_row":{"a":"1","b":"2"},"environment":{"ENV":"default"}}
```

两个参考 Runner 实现同一协议，同一套 `.spec` 可互换执行：

- Python（`runners/python/testhub_runner.py`）：`@step("文本 <参数>")`、`@before_scenario` 等装饰器、`DataTable`、`Messages.write()`、`SkipStep`、`data_store.{suite,spec,scenario}` 与 `ExecutionContext`。`step_impl/api_steps.py` 用 `TESTHUB_URL` 调本进程 API，供 `selfcheck.spec` 自举。
- Node.js（`runners/node/testhub_runner.js`，零 npm 依赖）：`step('文本 <参数>', fn)`、`beforeScenario(fn)` 等、同样的 `DataTable` / `Messages` / `SkipStep` / `dataStore`；步骤函数可以是 `async`，Runner 会按消息顺序 `await`。`require('testhub-runner')` 通过模块解析别名指向本文件，无需安装。`pong.version` 为 `node-1.0`。`step_impl/api_steps.js` 与 Python 版对应。

## 6. 配置

JSON 文件（`--config`），键与 `--print-config` 输出一致：

```json
{
  "server":    {"host":"0.0.0.0","port":8080,"worker_threads":8,"web_ui":true,"web_dir":"",
                "auth_token":"","auth_protect_reads":false},
  "runner":    {"language":"python","command":"","project_path":"runners/python",
                "connection_timeout":15000,"request_timeout":60000,"auto_restart":true,"max_restarts":5,"mock_delay_ms":0,
                "pool_size":0},
  "execution": {"max_concurrent_tests":1,"default_timeout":300000,"step_timeout":60000,"history_limit":200,
                "results_dir":"data/results","environment":{"BASE_URL":"http://localhost:3000"}},
  "callbacks": {"enabled":true,"timeout_ms":10000,"max_attempts":3,"retry_backoff_ms":1000,"public_base_url":""},
  "specs":     {"dir":"specs","concepts_dir":"","current":"default","watch":true,"watch_interval_ms":2000,
                "projects":[{"id":"default","name":"默认","dir":"specs","concepts_dir":""}]},
  "scheduler": {"enabled":true,"interval_ms":1000,"dir":"data/schedules"},
  "logging":   {"level":"info","file":"","requests":true}
}
```

优先级：CLI 参数 > 环境变量（`TESTHUB_AUTH_TOKEN`）> 配置文件 > 默认值。

## 7. 目录结构

```
src/
  main.cpp                 CLI、配置合并、daemonize / Windows 服务、信号
  win_service.{h,cpp}      Windows SCM 安装/卸载/ServiceMain（仅 WIN32 链入 testhub.exe）
  testhub.{h,cpp}          TestHubConfig + TestHub 门面
  server/                  http_server, websocket_server, api_routes, auth.h, web_ui, web_assets.h
  engine/                  execution_engine, result_store, test_queue, tag_filter, scheduler, trends.h
  notify/                  callback_notifier（callback_url 投递与重试）
  report/                  report_writer（JUnit XML / HTML）
  runner/                  runner, mock_runner, process_runner, runner_bridge
  spec/                    spec, spec_parser, spec_repository, spec_watcher（目录轮询监控）
  event/                   event_bus
  model/                   types, json_convert
  util/                    json, http_client, sha1, base64, logger, string_util, file_util, time_util, cron.h
web/                       index.html, app.js, app.css, favicon.svg
runners/python/            testhub_runner.py, step_impl/, test_runner_protocol.py
runners/node/              testhub_runner.js, step_impl/, test_runner_protocol.js
specs/                     示例规范与 concepts/
tests/                     test_framework.h, test_main.cpp, test_*.cpp, test_integration.cpp
packaging/                 docker 默认配置、Windows 打包/服务脚本、systemd 单元
Dockerfile / compose.yaml
cmake/EmbedResources.cmake
.github/workflows/ci.yml
```

## 8. 质量保障

- **单元测试**（`testhub_unit_tests`）：JSON 解析/序列化/下标；规范解析（标题、标签、上下文、清理、数据表、参数、概念、错误/警告）；标签表达式；优先级队列；事件总线通配与历史；HTTP 请求解析、路由、流水线、ETag、HEAD/405、请求过滤器；鉴权策略（读/写、凭据来源、常量时间比较）；WebSocket 握手与帧；执行引擎（mock Runner：过滤、数据驱动、超时、取消、fail_fast、重跑、并发会话、**parallel_streams 场景重叠且结果保序**、**step_retry / retry:N 仅重试 FAILED**、**trendRuns / compareTests 自动基线**）；结果持久化（JSON 往返、损坏文件跳过、重启回放与裁剪）；报表（JUnit 结构与计数、转义、空结果、HTML 自包含）；规范目录监控；Runner 池（注入假 Runner：并行分配与会话绑定、阻塞等待、逐槽重启与预算、健康槽位优先、并发安全收缩、停止/重启生命周期、**reserveSlots 原子预约**）；**cron 解析与调度器**（别名、Vixie DOM/DOW、同一分钟去重、skip_if_running、仅改 enabled 不重置 lastFiredMinute、持久化回放）；**趋势纯函数**（分组/截断、回归改善、数据行、added/removed）；**规范项目**（缺省合成 default、JSON current、空 id 别名去重、`--specs` 覆盖选中或改写、toJson 往返）。
- **集成测试**（`testhub_integration_tests`）：在临时目录复制 `specs/`，以端口 0 启动完整服务器，用原生 TCP 客户端验证 REST 全流程、并发请求、大正文、流水线、WebSocket 事件流、规范 CRUD、取消、重启后历史回放、回调投递（503 后重试成功、连接拒绝后放弃）、Bearer Token（写保护与全保护两种模式、WebSocket 查询参数）、WebSocket 秒连秒断压力回归、目录监控端到端、**真实 Python Runner 池**（两个进程并行执行两个 0.8 s 的测试总耗时 < 1.5 s；`kill -9` 其中一个进程后按需自愈且孤儿进程组被清理；手动重启替换全部进程；无 `python3` 时跳过）、**真实 Node.js Runner**（同一套 login/calculator/checkout 规范全部通过；断言失败带回 `AssertionError` 堆栈；未实现步骤报 error；无 `node` 时跳过）、**parallel_streams**（单个测试的两个 0.8 s 场景拆到两个 Python 进程，总耗时 < 1.5 s，结果顺序与规范一致）、**步骤重试**（mock flaky 步骤 `step_retry=1` 后通过且 `attempts=2`；越界 400）、**自举**（`selfcheck.spec` 经 Python / Node 调用本进程 HTTP API，并断言入队的 `calculator.spec` 子测试随后通过）、**测试计划**（CRUD、非法 cron 400、立即运行 202 且 `submitted_by=schedule:<id>`、停用）、**趋势与对比**（两次 `login.spec` 后 `GET /trends` 与 `GET /tests/{id}/compare` 自动基线）、**多规范项目**（`GET /projects`、切换后 `/specs` 根目录变化、运行中 409）。
- **并发正确性**：所有线程句柄的赋值与检查共享同一把锁（WebSocket 读线程见 `Connection::readerMutex`）；停止流程在持锁状态下改标志再 `notify`，避免丢失唤醒；终态记录先落盘再对外可见。排查偶发问题时用 ThreadSanitizer 构建（`-DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"`）运行集成测试，当前零告警。
- **协议测试**：`python_runner_protocol`（子进程启动 Python Runner，验证 ping/get_steps/execute_step/hook/kill 与错误路径）；`node_runner_protocol`（同样覆盖 async 步骤等待、`console.log` 不污染协议通道、缺失实现目录、非法 JSON 不杀死进程）。
- **CI**：Ubuntu（g++、clang++）、macOS、Windows MSVC，`-Werror`，`ctest`（Windows 跳过 POSIX 集成测试），二进制冒烟，独立 Python / Node 协议测试；另有 Docker 镜像构建并以 `login.spec` 冒烟。Linux 上 `--service install` 必须退出码 2。

## 9. 已知限制与演进方向

- 结果以单文件 JSON 持久化，适合中小规模历史；海量历史或跨实例查询需要 SQLite/数据库后端。
- Runner 池默认以"测试"为分配粒度：`pool_size < max_concurrent_tests` 时多余的 worker 会等待空闲进程（测试保持 `queued`）。`parallel_streams > 1` 时一个测试会原子预约多个槽位；若池中空闲进程不足请求的流数，该测试继续排队直到凑齐（不会降级为更少的流，以避免半占用死锁）。数据驱动的每一行视为独立场景参与分片。
- `POST /runner/restart` 与关停会等待正在执行的测试释放进程；长测试期间的手动重启因此可能等待较久（可先取消测试）。
- 回调仅支持 `http://`（无 TLS）；需要 HTTPS 时请经由本地反向代理或内网中转。
- 鉴权为单一共享 Bearer Token（无用户/角色区分），且服务本身不提供 TLS；公网暴露时请置于 HTTPS 反向代理之后。

详细任务列表见 [TODO.md](TODO.md)。
