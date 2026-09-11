# TestHub - 持久化自动化测试系统

[![CI](https://github.com/Linhanmic/testhub/actions/workflows/ci.yml/badge.svg)](https://github.com/Linhanmic/testhub/actions/workflows/ci.yml)

TestHub 是一个**长运行的自动化测试守护进程**：它常驻内存、通过 HTTP API 与 WebSocket 接收测试请求并实时推送执行进度，内嵌图形化 Web 控制台，并通过语言无关的 JSON-lines 协议驱动外部 Runner 进程执行步骤实现。

规范文件采用 Gauge 风格的 Markdown（`.spec` / `.cpt`），但 TestHub 不依赖 Gauge，也不依赖任何第三方 C++ 库：JSON、HTTP/1.1、WebSocket（RFC 6455）、SHA-1/Base64、规范解析器全部自带，`cmake && cmake --build` 即可得到一个单一可执行文件。

## 核心特性

| 领域 | 能力 |
|------|------|
| 运行模式 | 守护进程常驻；`--daemon`、PID 文件、JSON 配置文件、CLI 覆盖 |
| 规范 | `# 规范` / `## 场景` / `* 步骤`、`tags:`、规范级数据表（数据驱动）、上下文步骤、`___` 清理步骤、概念（`.cpt`）展开、`"静态"` / `<动态>` / `<file:>` / `<table:>` 参数、内联表格；目录监控：编辑器/git 改动自动生效并推送 `specs.reloaded` |
| 执行引擎 | 优先级队列、标签过滤表达式（`smoke & !slow`）、场景名过滤、fail_fast、测试/步骤超时、取消、`failed_only` 重跑、结果历史；**步骤重试**（请求 `step_retry` 或标签 `retry:N`，仅对 FAILED）；**并行流**（`parallel_streams`）把单个测试的场景拆到多个 Runner 进程 |
| 持久化 | 已完成的测试以 JSON 落盘（默认 `data/results/`），重启后自动回放历史与统计 |
| 报表 | 按需导出 JUnit XML（供 Jenkins / GitLab / GitHub Actions 收集）与自包含 HTML 报告；UI 一键下载 |
| 回调 | 请求携带 `callback_url`，测试结束后 POST JSON 摘要（含失败场景清单与报表链接），失败按指数退避重试 |
| 鉴权 | 可选 Bearer Token：默认保护写操作，可扩展到读操作与 WebSocket；UI 内置 token 输入 |
| Runner | 跨平台子进程桥接 + JSON-lines 协议；内置 mock Runner；**Python** 与 **Node.js** 两个参考 Runner（步骤注册、钩子、数据表、消息；Node 支持 async 步骤），同一套 .spec 可互换执行；**Runner 池**：默认每个并发测试独占一个进程，`parallel_streams` 可让一个测试占用多个进程；进程逐个自愈、按需重启 |
| 服务端 | 多线程 HTTP/1.1（keep-alive、流水线、Content-Length、超时、`{param}` 路由、CORS、HEAD/OPTIONS、ETag 静态资源、SPA 回退） |
| 实时性 | 异步事件总线；`/ws/v1/events` WebSocket 推送（按类型/测试 ID 订阅、历史回放） |
| Web UI | 内嵌单页应用：总览、提交测试、测试记录、结果树（搜索 / 只看失败）、运行中实时执行树、规范浏览/编辑/校验、Runner 状态、事件流（暂停 / 导出）、键盘快捷键、暗色模式 |
| 质量 | 78 个单元测试 + 19 个 HTTP/WS 集成测试 + 7 个 Python 协议测试 + 12 个 Node.js 协议测试；`ctest` 一键运行；GitHub Actions（Linux g++/clang++、macOS，`-Werror`）；ThreadSanitizer 零告警 |

## 快速开始

### 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # 可选：运行全部测试
```

要求：C++17 编译器（GCC 9+ / Clang 10+ / MSVC 2019+）与 CMake 3.16+。若系统默认 `c++` 不可用，加 `-DCMAKE_CXX_COMPILER=g++`。

### 启动

```bash
# 使用内置 mock Runner（无外部依赖，所有步骤自动通过）
./build/testhub --port 8080 --specs ./specs

# 使用 Python 参考 Runner 执行仓库自带的示例规范
./build/testhub --port 8080 --language python --dir runners/python

# 同一套规范，换用 Node.js 参考 Runner（协议语言无关）
./build/testhub --port 8080 --language node --dir runners/node

# 4 个测试并发：默认拉起 4 个 Runner 进程（Runner 池），每个测试独占一个进程
./build/testhub --language python --dir runners/python -j 4

# 后台运行
./build/testhub --daemon --pid-file /tmp/testhub.pid --log-file /tmp/testhub.log --config testhub.json
```

启动后打开 <http://localhost:8080/> 即可使用 Web 控制台；API 位于 `/api/v1/`，WebSocket 位于 `/ws/v1/events`。

### 提交第一个测试

```bash
# 运行 login.spec 中带 smoke 标签的场景
curl -X POST http://localhost:8080/api/v1/tests \
  -H 'Content-Type: application/json' \
  -d '{"spec_files": ["login.spec"], "tags": ["smoke"]}'
# => {"test_id":"test-20260910-142940-001","status":"queued",...}

curl http://localhost:8080/api/v1/tests/test-20260910-142940-001          # 状态
curl http://localhost:8080/api/v1/tests/test-20260910-142940-001/result   # 结果树
curl -o report.xml  "http://localhost:8080/api/v1/tests/test-20260910-142940-001/report?format=junit"  # JUnit XML
curl -o report.html "http://localhost:8080/api/v1/tests/test-20260910-142940-001/report?format=html"   # HTML 报告
```

`spec_files` 为空数组时表示运行规范目录下的全部规范。JUnit XML 可直接交给 Jenkins、GitLab CI（`artifacts:reports:junit`）或 GitHub Actions 的测试报告插件。

请求体其他可选字段：`name`、`scenarios`（场景名过滤）、`priority`（low/normal/high/urgent）、`environment`、`timeout_ms`、`fail_fast`、`step_retry`（断言失败步骤的最大重试次数，默认 0、上限 5；规范/场景标签 `retry:N` 或 `retry-N` 与本字段取较大值；不重试 `error` / 取消 / 超时）、`parallel_streams`（把本测试的场景拆到多个 Runner 进程，默认 1；受池大小与场景数限制）、`metadata`（字符串键值，会原样带入报表与回调）、`callback_url`（测试结束后 POST JSON 摘要到该 `http://` 地址，失败按指数退避重试；载荷含 `state`、场景计数、`failed_scenarios_detail` 与 `links.report_junit` 等链接）。

HTTP 绑定成功后，守护进程会把 `TESTHUB_URL`（`callbacks.public_base_url` 或 `http://127.0.0.1:<port>`）写入 Runner 环境；若启用了鉴权，同时写入 `TESTHUB_TOKEN`。用 Python 或 Node Runner 即可跑仓库自带的自举规范，用 TestHub 自己的 API 验证自己：

```bash
curl -X POST http://localhost:8080/api/v1/tests \
  -H 'Content-Type: application/json' \
  -d '{"spec_files":["selfcheck.spec"],"tags":["selfcheck"]}'
```

`selfcheck.spec` 只断言子测试提交返回 202，不会在步骤里等待子测试结束（避免 `-j 1` 占满唯一 worker）。mock Runner 不会真正发 HTTP，自举需要 `--language python` 或 `--language node`。

## 编写规范与步骤实现

`specs/calculator.spec`：

```markdown
# 计算器
tags: calc

|a  |b  |sum|
|---|---|---|
|1  |2  |3  |
|10 |20 |30 |

## 两数相加
* 输入第一个数 <a>
* 输入第二个数 <b>
* 点击加号
* 结果应该是 <sum>
```

`runners/python/step_impl/calc_steps.py`：

```python
from testhub_runner import step, data_store

@step("输入第一个数 <a>")
def first_number(a):
    data_store.scenario["a"] = int(a)

@step("结果应该是 <expected>")
def result_should_be(expected):
    assert data_store.scenario["result"] == int(expected)
```

同一步骤的 Node.js 实现（`runners/node/step_impl/calc_steps.js`，支持 `async` 函数）：

```js
const assert = require('assert');
const { step, dataStore } = require('testhub-runner');

step('输入第一个数 <a>', (a) => { dataStore.scenario.a = Number(a); });
step('结果应该是 <expected>', async (expected) => {
    assert.strictEqual(dataStore.scenario.result, Number(expected));
});
```

Runner 启动时会加载 `--dir` 下 `step_impl/` 中的全部 `.py` / `.js` 文件。更多示例见 `specs/`、`runners/python/step_impl/` 与 `runners/node/step_impl/`，协议细节见 [DESIGN.md](DESIGN.md#53-runner-通信协议)。

## API 端点

| 方法 | 路径 | 描述 |
|------|------|------|
| GET | `/api/v1/health` | 健康检查 |
| GET | `/api/v1/status` | 服务器统计（队列、运行中、通过/失败计数、本机 `url`） |
| GET | `/api/v1/config` | 当前生效配置 |
| POST | `/api/v1/tests`（别名 `/tests/run`） | 提交测试，返回 202 与 `test_id` |
| GET | `/api/v1/tests?state=&limit=&offset=` | 列出测试 |
| DELETE | `/api/v1/tests` | 清空已完成历史 |
| GET | `/api/v1/tests/{id}` | 测试状态（含请求与进度） |
| GET | `/api/v1/tests/{id}/result` | 结果树（未完成返回 202） |
| GET | `/api/v1/tests/{id}/report?format=junit\|html&download=1` | JUnit XML / HTML 报表（未完成返回 409） |
| GET | `/api/v1/tests/{id}/events` | 该测试的事件历史 |
| POST | `/api/v1/tests/{id}/cancel` | 取消（已结束返回 409） |
| DELETE | `/api/v1/tests/{id}` | 取消或删除记录 |
| POST | `/api/v1/tests/{id}/rerun` | 重跑（`{"failed_only": true}` 仅重跑失败场景） |
| GET | `/api/v1/queue` | 当前队列 |
| GET | `/api/v1/specs` | 列出规范（含场景数、标签、无效文件） |
| POST | `/api/v1/specs/validate` | 校验文件或内联内容 |
| POST | `/api/v1/specs/reload` | 立即重新扫描规范目录（返回 `changes`；目录监控开启时通常无需手动调用） |
| GET / PUT / DELETE | `/api/v1/specs/{path}` | 读取（`?raw=true`）/ 创建或更新 / 删除规范 |
| GET | `/api/v1/concepts` | 列出概念 |
| GET | `/api/v1/runner/status` | Runner 池状态（聚合 + `runners[]` 每个进程的状态/PID/已执行步骤/重启次数） |
| POST | `/api/v1/runner/restart` | 重启池中全部 Runner 进程（等待正在执行的测试结束） |
| GET | `/api/v1/runner/steps` | Runner 报告的已实现步骤 |
| GET | `/api/v1/events?limit=&type=&test_id=` | 全局事件历史 |
| WS | `/ws/v1/events` | 实时事件流 |

WebSocket 连接后发送 `{"action":"subscribe","events":["test.*","scenario.*"],"test_id":"..."}` 可缩小订阅范围；服务器推送 `{"type":"event","event":"step.completed","test_id":"...","timestamp":"...","data":{...}}`。

## 命令行选项

```
-p, --port <port>          HTTP 监听端口（默认 8080，0 表示自动分配）
-H, --host <host>          监听地址（默认 0.0.0.0）
-l, --language <lang>      Runner 语言：mock | python | node | custom（默认 mock）
-r, --runner-cmd <cmd>     自定义 Runner 启动命令
-d, --dir <path>           测试项目目录（Runner 工作目录）
    --runner-pool <n>      Runner 进程数（默认 0：跟随 -j，每个并发测试一个进程）
-s, --specs <path>         规范目录（默认 specs）
    --concepts <path>      概念目录（默认与规范目录相同）
    --watch-interval <ms>  规范目录轮询间隔（默认 2000，0 禁用）
    --no-watch             不监控规范目录变化
-c, --config <file>        JSON 配置文件
-j, --concurrency <n>      并发执行的测试数（默认 1）
    --timeout <ms>         测试默认超时
    --results-dir <path>   结果持久化目录（默认 data/results）
    --no-persist           不持久化结果，仅保存在内存
    --public-url <url>     回调载荷中链接的公开地址前缀
    --no-callbacks         禁用 callback_url 完成回调
    --auth-token <token>   启用 Bearer Token 鉴权（或环境变量 TESTHUB_AUTH_TOKEN）
    --auth-protect-reads   鉴权同时覆盖 GET 与 WebSocket
    --log-level <level>    debug | info | warn | error | off
    --log-file <file>      日志文件
    --no-ui                不提供 Web UI
    --web-dir <path>       从磁盘目录提供 Web UI（前端开发模式，免重新编译）
    --pid-file <file>      写入 PID 文件
    --daemon               守护进程模式（POSIX）
    --print-config         打印最终生效配置并退出
```

配置文件为 JSON，键名与 `--print-config` 输出一致（`server` / `runner` / `execution` / `specs` / `logging`），CLI 参数优先级高于配置文件。

## 架构

```
┌──────────────────────────────── TestHub 进程 ────────────────────────────────┐
│  HttpServer ── ApiRoutes ─┐            WebSocketServer ◄── EventBus (异步派发) │
│      │  静态资源(内嵌 web/) │                                   ▲              │
│      ▼                    ▼                                   │ publish       │
│  Web UI (SPA)      ExecutionEngine ── TestQueue(优先级) ── worker 线程 ×N       │
│                           │  SpecRepository / SpecParser / ConceptDictionary   │
│                           ▼                                                   │
│                     RunnerBridge (Runner 池：槽位 ×N、会话/并行流、逐槽自愈)       │
│                    ┌──────┴───────┐                                           │
│                MockRunner    ProcessRunner ×N (stdin/stdout JSON-lines)        │
└─────────────────────────────────────┼─────────────────────────────────────────┘
                                      ▼
        runners/python/testhub_runner.py ×N ( step_impl/*.py )  或  runners/node/testhub_runner.js ×N ( step_impl/*.js )
```

详细设计见 [DESIGN.md](DESIGN.md)，开发指南见 [QUICKSTART.md](QUICKSTART.md)，任务与迭代记录见 [TODO.md](TODO.md)。

## 目录结构

```
CMakeLists.txt          构建脚本（自动内嵌 web/ 资源）
cmake/EmbedResources.cmake
src/
  main.cpp              CLI / 守护进程入口
  testhub.{h,cpp}       TestHub 门面：配置、组件装配、生命周期
  server/               http_server, websocket_server, api_routes, web_ui, web_assets
  engine/               execution_engine, test_queue, tag_filter
  runner/               runner 接口, mock_runner, process_runner, runner_bridge
  spec/                 spec 数据模型, spec_parser, spec_repository
  event/                event_bus
  model/                types, json_convert
  util/                 json, sha1, base64, logger, string/file/time 工具
web/                    Web UI 源码（index.html, app.js, app.css, favicon.svg）
runners/python/         Python 参考 Runner、示例步骤实现、协议自测
runners/node/           Node.js 参考 Runner（同一协议，async 步骤）、示例步骤实现、协议自测
specs/                  示例规范（login / calculator / checkout / streams / selfcheck / slow）与概念
tests/                  单元测试与集成测试（自带迷你测试框架）
.github/workflows/      CI
```

## 许可证

Apache License 2.0
