# TestHub 开发任务追踪

本文件记录路线图、迭代记录与决策。状态：`[x]` 完成、`[ ]` 待办、`[~]` 进行中。

## 当前状态（v1.1.0）

- 自包含 C++17 项目，零第三方依赖，`-Werror` 零警告（GCC / Clang）
- 78 个自动化测试全部通过（61 单元 + 10 集成 + 7 Python 协议），GitHub Actions 三平台 CI
- 约 12k 行（含前端、Python Runner、测试）

## 路线图

### P0 — 已完成的基础能力

- [x] 仓库自包含：移除对外部 gauge-cpp 的依赖、清理 `build/` 产物、`.gitignore`、CMake 重构
- [x] 零依赖 JSON（解析/序列化/下标访问/错误定位）
- [x] `.spec` / `.cpt` 解析器（标题、标签、上下文、清理、数据表、参数、概念、行号、错误与警告）
- [x] HTTP/1.1 服务器（Content-Length、keep-alive、流水线、超时、`{param}` 路由、HEAD/OPTIONS/405、CORS、ETag 静态资源、SPA 回退）
- [x] 执行引擎（优先级队列、标签表达式、场景过滤、数据驱动、上下文/清理、超时、取消、fail_fast、重跑、历史）
- [x] Runner 抽象 + 子进程桥接（JSON-lines、心跳、自动重启、场景级会话锁、步骤缓存）
- [x] Python 参考 Runner（装饰器、钩子、DataTable、Messages、SkipStep、data_store）+ 示例步骤实现
- [x] 异步事件总线 + WebSocket 推送（订阅过滤、历史回放）
- [x] 内嵌 Web UI（总览、提交、测试记录、结果树、实时执行树、规范浏览/编辑/校验、Runner、事件流）
- [x] 测试体系（自带迷你框架、单元 + 集成 + 协议测试、ctest、CI）
- [x] 文档同步（README / DESIGN / QUICKSTART / TODO）
- [x] 结果持久化：终态记录 JSON 落盘（`data/results/`），启动回放历史与统计，随 history_limit 裁剪
- [x] 报表导出：`GET /tests/{id}/report?format=junit|html`，UI 详情页一键下载
- [x] 回调通知：`callback_url` 完成后 POST 摘要，指数退避重试，`callback.*` 事件与状态计数

### P1 — 下一步（按优先级）

- [ ] **鉴权**：`server.auth_token`，Bearer Token 校验写操作；UI 支持输入 token
- [ ] **Runner 池**：`max_concurrent_tests > 1` 时启动多个 Runner 进程真正并行（当前为场景级串行）
- [ ] **Node.js 参考 Runner**：复用 JSON-lines 协议，验证语言无关性
- [ ] **规范目录监控**：文件变更自动 reload 并推送 `specs.reloaded`

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

---

## 质量指标

| 指标 | 当前 |
|------|------|
| 编译警告（`-Wall -Wextra -Wpedantic -Werror`） | 0（GCC 13、Clang 18） |
| 自动化测试 | 78 个，全部通过；`ctest` 约 2 s |
| 健康检查响应 | < 1 ms（本机） |
| 空载内存 | 约 7 MB（不含 Runner 子进程） |
| 代码规模 | 约 13.5k 行（C++ 约 10k，前端约 1.1k，Python 约 0.7k，测试约 2.2k） |
