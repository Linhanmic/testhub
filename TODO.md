# TestHub 开发任务追踪

本文件记录路线图、迭代记录与决策。状态：`[x]` 完成、`[ ]` 待办、`[~]` 进行中。

## 当前状态（v1.1.0）

- 自包含 C++17 项目，零第三方依赖，`-Werror` 零警告（GCC / Clang）
- 109 个自动化测试全部通过（74 单元 + 16 集成 + 7 Python 协议 + 12 Node.js 协议），GitHub Actions 三平台 CI；ThreadSanitizer 零告警
- 约 15k 行（含前端、Python / Node.js Runner、测试）

## 路线图

### P0 — 已完成的基础能力

- [x] 仓库自包含：移除对外部 gauge-cpp 的依赖、清理 `build/` 产物、`.gitignore`、CMake 重构
- [x] 零依赖 JSON（解析/序列化/下标访问/错误定位）
- [x] `.spec` / `.cpt` 解析器（标题、标签、上下文、清理、数据表、参数、概念、行号、错误与警告）
- [x] HTTP/1.1 服务器（Content-Length、keep-alive、流水线、超时、`{param}` 路由、HEAD/OPTIONS/405、CORS、ETag 静态资源、SPA 回退）
- [x] 执行引擎（优先级队列、标签表达式、场景过滤、数据驱动、上下文/清理、超时、取消、fail_fast、重跑、历史）
- [x] Runner 抽象 + 子进程桥接（JSON-lines、崩溃检测、自动重启、步骤缓存）
- [x] Python 参考 Runner（装饰器、钩子、DataTable、Messages、SkipStep、data_store）+ 示例步骤实现
- [x] Node.js 参考 Runner（同一 JSON-lines 协议、async 步骤、零 npm 依赖）+ 示例步骤实现；同一套 .spec 可互换执行
- [x] 异步事件总线 + WebSocket 推送（订阅过滤、历史回放）
- [x] 内嵌 Web UI（总览、提交、测试记录、结果树、实时执行树、规范浏览/编辑/校验、Runner、事件流）
- [x] 测试体系（自带迷你框架、单元 + 集成 + 协议测试、ctest、CI）
- [x] 文档同步（README / DESIGN / QUICKSTART / TODO）
- [x] 结果持久化：终态记录 JSON 落盘（`data/results/`），启动回放历史与统计，随 history_limit 裁剪
- [x] 报表导出：`GET /tests/{id}/report?format=junit|html`，UI 详情页一键下载
- [x] 回调通知：`callback_url` 完成后 POST 摘要，指数退避重试，`callback.*` 事件与状态计数
- [x] 鉴权：`server.auth_token` Bearer Token（写操作 / 可选全保护），UI token 输入与 401 处理
- [x] 规范目录监控：轮询快照，外部变更自动重载概念并推送 `specs.reloaded`，UI 实时刷新
- [x] Runner 池：每个并发测试独占一个 Runner 进程真正并行，逐槽自愈，UI 展示每个进程

### P1 — 下一步（按优先级）

- [ ] **测试内并行**：把单个测试的场景拆到池中多个进程（Gauge `--parallel` 流语义），需要按进程隔离 suite/spec 钩子

### P2 — 增强

- [ ] 测试计划 / 定时任务（cron 表达式，周期性提交）
- [ ] 结果对比与趋势（同一规范历次通过率、耗时曲线）
- [ ] 步骤级重试策略（`retry: n` 标签或请求参数）
- [ ] UI：结果树搜索/只看失败、事件流暂停与导出、键盘快捷键
- [ ] UI：规范编辑器语法高亮与步骤自动补全（基于 `/runner/steps`）
- [ ] 多规范目录 / 多项目切换
- [ ] Windows 打包与服务安装脚本；Dockerfile

### P3 — 工程质量

- [ ] 压力测试脚本（并发连接、大结果树、长时间运行内存曲线）
- [ ] 覆盖率统计（gcov/llvm-cov）接入 CI
- [ ] 静态分析（clang-tidy）配置
- [ ] i18n：UI 文案抽离，提供英文界面

---

## 迭代记录

### 迭代 1 — 骨架（早期）

搭建项目结构、HTTP API 雏形与基本类型；HTTP 解析存在边界问题，Runner 依赖外部 gauge-cpp，未能独立编译。

### 迭代 2 — 自包含重构

- 移除 `../gauge-cpp` 依赖，新增 `util/json.h`、`spec/spec_parser.*`、`event/event_bus.*`、`engine/execution_engine.*`、`runner/*`
- CMake 重构：`testhub_core` 静态库 + 可执行文件 + 测试；`cmake/EmbedResources.cmake` 内嵌 `web/`
- 修复编译问题：缺失头文件、字符串拼接、`concept` 关键字冲突、`-Wformat-truncation`
- 决策：默认 `c++`（clang 18）在当前环境无法链接 libstdc++，文档统一给出 `-DCMAKE_CXX_COMPILER=g++`

### 迭代 3 — 服务端与 UI

- HTTP/1.1 服务器、RFC 6455 WebSocket、REST 路由、静态资源与 SPA 回退
- 原生 JS 单页应用（6 个页面）与 CLI（配置文件 + 参数覆盖 + daemon）
- 修复：`EmbedResources` 参数引号导致 `RELATIVE_PATH` 失败

### 迭代 4 — Python Runner 与协议

- `testhub_runner.py` 参考实现 + 17 个示例步骤 + 协议单测
- 修复：`step_impl` 以独立模块名导入 `testhub_runner` 导致注册表为空 → `sys.modules` 别名
- 修复：`runner/status` 的 `step_count` 为 0 → 桥接层缓存步骤列表

### 迭代 5 — 并发正确性

- 发现 `max_concurrent_tests > 1` 时不同测试的步骤会在有状态 Runner 中交错
- 引入 `Runner::isConcurrencySafe()` 与 `RunnerBridge::acquireSession()`，引擎在场景级别持有会话锁；`runner_` 改为 `shared_ptr` 避免重启期间悬空

### 迭代 6 — 测试体系

- 自带 `tests/test_framework.h`，51 个单元测试、7 个集成测试、ctest、GitHub Actions
- 测试暴露并修复：HEAD 返回 405、HTTP 流水线丢失第二个请求、`Json::operator[](int)` 对可变对象解析到 `const char*` 重载、解析器缺少"无场景"警告、`executedScenarios` 把跳过场景计入、`CHECK_EQ` 悬垂引用

### 迭代 7 — 浏览器实测与 UI 完善

- 用 Playwright 逐页实测：总览、提交表单、测试记录、详情、规范结构/源码/编辑、Runner、事件流
- 新增：运行中根据事件流构建实时执行树（规范 → 场景 → 步骤，含概念嵌套与运行中标记）；结果树用数据行替换 `<列>` 并悬浮显示原名；规范页标出 Runner 未实现的步骤并给出提示
- 修复：页面切换后旧监听器残留导致"取消"重复触发 4 次 → 每次导航替换 `#main` 节点；测试列表异步加载在切页后访问空节点；进度百分比与"当前步骤"不随进度刷新；跳过步骤/未运行场景显示 `0 ms`；结构视图多余 `-`；无行号错误显示 `L0:`；空标签时副标题多余 ` -`
- 引擎：取消/超时/hook 失败导致未运行的场景现在保留完整步骤列表（全部标记跳过），错误信息区分原因
- Runner：新增通用 `等待 <seconds> 秒` 步骤与 `specs/slow.spec`，用于演示实时监控、超时与取消
- 测试框架：`--filter` / `--list` 选项
- 文档：README / DESIGN / QUICKSTART / TODO 全部按实现重写

### 迭代 8 — 结果持久化

- 新增 `ResultStore`：终态记录写入 `<results_dir>/<test_id>.json`（tmp + rename），`start()` 回放到内存与统计，删除/清空/裁剪同步删文件，损坏文件跳过
- `json_convert.h` 补齐 `TestStatus` / `TestResult` 及嵌套结构的反序列化；`TimeUtil::fromIso8601`
- 配置 `execution.results_dir`（默认 `data/results`），CLI `--results-dir` / `--no-persist`；`.gitignore` 忽略 `data/`
- 修复：过滤后没有任何场景匹配的测试之前被标为 `passed`，现在为 `skipped` 并附警告（编写集成测试时用错标签暴露了此问题）
- 测试：ResultStore JSON 往返 + 引擎重启回放/裁剪单测；集成测试模拟服务器重启后列表、状态、结果、统计、重跑、删除文件

### 迭代 9 — 报表导出

- 新增 `report/report_writer.*`：JUnit XML（每规范一个 `testsuite`，每场景/数据行一个 `testcase`，失败 → `failure`、错误 → `error`、跳过/取消 → `skipped`，步骤列表与 Runner 消息写入 `system-out`，标签/环境/元数据写入 `properties`）与自包含 HTML（内联样式、失败项默认展开、概念嵌套、数据行参数替换）
- API：`GET /api/v1/tests/{id}/report?format=junit|xml|html[&download=1]`；未完成返回 409，未知格式 400；持久化的历史记录同样可导出
- UI：详情页新增"HTML 报告"（新标签页打开）与"JUnit XML"（下载）按钮
- 测试：5 个 ReportWriter 单测（结构、转义、控制字符剔除、空结果、HTML 自包含）；集成测试覆盖两种格式、Content-Disposition、409/400/404；`xmllint` 校验输出

### 迭代 10 — 完成回调

- 新增零依赖 `HttpClient`（`util/http_client.*`）：URL 解析、非阻塞 connect + 超时、Content-Length / chunked / 读到关闭三种正文，仅 `http://`
- 新增 `CallbackNotifier`（`notify/callback_notifier.*`）：订阅 `test.completed` / 排队阶段的 `test.cancelled`，投递线程按到期时间取任务；2xx 成功，网络错误/5xx/429 指数退避重试至 `max_attempts`；发布 `callback.delivered` / `callback.failed`；`GET /status` 暴露 `callbacks{pending,delivered,failed,attempts}`
- 载荷含请求元数据、场景计数、`failed_scenarios_detail`、`links`（`public_base_url` 前缀）；头部 `X-TestHub-Event/Test-Id/Attempt`
- 配置 `callbacks.*`；CLI `--public-url` / `--no-callbacks`；提交时校验 `callback_url`（非 http 返回 400）
- UI：提交表单新增回调 URL、详情页显示回调地址、总览"回调"计数、事件流着色并显示 `url/status/attempts`
- 测试：HttpClient 单测（解析、真实服务器 POST、404/204、超时、拒绝连接、https 拒绝）、载荷单测；集成测试用第二个 `HttpServer` 作接收端验证 503 → 重试成功与拒绝连接 → 3 次后放弃

### 迭代 11 — Bearer Token 鉴权

- `HttpServer::setRequestFilter()`：路由前统一过滤，WebSocket 升级同样经过；OPTIONS 预检放行
- `AuthPolicy`（`server/auth.h`）：写操作默认受保护，`auth_protect_reads` 扩展到 GET/HEAD/WS；凭据来自 `Authorization: Bearer`、`X-Auth-Token`，读操作与 WS 可用 `access_token` 查询参数；常量时间比较；401 + `WWW-Authenticate`
- 配置 `server.auth_token` / `server.auth_protect_reads`，CLI `--auth-token` / `--auth-protect-reads`，环境变量 `TESTHUB_AUTH_TOKEN`；`/config` 与 `--print-config` 掩码 token；启动日志提示鉴权状态；`/health` 暴露 `auth_required` / `auth_protect_reads`
- UI：侧栏"鉴权"状态按钮与 token 对话框（localStorage），请求自动附加头，401 自动弹窗、保存后重连 WS 并重载页面；报表链接与 WS 在全保护模式下携带 `access_token`
- 测试：AuthPolicy 单测（读写/路径/凭据来源/常量时间）、过滤器单测；集成测试覆盖两种模式下的 REST、预检、配置掩码、WS 拒绝/放行
- 浏览器实测：无 token → 弹窗 → 输入后页面与实时连接恢复；错误 token → "token 无效"提示

### 迭代 12 — 并发稳定性（CI 偶发崩溃排查）

- 起因：迭代 11 推送后 ubuntu/clang CI 的集成测试偶发 `terminate called without an active exception`；本地 60 次未复现，改用 ThreadSanitizer 定位
- 根因：`WebSocketServer::handleUpgrade` 先注册连接再给 `conn->reader` 赋值，客户端秒断时读线程已进入 `closeConnection` 且看到句柄"不可 join"，于是没有把自己交给 finished 列表；joinable 的 `std::thread` 随 `Connection` 析构触发 `std::terminate`。修复：句柄赋值与检查同锁（`readerMutex`），已结束的读线程在下次升级时回收而非只在 `stop()`
- 日志错位：`[ PASS ]` 写入全缓冲的 stdout、日志写入 stderr，异常终止吞掉了已通过的结果行，导致 CI 日志把崩溃"归咎"到 callback 测试；测试框架现在每条结果立即 flush
- 顺带修复两处丢失唤醒：`HttpServer::stop()` 与 `CallbackNotifier::stop()` 在无锁状态下改标志并 notify，等待方可能卡在 `join()`
- 顺带修复引擎竞态：终态先写入 `records_` 再落盘，轮询到终态的客户端可能读不到结果文件（本地循环 40 次复现 1 次）；现在先序列化副本再发布终态，取消路径同样处理
- 测试：新增 40 次 WS 秒连秒断的回归测试；WS 过滤测试改为等待 `subscribed` 确认后再提交；gcc/clang 各循环 60 次、TSan 全量运行零告警

### 迭代 13 — 规范目录监控

- 新增 `spec/spec_watcher.*`：后台线程按 `specs.watch_interval_ms`（默认 2 s）对规范目录与概念目录做 `{mtime,size}` 快照并 diff；新增/修改/删除合并为一个 `specs.reloaded`（`source=watcher`，含计数与文件清单）；`.cpt` 变化时重载概念字典
- 稳定窗口（200 ms）：刚写入的文件推迟一轮，避免解析编辑器写了一半的内容；API 写入后 `acknowledge()` 防止重复报告；`POST /specs/reload` 同时触发扫描并返回 `changes`
- 配置 `specs.watch` / `specs.watch_interval_ms`，CLI `--watch-interval` / `--no-watch`；`/status.spec_watcher` 暴露跟踪文件数、扫描/变更/重载次数与最近变更时间
- UI：规范页收到 watcher 事件时弹出"规范目录已变化：新增 1"并自动刷新列表；总览"服务"卡片新增"规范监控"行；事件流渲染 `source/files/created/updated/deleted`（零值隐藏）
- 测试：4 个单测（增删改检测与相对路径、概念重载、acknowledge 与稳定窗口、后台线程/禁用模式）+ 1 个集成测试（外部写入/删除被运行中的服务器发现、`/specs` `/concepts` 同步、API 写入不重复报告、状态计数、手动 reload 的 changes）
- 浏览器实测：打开规范页 → 终端写入 `watch-demo.spec` → 列表自动出现新行；删除后自动消失；总览显示"每 1 s · 5 个文件 · 2 次变更"

### 迭代 14 — Runner 池

- `RunnerBridge` 重写为 Runner 池：`runner.pool_size` / `--runner-pool`（默认 0 = 跟随 `-j`）个槽位，每个槽位一个独立 Runner 进程；`acquireSession()` 改为 RAII `Session`，独占槽位并用 `thread_local` 绑定到当前线程，此后该线程的步骤/钩子都落在绑定的进程上
- 引擎把会话从"场景级"提升到"测试级"：`executeTest` 开头申请进程并持有到结束，一个测试 = 一个进程（before_suite … after_suite 同进程），不同测试真正并行；没有空闲进程时测试保持 `queued`
- 逐槽自愈：每个槽位独立的 `restart_count` 预算，重启在锁外进行不阻塞其他槽位；分配时优先在线槽位，其次可重启槽位，全部永久失效才返回明确错误而非无限等待；并发安全的 mock 自动收缩为 1 个共享槽位
- `stop/restart` 先排空（新会话等待）再并行停止/拉起全部进程；`/runner/status` 新增 `pool_size / alive / busy` 与 `runners[]` 明细；`runner.*` 事件携带 `slot`；子进程可读 `TESTHUB_RUNNER_INDEX` / `TESTHUB_RUNNER_POOL_SIZE`
- **中途放弃的方案**：最初把 suite/spec 级钩子设计成"在池中每个进程上各执行一次"（Gauge 语义），实测两个 0.8 s 的测试总耗时 1.6 s——广播时要依次等待每个被占用的进程，测试之间实际上被串行化了。改为测试级会话后总耗时 < 1 s
- **顺带发现并修复的死锁**：Runner 命令经 `sh -c` 启动，`kill -9` 掉 `sh` 后真正的 python 进程成为孤儿并继续持有管道，读线程永远等不到 EOF，`ProcessRunner::stop()` 在 `join()` 上永久阻塞——槽位卡在 `connecting`、`TestHub::stop()` 也无法退出（单 Runner 时代同样存在）。修复：子进程启动即自成进程组，`terminate()` 无论直接子进程是否已退出都清理整个进程组
- UI：总览 Runner 卡片显示"3 个进程 · 3 在线 · 2 忙碌"与每进程小方块（颜色=状态，悬停看 PID/步骤数/重启数/错误）；Runner 页新增每进程表格；事件流显示 `slot`
- 测试：7 个单测（注入假 Runner：并行分配与会话绑定、阻塞等待与 busy 状态、逐槽重启、每槽重启预算与健康槽位优先、会话内钩子绑定、并发安全收缩、停止等待与重启替换）+ 1 个真实 Python 池集成测试（两进程并行 < 1.5 s、`kill -9` 后按需自愈且孤儿进程组消失、手动重启替换全部进程）；gcc/clang/TSan 全绿，35 轮循环零失败
- 浏览器实测：`-j 3` 提交 3 个 5 s 的 `slow.spec`，三个测试 5.00 / 5.00 / 5.01 s 同时完成（之前串行为 7 s / 10 s）；`kill -9` 一个进程后再提交 3 个并发测试，该槽位重启（restart 1）、全部通过

### 迭代 15 — Node.js 参考 Runner

- `runners/node/testhub_runner.js`：零 npm 依赖的 CommonJS 实现，协议与 Python Runner 完全一致；`step()` / `beforeScenario()` 等注册 API，`Messages` / `DataTable` / `SkipStep` / `dataStore`；步骤函数可以是 `async`，消息严格按顺序 `await`
- `require('testhub-runner')` 通过 `Module._resolveFilename` 别名解析到本文件，步骤实现无需 npm 安装；`console.log` 重定向到 stderr，不污染协议通道
- 示例 `step_impl/login_steps.js` 与 Python 版一一对应（含 `等待 <seconds> 秒` 的 async 实现），同一套 `specs/` 可互换执行
- `RunnerBridge::defaultCommandForLanguage("node"|"js"|"javascript"|"nodejs")` 自动定位捆绑脚本；CMake `find_program(node)` 注册 `node_runner_protocol`；CI 安装 Node.js 22 并独立跑协议测试
- 测试：12 个协议用例（ping/get_steps/场景通过/断言失败/缺实现/表格/async 等待/dataStore 清空/未知类型/非法 JSON/缺失目录/kill 退出 0）+ 1 个集成测试（login+calculator+checkout 12 场景全部通过、失败场景带回 AssertionError 堆栈、手动重启换 PID）
- 实测：`--language node --dir runners/node -j 3` 三个含 5 s async 等待的测试 5.02 / 5.03 / 5.02 s 同时完成，每进程各执行 75 步；UI Runner 页显示语言 `node`、版本 `node-1.0`、三进程池

---

## 决策记录

| 日期 | 决策 | 原因 / 权衡 |
|------|------|-------------|
| 迭代 2 | 不引入任何第三方 C++ 库 | 单一可执行文件、跨平台构建简单；代价是自行维护 HTTP/WS/JSON，需要充分测试 |
| 迭代 2 | 放弃 Gauge gRPC 协议，改用 JSON-lines over stdio | 无需 Protobuf/gRPC 依赖，任何语言几十行即可实现 Runner；Gauge 现有插件不能直接复用 |
| 迭代 3 | 前端使用原生 JS，不引入框架与构建工具 | 资源直接内嵌，`--web-dir` 即可热改；页面复杂度可控 |
| 迭代 5 | 并发粒度定为"场景"而非"步骤"或"测试" | 步骤级会让有状态 Runner 交错；测试级会让并发形同虚设；场景级兼顾正确性与吞吐 |
| 迭代 6 | 自写迷你测试框架而非引入 Catch2/GTest | 保持零依赖；需求简单（TEST_CASE/CHECK/REQUIRE 足够） |
| 迭代 7 | 运行中视图由事件流在前端构建，而非服务端提供部分结果 | 复用既有事件，无需新增 API；结果落地后以服务端结果树为准 |
| 迭代 8 | 持久化采用“一测试一 JSON 文件”而非 SQLite | 零依赖、可直接用文本工具查看/备份、与 API 输出同构；查询需求复杂时再引入数据库 |
| 迭代 9 | 报表按需渲染而非随结果落盘 | 结果 JSON 已是事实来源，报表只是视图；避免多份文件不一致，格式演进无需迁移 |
| 迭代 10 | 回调客户端自研且仅支持 http:// | 保持零依赖；HTTPS 需要 TLS 库，交给反向代理/内网中转更符合守护进程的部署形态 |
| 迭代 11 | 单一共享 token，默认只保护写操作 | 守护进程多部署在内网/CI；先解决"误操作与未授权写入"，读保护按需开启；多用户/角色留待有明确需求时再做 |
| 迭代 13 | 目录监控用轮询快照而非 inotify/FSEvents | 零依赖、三平台同一实现、无需处理事件合并与队列溢出；规范目录规模小，秒级轮询开销可忽略；稳定窗口天然解决编辑器分步写入 |
| 迭代 14 | Runner 池的分配粒度从"场景"改为"测试"（推翻迭代 5 的结论） | 迭代 5 只有一个进程，场景级是在"串行"里争取交错；有了多进程后，测试级让每个测试独占进程：suite/spec 钩子天然只在自己的进程上执行、无需广播、不同测试互不干扰；代价是 `pool_size < -j` 时多余 worker 空等，而默认池大小跟随 `-j` 消除了这一情形 |
| 迭代 14 | 进程存活检查按需进行（分配时），不加心跳线程 | 每次分配/每步执行前都会 `waitpid(WNOHANG)`，成本可忽略；崩溃的进程在下次使用时重启，UI 报告"will be restarted on next use"；额外的心跳线程只会更早发现但不会更早需要它 |
| 迭代 15 | Node.js Runner 用 CommonJS + 模块解析别名，不引入 npm 包 | 保持仓库零第三方依赖；`require('testhub-runner')` 解析到捆绑脚本即可；async/await 是 Node 自带能力，用来验证协议对异步步骤的等待语义 |

---

## 质量指标

| 指标 | 当前 |
|------|------|
| 编译警告（`-Wall -Wextra -Wpedantic -Werror`） | 0（GCC 13、Clang 18） |
| 自动化测试 | 109 个，全部通过（74 单元 + 16 集成 + 7 Python 协议 + 12 Node 协议）；`ctest` 约 5 s；TSan 零告警 |
| 健康检查响应 | < 1 ms（本机） |
| 空载内存 | 约 7 MB（不含 Runner 子进程） |
| 代码规模 | 约 14k 行（C++ 约 10.2k，前端约 1.2k，Python 约 0.7k，测试约 2.4k） |
