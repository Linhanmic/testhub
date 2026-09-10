# TestHub - 持久化自动化测试系统

TestHub 是一个全新的自动化测试系统，它与 Gauge 完全不同，但可以复用 Gauge 的 Runner 插件。TestHub 是一个**长运行的守护进程**，不需要每次运行测试都启动新进程。

## 核心特性

- **持久化运行**: 作为守护进程持续运行，无需每次启动新进程
- **HTTP API**: 提供 RESTful API 接口提交和管理测试
- **WebSocket 实时推送**: 实时推送测试执行状态和结果
- **测试队列**: 支持测试任务排队和优先级调度
- **Runner 复用**: 复用 Gauge 的 Runner 插件执行步骤
- **热加载**: 支持运行时加载新的测试规范

## 快速开始

### 启动服务器

```bash
# 使用 Java Runner 启动
testhub --port 8080 --language java

# 使用 Python Runner 启动
testhub --port 8080 --language python

# 后台运行
testhub --daemon --port 8080 --language java --pid-file /var/run/testhub.pid
```

### 使用 CLI 提交测试

```bash
# 提交测试
testhub run specs/login.spec specs/search.spec

# 按标签过滤
testhub run --tags smoke specs/

# 查询测试状态
testhub status test-20240101-001

# 列出所有测试
testhub list

# 获取测试结果
testhub result test-20240101-001
```

### 使用 HTTP API

```bash
# 提交测试
curl -X POST http://localhost:8080/api/v1/tests/run \
  -H "Content-Type: application/json" \
  -d '{"spec_files": ["specs/login.spec"], "tags": ["smoke"]}'

# 查询状态
curl http://localhost:8080/api/v1/tests/test-20240101-001

# 列出所有测试
curl http://localhost:8080/api/v1/tests

# 获取 Runner 状态
curl http://localhost:8080/api/v1/runner/status

# 健康检查
curl http://localhost:8080/api/v1/health
```

## API 端点

| 方法 | 路径 | 描述 |
|------|------|------|
| POST | /api/v1/tests/run | 提交测试运行请求 |
| GET | /api/v1/tests/{id} | 查询测试状态 |
| GET | /api/v1/tests | 列出所有测试 |
| DELETE | /api/v1/tests/{id} | 取消测试 |
| GET | /api/v1/tests/{id}/result | 获取测试结果 |
| GET | /api/v1/specs | 列出规范文件 |
| POST | /api/v1/specs/validate | 验证规范文件 |
| GET | /api/v1/runner/status | 获取 Runner 状态 |
| POST | /api/v1/runner/restart | 重启 Runner |
| GET | /api/v1/health | 健康检查 |

## 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                      TestHub Server                         │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │  HTTP API    │  │  WebSocket   │  │  Scheduler   │      │
│  │  Server      │  │  Server      │  │  (任务调度)   │      │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘      │
│         └─────────────────┼──────────────────┘              │
│                           │                                 │
│  ┌────────────────────────┼────────────────────────────┐    │
│  │           Test Execution Engine                     │    │
│  │           (测试执行引擎)                             │    │
│  └────────────────────────┬────────────────────────────┘    │
│                           │                                 │
│  ┌────────────────────────▼────────────────────────────┐    │
│  │           Runner Bridge (Runner 桥接)               │    │
│  │           (复用 Gauge Runner 插件)                   │    │
│  └────────────────────────┬────────────────────────────┘    │
│                           │                                 │
└───────────────────────────┼─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│              Gauge Runner Plugin                             │
│              (gauge-java / gauge-python / ...)               │
└─────────────────────────────────────────────────────────────┘
```

## 与 Gauge 的区别

| 特性 | Gauge | TestHub |
|------|-------|---------|
| 运行模式 | 每次启动新进程 | 守护进程持续运行 |
| 通信方式 | CLI 命令行 | HTTP API + WebSocket |
| 测试提交 | 命令行参数 | API 请求 |
| 结果获取 | 控制台输出 | API 查询 + WebSocket 推送 |
| 并发模型 | 多进程并行 | 单进程异步队列 |
| 状态管理 | 无状态 | 有状态，内存中维护 |

## 配置文件

```yaml
# testhub.yaml
server:
  host: "0.0.0.0"
  port: 8080

runner:
  language: "java"
  connection_timeout: 30000
  request_timeout: 60000
  auto_restart: true

execution:
  max_concurrent_tests: 5
  max_parallel_streams: 4
  default_timeout: 300000

specs:
  default_dir: "specs"
  concepts_dir: "concepts"
```

## 编译

```bash
# 创建构建目录
mkdir build
cd build

# 配置 CMake
cmake ..

# 编译
cmake --build .
```

## 依赖

- C++17 编译器
- CMake 3.16+
- Gauge Runner 插件（java/python/csharp/js/ruby）

## 许可证

Apache License 2.0
