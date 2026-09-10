# TestHub 设计文档

本文档描述 TestHub **当前实现**的架构、模块、数据模型与协议。使用方法见 [README.md](README.md)，开发流程见 [QUICKSTART.md](QUICKSTART.md)，未来规划见 [TODO.md](TODO.md)。

## 1. 系统概述

TestHub 是一个常驻内存的自动化测试守护进程。它接收 HTTP 请求，把测试任务放入优先级队列，由工作线程解析 Gauge 风格的 Markdown 规范并逐步驱动外部 Runner 进程执行步骤实现，执行过程通过事件总线实时推送到 WebSocket 客户端与内嵌 Web UI。

### 1.1 设计目标

| 目标 | 落地方式 |
|------|----------|
| 零第三方依赖、单一可执行文件 | 自带 JSON、HTTP/1.1、WebSocket、SHA-1、Base64、规范解析器；Web 资源在构建期内嵌 |
| 长运行、可观测 | 常驻队列 + 结果历史；事件总线 + WebSocket；健康/状态端点；结构化日志 |
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
| 状态 | 无状态 | 内存中维护队列、进度、历史 |

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
                 │    └─ records_ (状态 + 结果历史，环形上限)                        │
                 │                                                                │
                 │  RunnerBridge (会话锁、心跳、自动重启、步骤缓存)                    │
                 │    ├─ MockRunner  (进程内，所有步骤通过，可配置延迟)               │
                 │    └─ ProcessRunner (POSIX fork/exec 或 Windows CreateProcess)   │
                 └────────────────────────────┼───────────────────────────────────┘
                                              │ stdin/stdout JSON-lines
                                              ▼
                              runners/python/testhub_runner.py → step_impl/*.py
```

### 2.1 线程模型

| 线程 | 数量 | 职责 |
|------|------|------|
| accept 线程 | 1 | `poll()` 监听 socket，接入连接后交给工作池 |
| HTTP 工作线程 | `server.worker_threads`（默认 8） | 解析请求（keep-alive、流水线）、路由、响应；WebSocket 升级后该线程转为该连接的读循环 |
| 引擎 worker | `execution.max_concurrent_tests`（默认 1） | 从队列取测试并执行 |
| EventBus 派发线程 | 1 | 把事件异步投递给订阅者（WS 连接、引擎内部、日志），避免阻塞发布方 |
| Runner 读线程 | 每个 ProcessRunner 1 | 读取子进程 stdout，按 `id` 匹配响应；`log` 消息转为 `runner.log` 事件 |
| 心跳线程 | 1（RunnerBridge） | 周期 `ping`，超时/退出时按 `auto_restart` 重启 |

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
- 结果未就绪时 `GET /tests/{id}/result` 返回 202；取消已结束测试返回 409。
- `spec_files` 为相对规范目录的路径，可为目录；空数组表示全部规范。
- 规范写接口拒绝 `..` 与非 `.spec/.md/.cpt` 扩展名。

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
- **重跑**：`rerun(id, failed_only)` 复制原请求；`failed_only` 时把失败场景名写入 `scenarios` 过滤。
- **历史**：`records_` 保留最近 `execution.history_limit` 条已完成记录；`DELETE /tests` 清空。
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
- `RunnerBridge`：
  - 按 `runner.language` 选择实现：`mock` / `python`（自动定位 `runners/python/testhub_runner.py`：`$TESTHUB_HOME`、可执行文件所在目录及其上级、`share/testhub`、当前目录）/ `node` / `custom`（`runner.command`）。
  - `acquireSession()` 返回 `std::unique_lock<std::recursive_mutex>`：对非并发安全 Runner，引擎在整个场景期间持有它，多个 worker 不会交错同一 Runner 的步骤；并发安全 Runner 返回空锁。
  - 心跳 `ping` / 崩溃检测 / `auto_restart`（上限 `max_restarts`），重启期间通过 `shared_ptr<Runner>` 保证正在执行的调用安全。
  - 缓存 `get_steps` 结果，供 `GET /runner/steps` 与 UI 的"未实现步骤"标注使用。

### 3.6 EventBus（`src/event/event_bus.*`）

单例；`publish()` 入队后由派发线程调用订阅者；订阅支持精确类型、`prefix.*` 通配与 `*`；保留最近 N 条历史供 `GET /events` 与 WebSocket 连接时回放。

### 3.7 WebSocketServer（`src/server/websocket_server.*`）

- RFC 6455 握手（`Sec-WebSocket-Accept` = Base64(SHA-1)），文本/二进制/ping/pong/close 帧，分片与掩码处理。
- 连接建立后发送 `{"type":"welcome","connection_id":"...","server_time":"..."}`。
- 客户端可发送 `{"action":"subscribe","events":["test.*","scenario.*"],"test_id":"..."}` 缩小范围，`{"action":"ping"}` 得到 `pong`。
- 每条事件封装为 `{"type":"event","event":...,"test_id":...,"timestamp":...,"data":{...}}`。

### 3.8 Web UI（`web/`，`src/server/web_ui.cpp`）

原生 JS 单页应用，hash 路由：

| 路由 | 功能 |
|------|------|
| `#/dashboard` | 统计卡片、通过率环图、最近测试、实时事件摘要 |
| `#/run` | 提交表单（规范多选、标签表达式、场景过滤、优先级、环境、超时、fail_fast），实时 curl 预览 |
| `#/tests` / `#/tests/{id}` | 列表（状态筛选、取消/重跑/删除）；详情页：状态卡、**运行中由事件流构建的实时执行树**、完成后的结果树（上下文/步骤/清理、概念展开、数据行参数替换、消息与堆栈）、请求信息、事件面板 |
| `#/specs` / `#/specs/{file}` | 规范列表（校验全部、重新加载、新建）；详情：结构视图（标出 Runner 未实现的步骤）、源码、编辑器（校验/保存/删除） |
| `#/runner` | Runner 状态、已实现步骤、重启 |
| `#/events` | 全量事件流，按类型/测试 ID 过滤，可隐藏 step.* |

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
    std::map<std::string, std::string> metadata;
    std::string callbackUrl;              // 已接受，回调尚未实现（见 TODO）
};

struct StepResult     { stepText, parameterizedText, state, errorMessage, stackTrace, duration,
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
| `step.started` / `step.completed` | `spec`, `scenario`, `step`, `parameterized_text`, `is_concept`, `state`, `duration`, `error` |
| `runner.connecting` / `runner.connected` / `runner.disconnected` / `runner.error` / `runner.log` | `language`, `pid`, `version`, `message`, `level` |
| `queue.updated` | `queue_size`, `action`（enqueued/dequeued/cancelled） |
| `specs.reloaded` | `concepts`（重新加载）或 `file`, `action`（created/updated/deleted） |

`data` 的值全部为字符串，前端按需解析。

## 5. 通信协议

### 5.1 REST 示例

```http
POST /api/v1/tests
{"spec_files":["login.spec"],"tags":["smoke & !slow"],"priority":"high","fail_fast":true,"timeout_ms":60000}

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

传输：Runner 进程 stdin 接收请求、stdout 返回响应，**每行一个 JSON 对象**；stderr 为自由日志。Runner 的 `print` 应重定向到 stderr（Python 参考实现已处理）。

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

Python 参考 Runner（`runners/python/testhub_runner.py`）提供 `@step("文本 <参数>")`、`@before_scenario` 等装饰器、`DataTable`、`Messages.write()`、`SkipStep`、`data_store.{suite,spec,scenario}` 与 `ExecutionContext`。

## 6. 配置

JSON 文件（`--config`），键与 `--print-config` 输出一致：

```json
{
  "server":    {"host":"0.0.0.0","port":8080,"worker_threads":8,"web_ui":true,"web_dir":""},
  "runner":    {"language":"python","command":"","project_path":"runners/python",
                "connection_timeout":15000,"request_timeout":60000,"auto_restart":true,"max_restarts":5,"mock_delay_ms":0},
  "execution": {"max_concurrent_tests":1,"default_timeout":300000,"step_timeout":60000,"history_limit":200,
                "environment":{"BASE_URL":"http://localhost:3000"}},
  "specs":     {"dir":"specs","concepts_dir":""},
  "logging":   {"level":"info","file":"","requests":true}
}
```

优先级：CLI 参数 > 配置文件 > 默认值。

## 7. 目录结构

```
src/
  main.cpp                 CLI、配置合并、daemonize、信号
  testhub.{h,cpp}          TestHubConfig + TestHub 门面
  server/                  http_server, websocket_server, api_routes, web_ui, web_assets.h
  engine/                  execution_engine, test_queue, tag_filter
  runner/                  runner, mock_runner, process_runner, runner_bridge
  spec/                    spec, spec_parser, spec_repository
  event/                   event_bus
  model/                   types, json_convert
  util/                    json, sha1, base64, logger, string_util, file_util, time_util
web/                       index.html, app.js, app.css, favicon.svg
runners/python/            testhub_runner.py, step_impl/, test_runner_protocol.py
specs/                     示例规范与 concepts/
tests/                     test_framework.h, test_main.cpp, test_*.cpp, test_integration.cpp
cmake/EmbedResources.cmake
.github/workflows/ci.yml
```

## 8. 质量保障

- **单元测试**（`testhub_unit_tests`）：JSON 解析/序列化/下标；规范解析（标题、标签、上下文、清理、数据表、参数、概念、错误/警告）；标签表达式；优先级队列；事件总线通配与历史；HTTP 请求解析、路由、流水线、ETag、HEAD/405；WebSocket 握手与帧；执行引擎（mock Runner：过滤、数据驱动、超时、取消、fail_fast、重跑、并发会话）。
- **集成测试**（`testhub_integration_tests`）：在临时目录复制 `specs/`，以端口 0 启动完整服务器，用原生 TCP 客户端验证 REST 全流程、并发请求、大正文、流水线、WebSocket 事件流、规范 CRUD、取消。
- **协议测试**（`python_runner_protocol`）：以子进程启动 Python Runner，验证 ping/get_steps/execute_step/hook/kill 与错误路径。
- **CI**：Ubuntu（g++、clang++）与 macOS，`-Wall -Wextra -Wpedantic -Werror`，`ctest`，二进制冒烟（curl）。

## 9. 已知限制与演进方向

- 结果与历史仅保存在内存中，重启即丢失（计划：JSON/SQLite 持久化）。
- 同一时刻只有一个 Runner 进程；`max_concurrent_tests > 1` 时通过场景级会话锁串行化步骤执行，真正并行需要 Runner 池。
- `callback_url` 已在请求模型中接受但尚未回调。
- 无鉴权；建议在受信网络内部署或置于反向代理之后（计划：Bearer Token）。
- 报表导出（JUnit XML / HTML）尚未实现。

详细任务列表见 [TODO.md](TODO.md)。
