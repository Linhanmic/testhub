# TestHub - 持久化自动化测试系统设计文档

## 1. 系统概述

TestHub 是一个全新的自动化测试系统，它与 Gauge 完全不同，但可以复用 Gauge 的 Runner 插件。TestHub 是一个**长运行的守护进程**，不需要每次运行测试都启动新进程。

### 1.1 核心特性

- **持久化运行**: 作为守护进程持续运行，无需每次启动新进程
- **HTTP API**: 提供 RESTful API 接口提交和管理测试
- **WebSocket 实时推送**: 实时推送测试执行状态和结果
- **测试队列**: 支持测试任务排队和优先级调度
- **Runner 复用**: 复用 Gauge 的 Runner 插件执行步骤
- **热加载**: 支持运行时加载新的测试规范
- **多项目支持**: 同时管理多个测试项目

### 1.2 与 Gauge 的区别

| 特性 | Gauge | TestHub |
|------|-------|---------|
| 运行模式 | 每次启动新进程 | 守护进程持续运行 |
| 通信方式 | CLI 命令行 | HTTP API + WebSocket |
| 测试提交 | 命令行参数 | API 请求 |
| 结果获取 | 控制台输出 | API 查询 + WebSocket 推送 |
| 并发模型 | 多进程并行 | 单进程异步队列 |
| 状态管理 | 无状态 | 有状态，内存中维护 |

## 2. 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                      TestHub Server                         │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │  HTTP API    │  │  WebSocket   │  │  Scheduler   │      │
│  │  Server      │  │  Server      │  │  (任务调度)   │      │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘      │
│         │                 │                  │              │
│         └─────────────────┼──────────────────┘              │
│                           │                                 │
│  ┌────────────────────────┼────────────────────────────┐    │
│  │                        │                            │    │
│  │  ┌─────────────────────▼─────────────────────┐      │    │
│  │  │           Test Execution Engine            │      │    │
│  │  │           (测试执行引擎)                    │      │    │
│  │  └─────────────────────┬─────────────────────┘      │    │
│  │                        │                            │    │
│  │  ┌─────────────────────▼─────────────────────┐      │    │
│  │  │           Spec Parser (规范解析器)          │      │    │
│  │  │           (复用 Gauge 解析器)               │      │    │
│  │  └─────────────────────┬─────────────────────┘      │    │
│  │                        │                            │    │
│  │  ┌─────────────────────▼─────────────────────┐      │    │
│  │  │           Runner Bridge (Runner 桥接)      │      │    │
│  │  │           (复用 Gauge Runner 插件)          │      │    │
│  │  └─────────────────────┬─────────────────────┘      │    │
│  │                        │                            │    │
│  └────────────────────────┼────────────────────────────┘    │
│                           │                                 │
│  ┌────────────────────────▼────────────────────────────┐    │
│  │              Gauge Runner Plugin                     │    │
│  │              (gauge-java / gauge-python / ...)       │    │
│  └──────────────────────────────────────────────────────┘    │
│                                                             │
└─────────────────────────────────────────────────────────────┘

         ▲                    ▲                    ▲
         │                    │                    │
    HTTP 请求             WebSocket           测试提交
    (提交/查询)           (实时推送)          (文件监控)
         │                    │                    │
         ▼                    ▼                    ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│   CLI 工具   │    │   Web 界面   │    │   CI/CD      │
│   (testhub)  │    │   (可选)     │    │   集成       │
└──────────────┘    └──────────────┘    └──────────────┘
```

## 3. 核心模块设计

### 3.1 HTTP API Server

提供 RESTful API 接口：

```
POST   /api/v1/tests/run          # 提交测试运行请求
GET    /api/v1/tests/{id}          # 查询测试状态
GET    /api/v1/tests               # 列出所有测试
DELETE /api/v1/tests/{id}          # 取消测试
GET    /api/v1/tests/{id}/result   # 获取测试结果
GET    /api/v1/specs               # 列出规范文件
POST   /api/v1/specs/validate      # 验证规范文件
GET    /api/v1/runner/status       # 获取 Runner 状态
POST   /api/v1/runner/restart      # 重启 Runner
WS     /ws/v1/events               # WebSocket 事件流
```

### 3.2 Test Execution Engine (测试执行引擎)

```cpp
class TestExecutionEngine {
public:
    // 提交测试任务
    std::string submitTest(const TestRequest& request);
    
    // 取消测试
    bool cancelTest(const std::string& testId);
    
    // 获取测试状态
    TestStatus getTestStatus(const std::string& testId);
    
    // 获取测试结果
    TestResult getTestResult(const std::string& testId);
    
    // 列出所有测试
    std::vector<TestInfo> listTests();
    
private:
    // 测试队列
    TestQueue queue_;
    
    // 活跃测试
    std::map<std::string, std::shared_ptr<TestSession>> activeTests_;
    
    // Runner 桥接
    RunnerBridge runnerBridge_;
    
    // 事件总线
    EventBus eventBus_;
};
```

### 3.3 Runner Bridge (Runner 桥接)

复用 Gauge Runner 插件的通信协议：

```cpp
class RunnerBridge {
public:
    // 启动 Runner
    bool startRunner(const std::string& language);
    
    // 停止 Runner
    bool stopRunner();
    
    // 执行步骤
    StepResult executeStep(const StepExecutionRequest& request);
    
    // 获取所有步骤
    std::vector<StepValue> getAllSteps();
    
    // 缓存文件
    bool cacheFile(const CacheFileRequest& request);
    
    // 获取 Runner 状态
    RunnerStatus getStatus();
    
private:
    // gRPC 客户端
    std::unique_ptr<GaugeRunner::Stub> grpcClient_;
    
    // Runner 进程
    Process runnerProcess_;
    
    // 消息队列
    MessageQueue messageQueue_;
};
```

### 3.4 Test Scheduler (任务调度器)

```cpp
class TestScheduler {
public:
    // 添加任务到队列
    void enqueue(const TestTask& task);
    
    // 获取下一个任务
    std::optional<TestTask> dequeue();
    
    // 设置优先级
    void setPriority(const std::string& taskId, Priority priority);
    
    // 取消任务
    bool cancel(const std::string& taskId);
    
    // 获取队列状态
    QueueStatus getStatus();
    
private:
    // 优先级队列
    std::priority_queue<TestTask> queue_;
    
    // 互斥锁
    std::mutex mutex_;
    
    // 条件变量
    std::condition_variable cv_;
};
```

### 3.5 Event Bus (事件总线)

```cpp
class EventBus {
public:
    // 订阅事件
    void subscribe(const std::string& eventType, EventHandler handler);
    
    // 发布事件
    void publish(const Event& event);
    
    // 取消订阅
    void unsubscribe(const std::string& eventType, const std::string& handlerId);
    
private:
    // 事件处理器映射
    std::map<std::string, std::vector<EventHandler>> handlers_;
    
    // 互斥锁
    std::mutex mutex_;
};
```

## 4. 数据模型

### 4.1 TestRequest (测试请求)

```cpp
struct TestRequest {
    std::string id;                    // 请求 ID
    std::vector<std::string> specFiles; // 规范文件列表
    std::vector<std::string> tags;      // 标签过滤
    std::string environment;            // 环境
    int parallelStreams;                // 并行流数量
    Priority priority;                  // 优先级
    std::map<std::string, std::string> metadata; // 元数据
};
```

### 4.2 TestStatus (测试状态)

```cpp
enum class TestState {
    QUEUED,      // 排队中
    RUNNING,     // 运行中
    PASSED,      // 通过
    FAILED,      // 失败
    CANCELLED,   // 已取消
    ERROR        // 错误
};

struct TestStatus {
    std::string testId;
    TestState state;
    int totalSpecs;
    int executedSpecs;
    int passedSpecs;
    int failedSpecs;
    int totalScenarios;
    int executedScenarios;
    int passedScenarios;
    int failedScenarios;
    double progress;        // 0.0 - 1.0
    std::string currentSpec;
    std::string currentScenario;
    std::chrono::system_clock::time_point startTime;
    std::chrono::system_clock::time_point endTime;
};
```

### 4.3 TestResult (测试结果)

```cpp
struct TestResult {
    std::string testId;
    TestState finalState;
    std::vector<SpecResult> specResults;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    double totalDuration;
    std::chrono::system_clock::time_point startTime;
    std::chrono::system_clock::time_point endTime;
};

struct SpecResult {
    std::string specFile;
    std::string specName;
    TestState state;
    std::vector<ScenarioResult> scenarioResults;
    double duration;
};

struct ScenarioResult {
    std::string scenarioName;
    TestState state;
    std::vector<StepResult> stepResults;
    std::string errorMessage;
    double duration;
};

struct StepResult {
    std::string stepText;
    TestState state;
    std::string errorMessage;
    std::string stackTrace;
    double duration;
};
```

### 4.4 Event (事件)

```cpp
struct Event {
    std::string type;       // 事件类型
    std::string testId;     // 关联的测试 ID
    std::chrono::system_clock::time_point timestamp;
    std::map<std::string, std::string> data;
};

// 事件类型常量
namespace EventType {
    const std::string TEST_SUBMITTED = "test.submitted";
    const std::string TEST_STARTED = "test.started";
    const std::string TEST_COMPLETED = "test.completed";
    const std::string TEST_CANCELLED = "test.cancelled";
    const std::string SPEC_STARTED = "spec.started";
    const std::string SPEC_COMPLETED = "spec.completed";
    const std::string SCENARIO_STARTED = "scenario.started";
    const std::string SCENARIO_COMPLETED = "scenario.completed";
    const std::string STEP_STARTED = "step.started";
    const std::string STEP_COMPLETED = "step.completed";
    const std::string RUNNER_CONNECTED = "runner.connected";
    const std::string RUNNER_DISCONNECTED = "runner.disconnected";
}
```

## 5. 通信协议

### 5.1 HTTP API 请求/响应格式

**提交测试请求:**
```json
POST /api/v1/tests/run
{
    "spec_files": ["specs/login.spec", "specs/search.spec"],
    "tags": ["smoke"],
    "environment": "default",
    "parallel_streams": 1,
    "priority": "normal"
}
```

**响应:**
```json
{
    "test_id": "test-20240101-001",
    "status": "queued",
    "message": "Test submitted successfully"
}
```

**查询测试状态:**
```json
GET /api/v1/tests/test-20240101-001

{
    "test_id": "test-20240101-001",
    "state": "running",
    "progress": 0.65,
    "total_specs": 3,
    "executed_specs": 2,
    "passed_specs": 2,
    "failed_specs": 0,
    "current_spec": "specs/search.spec",
    "current_scenario": "Search with data table",
    "start_time": "2024-01-01T10:00:00Z"
}
```

### 5.2 WebSocket 事件格式

```json
{
    "event": "scenario.completed",
    "test_id": "test-20240101-001",
    "timestamp": "2024-01-01T10:05:30Z",
    "data": {
        "spec_file": "specs/login.spec",
        "scenario_name": "Successful login",
        "state": "passed",
        "duration": 1.234
    }
}
```

### 5.3 Runner 通信协议

复用 Gauge 的 gRPC 协议与 Runner 插件通信：

```protobuf
service GaugeRunner {
    rpc ExecuteStep(StepExecutionRequest) returns (StepExecutionResponse);
    rpc CacheFile(CacheFileRequest) returns (CacheFileResponse);
    rpc GetAllSteps(Empty) returns (GetAllStepsResponse);
    rpc GetStepPositions(StepPositionsRequest) returns (StepPositionsResponse);
    rpc Kill(Empty) returns (Empty);
}
```

## 6. 工作流程

### 6.1 测试提交流程

```
用户提交测试请求
       │
       ▼
┌──────────────┐
│ HTTP API     │
│ 接收请求     │
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ 解析规范文件 │  ← 复用 Gauge 解析器
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ 创建测试任务 │
│ 加入队列     │
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ 返回任务 ID  │
└──────────────┘
```

### 6.2 测试执行流程

```
调度器从队列取出任务
       │
       ▼
┌──────────────┐
│ 创建测试会话 │
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ 遍历规范文件 │
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ 遍历场景     │
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ 执行步骤     │──→ Runner Bridge ──→ Gauge Runner
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ 收集结果     │
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ 发布事件     │──→ WebSocket ──→ 客户端
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ 更新状态     │
└──────────────┘
```

## 7. 目录结构

```
testhub/
├── CMakeLists.txt
├── DESIGN.md
├── src/
│   ├── main.cpp                    # 程序入口
│   ├── server/                     # 服务器模块
│   │   ├── http_server.h/cpp       # HTTP 服务器
│   │   ├── websocket_server.h/cpp  # WebSocket 服务器
│   │   └── request_handler.h/cpp   # 请求处理器
│   ├── engine/                     # 执行引擎
│   │   ├── execution_engine.h/cpp  # 执行引擎
│   │   ├── test_session.h/cpp      # 测试会话
│   │   ├── test_queue.h/cpp        # 测试队列
│   │   └── scheduler.h/cpp         # 任务调度器
│   ├── runner/                     # Runner 桥接
│   │   ├── runner_bridge.h/cpp     # Runner 桥接
│   │   ├── grpc_client.h/cpp       # gRPC 客户端
│   │   └── message_protocol.h/cpp  # 消息协议
│   ├── parser/                     # 规范解析
│   │   ├── spec_parser.h/cpp       # 规范解析器
│   │   └── concept_parser.h/cpp    # 概念解析器
│   ├── event/                      # 事件系统
│   │   ├── event_bus.h/cpp         # 事件总线
│   │   └── event_types.h           # 事件类型
│   ├── model/                      # 数据模型
│   │   ├── test_request.h          # 测试请求
│   │   ├── test_status.h           # 测试状态
│   │   ├── test_result.h           # 测试结果
│   │   └── event.h                 # 事件
│   └── util/                       # 工具类
│       ├── file_util.h/cpp         # 文件工具
│       ├── string_util.h/cpp       # 字符串工具
│       └── json_util.h/cpp         # JSON 工具
├── tests/                          # 测试文件
└── examples/                       # 示例
    └── api_client/                 # API 客户端示例
```

## 8. 使用示例

### 8.1 启动 TestHub 服务器

```bash
# 启动服务器
testhub --port 8080 --runner-language java

# 后台运行
testhub daemon --port 8080 --runner-language java --pid-file /var/run/testhub.pid
```

### 8.2 使用 CLI 提交测试

```bash
# 提交测试
testhub run specs/login.spec specs/search.spec

# 按标签过滤
testhub run --tags "smoke" specs/

# 查询测试状态
testhub status test-20240101-001

# 获取测试结果
testhub result test-20240101-001
```

### 8.3 使用 HTTP API

```bash
# 提交测试
curl -X POST http://localhost:8080/api/v1/tests/run \
  -H "Content-Type: application/json" \
  -d '{"spec_files": ["specs/login.spec"], "tags": ["smoke"]}'

# 查询状态
curl http://localhost:8080/api/v1/tests/test-20240101-001

# 获取结果
curl http://localhost:8080/api/v1/tests/test-20240101-001/result
```

### 8.4 WebSocket 实时监听

```javascript
const ws = new WebSocket('ws://localhost:8080/ws/v1/events');

ws.onmessage = (event) => {
    const data = JSON.parse(event.data);
    console.log(`Event: ${data.event}, Test: ${data.test_id}`);
    
    if (data.event === 'test.completed') {
        console.log(`Test completed with state: ${data.data.state}`);
    }
};
```

## 9. 配置文件

```yaml
# testhub.yaml
server:
  host: "0.0.0.0"
  port: 8080
  max_connections: 100

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
  watch_changes: true

logging:
  level: "info"
  file: "logs/testhub.log"
  max_size: "100MB"
  max_files: 10
```

## 10. 扩展性设计

### 10.1 插件系统

支持自定义扩展：
- 自定义报告器
- 自定义通知器（邮件、Slack 等）
- 自定义认证方式

### 10.2 分布式部署

未来支持：
- 多节点部署
- 负载均衡
- 测试任务分发

### 10.3 存储后端

支持多种存储：
- 内存存储（默认）
- SQLite 存储
- PostgreSQL 存储
- Redis 存储
