# TestHub 快速开始与开发指南

面向开发者：如何构建、运行、测试与扩展 TestHub。使用说明见 [README.md](README.md)，设计文档见 [DESIGN.md](DESIGN.md)。

## 1. 环境准备

| 依赖 | 版本 | 说明 |
|------|------|------|
| C++ 编译器 | GCC 9+ / Clang 10+ / MSVC 2019+ | 需要完整 C++17 |
| CMake | 3.16+ | |
| Python | 3.8+（可选） | 运行 Python 参考 Runner 及其协议测试 |
| Node.js | 16+（可选） | 运行 Node.js 参考 Runner 及其协议测试；也用于 `node --check web/app.js` |

没有第三方 C++ 依赖，无需包管理器。

## 2. 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug          # 首次配置
cmake --build build -j                                 # 编译（web/ 资源会自动内嵌）
ctest --test-dir build --output-on-failure             # 运行全部测试
```

常用变量：

| 变量 | 默认 | 说明 |
|------|------|------|
| `CMAKE_CXX_COMPILER` | 系统默认 | 若默认 `c++` 不可用可指定 `g++` / `clang++` |
| `TESTHUB_BUILD_TESTS` | ON | 是否构建 `tests/` |
| `TESTHUB_WARNINGS_AS_ERRORS` | OFF | CI 中开启，`-Wall -Wextra -Wpedantic -Werror` |

测试目标：

- `testhub_unit_tests` — JSON、规范解析器、标签过滤、队列、事件总线、HTTP、WebSocket 帧、执行引擎（使用 mock Runner）
- `testhub_integration_tests` — 启动真实服务器（端口 0），用原生 TCP 客户端验证 REST、流水线、并发、WebSocket 事件流、规范 CRUD、取消
- `python_runner_protocol` — 以子进程方式启动 Python Runner，验证 JSON-lines 协议
- `node_runner_protocol` — 以子进程方式启动 Node.js Runner，验证同一协议（含 async 步骤）

```bash
./build/tests/testhub_unit_tests --filter "spec:"      # 只跑名称包含 spec: 的用例
./build/tests/testhub_integration_tests --list
python3 runners/python/test_runner_protocol.py -v
node runners/node/test_runner_protocol.js
```

## 3. 运行

```bash
./build/testhub --port 8080 --log-level debug                       # mock Runner
./build/testhub --port 8080 --language python --dir runners/python  # Python Runner
./build/testhub --port 8080 --language node --dir runners/node      # Node.js Runner（同一套 specs）
./build/testhub --port 8080 --web-dir web                            # 前端开发：直接读取 web/ 目录，改完刷新即生效
```

验证：

```bash
curl http://localhost:8080/api/v1/health
curl -X POST http://localhost:8080/api/v1/tests -H 'Content-Type: application/json' -d '{"spec_files":["calculator.spec"]}'
curl http://localhost:8080/api/v1/tests?limit=5
curl "http://localhost:8080/api/v1/trends?spec=login.spec"
curl "http://localhost:8080/api/v1/tests/<id>/compare"

# 自举：用 .spec 经 HTTP API 验证本进程（需要 python/node Runner，不要用 mock）
curl -X POST http://localhost:8080/api/v1/tests \
  -H 'Content-Type: application/json' \
  -d '{"spec_files":["selfcheck.spec"]}'
```

浏览器打开 <http://localhost:8080/>。

## 4. 代码地图

```
src/main.cpp               解析 CLI / 配置文件，daemonize，信号处理
src/testhub.{h,cpp}        TestHubConfig（applyJson/toJson）+ TestHub 门面（start/stop，装配各组件）
src/server/http_server.*   HTTP/1.1 服务器：accept 轮询 + 工作线程池，路由表，静态资源，升级钩子
src/server/api_routes.cpp  TestHub::registerApiRoutes() — 全部 REST 端点
src/server/auth.h          AuthPolicy — Bearer Token 校验（HttpServer 请求过滤器）
src/server/web_ui.cpp      TestHub::registerWebUi() — 内嵌资源或 --web-dir，SPA 回退
src/server/websocket_*     RFC 6455 握手/帧编解码，订阅过滤，EventBus 转发
src/engine/execution_engine.*  提交 → 队列 → worker → 规范/场景/步骤执行 → 结果与事件
src/engine/trends.h        结果趋势聚合与场景级对比（纯函数）
src/engine/scheduler.*     UTC cron 测试计划（轮询 tick、JSON 落盘）
src/engine/result_store.*  结果 JSON 落盘与启动回放
src/report/report_writer.* JUnit XML / HTML 报表渲染
src/notify/callback_notifier.* callback_url 完成回调（队列、重试、事件）
src/util/http_client.*     零依赖 HTTP 客户端（仅 http://，用于回调）
src/engine/test_queue.h    优先级队列（priority + 提交时间）
src/engine/tag_filter.h    标签表达式解析（& | ! 括号）
src/runner/runner.h        Runner 抽象接口（start/stop/getAllSteps/executeStep/runHook/isConcurrencySafe）
src/runner/process_runner.* POSIX/Windows 子进程 + JSON-lines
src/runner/runner_bridge.*  Runner 池：槽位分配、测试级会话（thread_local 绑定）、逐槽自愈、状态聚合、步骤缓存
src/spec/spec_parser.*     .spec/.cpt 解析器，ConceptDictionary
src/spec/spec_repository.* 规范目录扫描、读取、写入、校验
src/spec/spec_watcher.*    规范目录轮询监控（自动重载概念、推送 specs.reloaded）
src/util/cron.h            五字段 UTC cron 解析（别名、Vixie DOM/DOW）
src/event/event_bus.*      单例异步事件总线，历史环形缓冲，通配订阅
src/model/types.h          TestRequest/TestStatus/TestResult/StepResult/Event 等
src/model/json_convert.h   模型 ↔ Json
src/util/json.h            零依赖 JSON（解析/序列化/下标访问）
web/                       前端：index.html + app.js（hash 路由 SPA）+ app.css
runners/python/            testhub_runner.py（协议实现 + 装饰器 API）、step_impl/、test_runner_protocol.py
runners/node/              testhub_runner.js（同一协议 + async 步骤）、step_impl/、test_runner_protocol.js
```

## 5. 常见任务

### 新增 API 端点

在 `src/server/api_routes.cpp` 的 `registerApiRoutes()` 中注册：

```cpp
http.get("/api/v1/hello/{name}", [this](const HttpRequest& req) {
    Json j = Json::object();
    j["message"] = "Hello, " + req.param("name");
    return HttpResponse::json(200, j);
});
```

支持 `get/post/put/del`，路径中 `{param}` 通过 `req.param()` 读取，`req.query()` 读取查询参数，`req.body` 为原始正文；用 `HttpResponse::error(code, message)` 返回统一错误格式。别忘了在 `tests/test_integration.cpp` 增加用例，并更新 README 的端点表。

### 新增事件类型

1. 在 `src/model/types.h` 的 `EventType` 命名空间添加常量；
2. 在引擎中 `publish(EventType::X, testId, {{"key", "value"}})`；
3. 前端 `web/app.js` 的 `renderEvent()` / `createLiveTree()` 按需处理。

### 修改 Web UI

前端是无构建步骤的原生 JS。用 `--web-dir web` 启动即可热改；提交前运行 `node --check web/app.js` 并重新 `cmake --build build`，让 `cmake/EmbedResources.cmake` 重新生成 `web_assets.cpp`。规范详情的「编辑」页在 textarea 下叠一层语法高亮，并按 `GET /runner/steps` 与概念做步骤补全（`Ctrl+Space`）。

### 编写 Runner

任何语言均可：从 stdin 逐行读取 JSON 消息，向 stdout 逐行写 JSON 响应（日志请写 stderr）。最小消息集：

```
→ {"type":"ping"}                      ← {"type":"pong","version":"my-runner-1.0"}
→ {"type":"get_steps"}                 ← {"type":"steps","steps":[{"text":"打开 <page>","parameterized_text":"打开 {}","params":["page"]}]}
→ {"type":"execute_step","id":"1","step_text":"打开 \"首页\"","parameterized_text":"打开 {}","args":[{"type":"static","value":"首页"}],"context":{...}}
                                       ← {"type":"step_result","id":"1","status":"passed","duration_ms":3,"messages":["..."]}
→ {"type":"hook","id":"2","hook":"before_scenario","context":{...}}
                                       ← {"type":"hook_result","id":"2","status":"passed"}
→ {"type":"kill"}                      （进程退出）
```

完整协议见 [DESIGN.md §5.3](DESIGN.md#53-runner-通信协议)。参考实现：`runners/python/testhub_runner.py` 与 `runners/node/testhub_runner.js`（`require('testhub-runner')` 无需 npm 安装）。用 `--language python` / `--language node`，或 `--language custom --runner-cmd "node my_runner.js"` 接入。

并发（`-j N`）时 TestHub 默认启动 N 个 Runner 进程组成池，每个测试默认独占一个进程，因此 Runner 只需处理串行请求、不必线程安全；进程可通过环境变量 `TESTHUB_RUNNER_INDEX` / `TESTHUB_RUNNER_POOL_SIZE` 区分自己（例如为每个进程分配独立的浏览器 profile 或端口）。提交测试时把 `parallel_streams` 设为大于 1，会把**该测试**的场景拆到多个进程（每个进程仍是串行的，会各自跑一遍 suite/spec 钩子）。`--runner-pool <n>` 可单独指定进程数。收到 `kill` 后请尽快退出，否则 1.5 s 后会被 SIGTERM/SIGKILL 终止整个进程组。

抖动的断言可设 `step_retry`（0–5）或在规范/场景上写 `tags: retry:1`（与请求取较大值）：仅对 `FAILED` 重试，不重试缺实现/崩溃/超时。结果树与 `step.retry` 事件会标出 `attempts`。

### 添加规范示例

把 `.spec` 放入 `specs/`，概念放入 `specs/concepts/`。服务默认每 2 秒轮询规范目录，用编辑器或 `git pull` 改动的文件会自动生效（概念自动重载，UI 规范页实时刷新并提示）；也可以调用 `POST /api/v1/specs/reload` 或在 UI 点击"重新加载"立即扫描。`--watch-interval <ms>` 调整频率，`--no-watch` 关闭。

多个规范根目录可在配置里列为项目，运行时在侧栏切换（有测试排队/运行中会 409，避免跑到一半换目录）：

```json
"specs": {
  "current": "main",
  "projects": [
    {"id": "main", "name": "主规范", "dir": "specs"},
    {"id": "alt", "name": "备用示例", "dir": "examples/alt-specs"}
  ]
}
```

未配置 `projects` 时会从 `specs.dir` 自动生成 id 为 `default` 的单项。`--specs` 若与某项目 `dir` 相同则选中该项，否则改写当前项目目录。计划里的相对路径相对于**当前**项目根目录。

### 添加定时计划

在 UI「测试计划」页填写 UTC cron（或 `@hourly`）并勾选规范，或：

```bash
curl -X POST http://localhost:8080/api/v1/schedules \
  -H 'Content-Type: application/json' \
  -d '{"name":"hourly-smoke","cron":"@hourly","spec_files":["login.spec"],"tags":["smoke"]}'
curl -X POST http://localhost:8080/api/v1/schedules/<id>/run   # 立即跑一次，不等整点
```

计划保存在 `data/schedules/`。`--no-scheduler` 关闭自动触发但仍可 CRUD / 立即运行。`skip_if_running` 默认开启，避免上一轮未结束时堆积。

### 容器与 Windows 服务

```bash
docker build -t testhub . && docker run --rm -p 8080:8080 testhub
```

Windows：`packaging/windows/package.ps1` 暂存 `testhub.exe` + specs/runners，管理员执行 `install-service.ps1` 调用 `testhub.exe --service install`（SCM 自动启动，工作目录为 exe 所在目录）。Linux 也可拷贝 `packaging/linux/testhub.service`。

## 6. 调试技巧

- `--log-level debug` 会打印每个 HTTP 请求、Runner 收发的每条 JSON 消息与事件派发；
- `GET /api/v1/events?limit=500` 或 UI 的"事件流"页面可回放最近事件；详情页与事件流支持暂停缓冲和导出 JSON。按 `?` 查看键盘快捷键。
- Web UI 的规范页会用 `GET /api/v1/runner/steps` 标出 Runner 未实现的步骤；
- Python Runner 可单独调试：`python3 runners/python/testhub_runner.py --impl-dir runners/python/step_impl --list-steps`；
- Node.js Runner 可单独调试：`node runners/node/testhub_runner.js --impl-dir runners/node/step_impl --list-steps`；
- 集成测试失败时，用 `./build/tests/testhub_integration_tests --filter <name>` 单独复现，服务器日志会一并输出。

## 7. 常见问题

**CMake 报告编译器不可用 / 链接 libstdc++ 失败**
指定编译器：`cmake -S . -B build -DCMAKE_CXX_COMPILER=g++`。使用 clang 且系统同时装有多个 GCC 时，可加 `-DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13`。

**Runner 状态是 `disconnected` / `step_count: 0`**
检查 `--dir` 指向包含 `step_impl/` 的目录；查看服务器日志中 Runner 的 stderr 输出；`POST /api/v1/runner/restart` 可手动重启。

**Runner 页某个进程显示 `error`："Runner process exited (will be restarted on next use)"**
池中的进程在两次测试之间退出了（被杀、崩溃、OOM）。这不影响其他进程；该槽位会在下次分配到它时自动重启（受 `runner.max_restarts` 每槽预算限制），也可以立即 `POST /api/v1/runner/restart` 重启全部进程。

**提交测试返回 400，提示 "Spec path(s) not found"**
`spec_files` 是相对规范目录的路径（如 `login.spec`、`checkout/pay.spec`），不是绝对路径；`GET /api/v1/specs` 可查看可用文件名。

**浏览器显示旧版本 UI**
静态资源带 ETag，重新编译后强制刷新（Ctrl+Shift+R）即可。

## 8. 提交前检查

```bash
cmake --build build -j && ctest --test-dir build
node --check web/app.js
python3 runners/python/test_runner_protocol.py
node runners/node/test_runner_protocol.js
```

CI（`.github/workflows/ci.yml`）会在 Linux（g++ / clang++）和 macOS 上以 `-Werror` 构建并运行上述全部测试。
