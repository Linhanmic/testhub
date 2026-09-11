# TestHub 开发任务追踪

本文件记录路线图、迭代记录与决策。状态：`[x]` 完成、`[ ]` 待办、`[~]` 进行中。

## 当前状态（v1.1.0）

- 自包含 C++17 项目，零第三方依赖，`-Werror` 零警告（GCC / Clang）
- 约 15k 行（含前端、Python / Node.js Runner、测试）
- 136 个自动化测试全部通过（95 单元 + 22 集成 + 7 Python 协议 + 12 Node.js 协议），GitHub Actions 三平台 CI；ThreadSanitizer 零告警

## 路线图

### P0 — 已完成的基础能力

- [x] 仓库自包含：移除对外部 gauge-cpp 的依赖、清理 `build/` 产物、`.gitignore`、CMake 重构
- [x] 零依赖 JSON（解析/序列化/下标访问/错误定位）
- [x] `.spec` / `.cpt` 解析器（标题、标签、上下文、清理、数据表、参数、概念、行号、错误与警告）
- [x] HTTP/1.1 服务器（Content-Length、keep-alive、流水线、超时、`{param}` 路由、HEAD/OPTIONS/405、CORS、ETag 静态资源、SPA 回退）
- [x] 执行引擎（优先级队列、标签表达式、场景过滤、数据驱动、上下文/清理、超时、取消、fail_fast、步骤重试、重跑、历史）
- [x] Runner 抽象 + 子进程桥接（JSON-lines、崩溃检测、自动重启、步骤缓存）
- [x] Python 参考 Runner（装饰器、钩子、DataTable、Messages、SkipStep、data_store）+ 示例步骤实现
- [x] Node.js 参考 Runner（同一 JSON-lines 协议、async 步骤、零 npm 依赖）+ 示例步骤实现；同一套 .spec 可互换执行
- [x] 异步事件总线 + WebSocket 推送（订阅过滤、历史回放）
- [x] 内嵌 Web UI（总览、提交、测试记录、测试计划、结果树、实时执行树、规范浏览/编辑/校验、Runner、事件流）
- [x] 测试体系（自带迷你框架、单元 + 集成 + 协议测试、ctest、CI）
- [x] 文档同步（README / DESIGN / QUICKSTART / TODO）
- [x] 结果持久化：终态记录 JSON 落盘（`data/results/`），启动回放历史与统计，随 history_limit 裁剪
- [x] 报表导出：`GET /tests/{id}/report?format=junit|html`，UI 详情页一键下载
- [x] 回调通知：`callback_url` 完成后 POST 摘要，指数退避重试，`callback.*` 事件与状态计数
- [x] 鉴权：`server.auth_token` Bearer Token（写操作 / 可选全保护），UI token 输入与 401 处理
- [x] 规范目录监控：轮询快照，外部变更自动重载概念并推送 `specs.reloaded`，UI 实时刷新
- [x] Runner 池：每个并发测试独占一个 Runner 进程真正并行，逐槽自愈，UI 展示每个进程
- [x] 自举验证：绑定端口后注入 `TESTHUB_URL` / `TESTHUB_TOKEN`；`specs/selfcheck.spec` 经 Python / Node 步骤实现调用本进程 HTTP API（健康、状态 URL、Runner 在线、规范列表、提交子测试）；步骤不轮询子测试以免 `-j 1` 死锁

### P2 — 增强

- [x] 测试计划 / 定时任务（cron 表达式，周期性提交）
- [x] 结果对比与趋势（同一规范历次通过率、耗时曲线）
- [x] 步骤级重试策略（`retry: n` 标签或请求参数）
- [x] UI：结果树搜索/只看失败、事件流暂停与导出、键盘快捷键
- [x] UI：规范编辑器语法高亮与步骤自动补全（基于 `/runner/steps`）
- [x] 多规范目录 / 多项目切换
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

### 迭代 16 — 测试内并行流

- 请求字段 `parallel_streams`（默认 1，上限 64）生效：引擎先解析规范再 `reserveSlots(N)` 原子预约 N 个 Runner 槽位，把场景（含数据驱动的每一行）轮询分到 N 条流
- 每条流在自己的进程/线程上执行 `before_suite` → 所负规范的 `before_spec` / 场景 / `after_spec` → `after_suite`；结果按原始顺序回填。mock 等并发安全 Runner 不按池大小封顶（共享槽位上真正重叠）
- 预约策略：等到同时有 N 个空闲才一次性占用，避免“先拿 1 再等其余”与其它测试互相死锁；空闲不足时测试保持 `queued`
- UI：提交表单增加“并行流”，详情页展示；事件带 `stream`
- 测试：2 个引擎单测（两个 400 ms 场景 403 ms 完成且顺序为甲/乙；streams=1 回归）+ `reserveSlots` 单测 + 1 个真实 Python 集成测试（单 worker、池大小 2、一个测试的两个 0.8 s 场景 903 ms 完成）

### 迭代 17 — 自举验证

- `TestHub::start()` 先绑定 HTTP 端口，再把 `TESTHUB_URL`（`public_base_url` 或 `http://127.0.0.1:<boundPort>`）和可选 `TESTHUB_TOKEN` 写入 `execution.environment` 与 Runner 子进程环境，然后才启动 Runner。`GET /status` 与 `server.started` 携带 `url`
- `specs/selfcheck.spec` + `runners/*/step_impl/api_steps.*`：用本进程 HTTP API 检查健康、状态 URL、规范列表、Runner 在线、以及存在运行中的测试；再 POST 提交 `calculator.spec`（只断言 202，不在步骤里 `wait`，避免 `-j 1` 死锁）
- `GET /status` / `/health` 不再向 Runner 发 `get_steps`：JSON-lines 一次一条，步骤执行中再查询步骤列表会与步骤里访问本进程 HTTP 互相等待。改为握手后预热缓存，状态接口只读缓存
- 自举运行时当前槽位为 `busy` 而非空闲时的 `connected`，步骤「Runner 应在线」同时接受这两种可用状态
- 测试：规范解析覆盖自举文件的引号参数；Python 集成测试跑完整 selfcheck + 子测试；Node 端到端用例末尾同样跑一遍

### 迭代 18 — 步骤级重试

- 请求字段 `step_retry`（默认 0，上限 5）与规范/场景标签 `retry:N` / `retry-N` 取较大值，作用在该场景的每一步
- 只重试 `FAILED`（断言失败）；`TEST_ERROR`、取消、超时不重试。概念的内层步骤各自独立重试
- `StepResult.attempts`（1 = 未重试）写入结果 JSON / 持久化 / JUnit 文本与 HTML；事件 `step.retry`（`attempt`、`max_retries`、`error`）+ 最终 `step.completed` 带 `attempts`
- Mock Runner：步骤文案含 `flaky` 时该文案第一次失败、之后通过（计数加锁，因 `isConcurrencySafe`）
- UI：提交表单「步骤重试」；详情请求栏与结果树在 `attempts>1` 时显示 ×N；实时执行树处理 `step.retry`
- 测试：1 个引擎单测（无重试失败 / `step_retry=1` 与 `retry:1` 通过且 attempts=2 / TEST_ERROR 不重试 / 用尽 attempts=3 / JSON 越界拒绝）+ 集成 400 与 mock flaky 端到端

### 迭代 19 — 结果树过滤与事件流操作

- 测试详情：结果树（含实时树）可按场景/步骤文本搜索，以及「只看失败」隐藏 `passed` 场景与步骤；过滤使用 `hidden="until-found"`，浏览器「页内查找」仍能命中并展开（`beforematch` 清空过滤条件）；不支持 `until-found` 时退回 `hidden` 布尔属性
- 详情与事件流页：暂停缓冲实时事件（按钮显示待刷新条数）、导出 JSON；`p` 切换暂停；暂停中的 feed 用虚线描边与浅底提示
- 键盘：`?` 快捷键说明（原生 `<dialog>`）、`/` 聚焦搜索、`f` 只看失败、`g` 后接 `d/r/t/s/n/e` 跳转页面；SPA 更新 `document.title` 并在换页后聚焦 `<main>`；对话框打开或输入框聚焦时不拦截（`Escape` 除外）
- 无新增 C++ 测试；行为由浏览器实测覆盖（只看失败 1/3、搜索 `another` 1/3、暂停期间 24 条不入 DOM、导出 51 条 JSON 含新测试、`g` `e` 进入事件流）

### 迭代 20 — 测试计划 / UTC cron 调度

- 五字段 UTC cron（`src/util/cron.h`）：`*` `,` `-` `/`、月份/星期名称、`@hourly` 等别名；DOM+DOW 均非 `*` 时按 Vixie 二者满足其一。`nextAfter` 跳过当前分钟
- `Scheduler`：JSON 落盘到 `data/schedules/`（tmp+rename）；后台轮询 `tick(now)` 可注入时间；同一分钟只触发一次（持久化 `last_fired_minute`）；仅当 cron 文本变化时才重置该标记，避免 PUT 只改 `enabled` 导致同一分钟再发
- `skip_if_running` 默认 true：上一 `last_test_id` 仍 queued/running 则跳过并发 `schedule.skipped`。提交 `submitted_by=schedule:<id>`，`metadata.schedule_id`
- API：`GET/POST /schedules`、`GET/PUT/DELETE /schedules/{id}`、`POST /schedules/{id}/run`（202 / 409）。CLI `--schedules-dir` / `--no-scheduler`；`GET /status` 含调度器统计
- UI：导航「测试计划」、列表（启用开关 / 立即运行 / 编辑 / 删除 / 搜索）、创建表单（cron 模板、规范多选、高级选项）；总览服务卡链接；事件 `schedule.*` 着色；快捷键 `g` `c`
- 测试：cron 解析 4 + 调度器 4（含 enable-only 不重置）单元；集成 CRUD / 非法 cron / 立即运行 / 停用。合计 125（86+20+7+12）

### 迭代 21 — 结果对比与趋势

- `src/engine/trends.h` 纯函数：`groupTrends` 按规范聚合（空过滤先输出 `(all)`，再按文件名；截最近 `limit` 次、时间升序）；`compareScenarios` 以 `spec + 场景名 + 数据行号` 为 key，分类 `regressed` / `improved` / `still_failed` / `unchanged` / `added` / `removed`（FAILED 与 TEST_ERROR 都算失败）
- 引擎：`trendRuns()` 按提交顺序收集终态（跳过取消）；`compareTests` 在 `with` 为空时从新到旧找规范集合有交集的上一终态（空集合视为与任何集合重叠）
- API：`GET /api/v1/trends?spec=&limit=`（默认 50、上限 200）；`GET /api/v1/tests/{id}/compare?with=`（缺基线 404，自己比自己 400）
- UI：导航「结果趋势」、总览 sparkline、详情「与上次对比」；SVG 曲线用 `<figure>` + `role="img"` + 数据表，无 Chart.js；快捷键 `g` `a`
- 测试：趋势纯函数 3 + 引擎自动基线 1 单元；集成两次 login.spec 后 trends/compare。合计 130（90+21+7+12）
- 浏览器实测：总览「通过率趋势」sparkline；`#/trends` 按规范卡片（通过率/耗时曲线 + 点数表），筛选 `login.spec` 只留一张；详情「与上次对比」对 trend-demo 修复后显示改善 1 / 未变 1；快捷键 `g` `a` 进入趋势页

### 迭代 22 — 规范编辑器高亮与步骤补全

- 编辑页用透明 textarea 叠在 `<pre>` 上做 Gauge 语法高亮（标题/步骤/标签/表格），与源码视图共用 `highlightSpec`
- 补全目录来自 `GET /runner/steps`（`text`）与 `GET /concepts`（概念 heading）；在 `*` 步骤行过滤，Ctrl+Space 或「步骤补全」打开 listbox，Enter/点击插入
- mock Runner 不报告步骤时仍可补全概念；Python/Node Runner 提供完整步骤列表
- 浏览器实测（Python Runner `:18084`）：编辑 `login.spec` 时 `#`/`##`/`tags:` 着色；hint「可补全 26 个 Runner 步骤、1 个概念」；输入 `* 输入` 过滤出「输入用户名/密码/第一个数/第二个数」；点击插入 `* 输入用户名 <name>` 且 listbox 关闭；Esc 关闭补全；源码 tab 行号 + 同类高亮；未点保存，磁盘上的 `specs/login.spec` 未改

### 迭代 23 — 多规范目录 / 项目切换

- 配置 `specs.projects` + `specs.current`；缺省从 `specs.dir` 合成 `default` 项目。`--specs` 匹配已有 `dir` 则选中，否则改写当前项目目录
- API：`GET /api/v1/projects`；`POST /api/v1/projects/{id}/select`（找不到 404；排队或运行中 409）。切换后重配 SpecRepository、重载概念、`acknowledge` 监控快照，发布 `project.changed` 与 `specs.reloaded`（source=project）
- Runner 仍全局共享：项目只切换规范/概念根目录，不重启步骤实现进程
- UI：侧栏原生 `<select>`；单项时禁用以免误触；详情 hash 在切换后回到规范列表
- 测试：单元 5（合成/JSON current/slug 去重/CLI 覆盖/往返）+ 集成切换与 409。自举增加 `GET /projects` count=1 与 `current_project=default`
- 示例：`examples/alt-specs/hello.spec`
- 浏览器实测（Python Runner `:18086`）：侧栏「主规范 / 备用示例」；规范页 6 个文件；切到备用后只剩 `hello.spec`，副标题为 `examples/alt-specs · 项目 alt`；总览显示项目「备用示例」

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
| 迭代 16 | 并行流采用“凑齐 N 个槽位再开工”，不降级为更少的流 | 降级会让同一测试的 suite 钩子只跑在部分进程上，语义随池占用情况漂移；排队等齐更可预期。数据驱动的每一行当作独立场景分片 |
| 迭代 17 | 自举步骤不轮询子测试；Runner「应在线」接受 connected/busy；状态接口不向忙进程发 get_steps | 步骤里 `wait` 子测试会在 `-j 1` 时占满唯一 worker 造成死锁；自举过程中当前槽位必然是 busy；JSON-lines 同步协议下 get_steps 与 execute_step 不能重叠 |
| 迭代 18 | 只重试 FAILED，不重试 TEST_ERROR / 取消 / 超时；请求与标签取 max，上限 5 | 缺实现、崩溃、超时再跑一遍通常无意义；断言抖动才适合有限次重试。标签可按场景覆盖全局请求，避免误伤稳定用例 |
| 迭代 19 | 过滤用 `hidden="until-found"` 而非从 DOM 删除；快捷键在输入框与对话框内不拦截 | 页内查找仍能发现被「只看失败」藏起的通过步骤；避免在表单或快捷键说明里误触 `g`/`f`/`/` |
| 迭代 22 | 编辑器高亮用叠加层而非 contenteditable；补全同时收录 Runner 步骤与概念 | contenteditable 难与原生撤销/选区/读屏共存；mock 不报告步骤时概念仍能补全 |
| 迭代 23 | 项目只切换规范/概念目录，Runner 保持全局 | 「多套用例、同一套步骤实现」是主场景；换 Runner 要重启进程池且不能与运行中的测试并存，留给以后按项目覆盖 runner 字段 |

---

## 质量指标

| 指标 | 当前 |
|------|------|
| 编译警告（`-Wall -Wextra -Wpedantic -Werror`） | 0（GCC 13、Clang 18） |
| 自动化测试 | 136 个，全部通过（95 单元 + 22 集成 + 7 Python 协议 + 12 Node 协议）；`ctest` 约 7 s；TSan 零告警 |
| 健康检查响应 | < 1 ms（本机） |
| 空载内存 | 约 7 MB（不含 Runner 子进程） |
| 代码规模 | 约 14k 行（C++ 约 10.2k，前端约 1.2k，Python 约 0.7k，测试约 2.4k） |
