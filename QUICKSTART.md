# TestHub 快速开始指南

## 环境准备

### 1. 检查编译环境
```powershell
# 检查 g++ 版本
g++ --version

# 检查 cmake 版本
cmake --version

# 检查 mingw32-make 版本
mingw32-make --version
```

### 2. 编译项目
```powershell
cd C:\Users\13657\Desktop\uHIL\gauge\testhub\build
mingw32-make -j4
```

### 3. 运行服务器
```powershell
cd C:\Users\13657\Desktop\uHIL\gauge\testhub\build
./testhub.exe --port 9090 --language java
```

## 测试 API

### 1. 健康检查
```powershell
Invoke-WebRequest -Uri "http://localhost:9090/api/v1/health" -Method Get
```

### 2. 提交测试
```powershell
$body = '{"spec_files":["specs/login.spec"]}'
Invoke-WebRequest -Uri "http://localhost:9090/api/v1/tests/run" -Method Post -ContentType "application/json" -Body $body
```

### 3. 查询测试状态
```powershell
Invoke-WebRequest -Uri "http://localhost:9090/api/v1/tests" -Method Get
```

## 项目结构

```
testhub/
├── CMakeLists.txt          # 构建配置
├── DESIGN.md               # 设计文档
├── README.md               # 项目说明
├── CURSOR_PROMPT.md        # Cursor 开发提示词
├── .cursorrules            # Cursor 规则文件
├── TODO.md                 # 任务追踪
├── QUICKSTART.md           # 本文件
├── src/
│   ├── main.cpp            # 程序入口
│   ├── testhub.h/cpp       # 主服务器类
│   ├── server/             # HTTP 服务器
│   ├── engine/             # 测试引擎
│   ├── runner/             # Runner 桥接
│   ├── event/              # 事件系统
│   ├── model/              # 数据模型
│   └── util/               # 工具类
├── build/                  # 编译输出
└── specs/                  # 测试规范
```

## 核心模块

### 1. HTTP 服务器 (server/http_server.h)
- 处理 HTTP 请求
- 路由匹配
- 请求/响应处理

### 2. 测试队列 (engine/test_queue.h)
- 优先级队列
- 任务调度
- 状态管理

### 3. Runner 桥接 (runner/runner_bridge.h)
- 与 Gauge Runner 通信
- 步骤执行
- 进程管理

### 4. 事件系统 (event/event_bus.h)
- 发布/订阅模式
- 实时事件推送

## 常见任务

### 添加新的 API 端点

1. 在 `server/http_server.h` 中声明处理函数
2. 在 `server/http_server.cpp` 中实现处理函数
3. 在 `registerDefaultRoutes()` 中注册路由

```cpp
// 1. 声明
HttpResponse handleNewEndpoint(const HttpRequest& request);

// 2. 实现
HttpResponse HttpServer::handleNewEndpoint(const HttpRequest& request) {
    return HttpResponse::json(200, "{\"message\":\"Hello\"}");
}

// 3. 注册
get("/api/v1/new", [this](const HttpRequest& req) { return handleNewEndpoint(req); });
```

### 添加新的事件类型

1. 在 `model/types.h` 中定义事件常量
2. 在需要的地方发布事件

```cpp
// 1. 定义常量
namespace EventType {
    const std::string NEW_EVENT = "new.event";
}

// 2. 发布事件
publishEvent(EventType::NEW_EVENT, testId, {{"key", "value"}});
```

### 修改数据模型

1. 在 `model/types.h` 中修改结构体
2. 更新相关的序列化/反序列化代码
3. 更新 API 处理函数

## 调试技巧

### 1. 启用调试输出
```cpp
std::cout << "[DEBUG] Variable: " << variable << std::endl;
```

### 2. 检查日志
服务器会输出详细的日志信息，包括：
- 请求处理
- 事件发布
- 错误信息

### 3. 使用 curl 测试
```bash
curl -v http://localhost:9090/api/v1/health
```

## 常见问题

### Q: 编译失败怎么办？
A: 检查错误消息，通常是：
- 缺少头文件
- 语法错误
- 链接错误

### Q: 服务器无法启动怎么办？
A: 检查：
- 端口是否被占用
- 防火墙设置
- 权限问题

### Q: API 返回 404 怎么办？
A: 检查：
- URL 是否正确
- HTTP 方法是否正确
- 路由是否注册

## 下一步

1. 阅读 `DESIGN.md` 了解整体设计
2. 阅读 `CURSOR_PROMPT.md` 了解开发计划
3. 查看 `TODO.md` 了解当前任务
4. 开始编码！

## 获取帮助

- 查看代码注释
- 搜索相关文件
- 检查错误消息
- 尝试不同的解决方案

记住：**不要停下来问用户，所有决策自主完成！**
