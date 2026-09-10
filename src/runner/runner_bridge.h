/*
 * TestHub - Runner 桥接
 * 复用 Gauge Runner 插件的通信协议
 */

#pragma once

#include "../model/types.h"
#include "../event/event_bus.h"

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <queue>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace testhub {

/**
 * Runner 消息类型（复用 Gauge 协议）
 */
enum class RunnerMessageType {
    // 执行相关
    ExecuteStep = 0,
    StepExecutionStatus = 1,
    ExecutionStarting = 2,
    ExecutionEnding = 3,
    SuiteExecutionResult = 4,
    SpecExecutionStarting = 5,
    SpecExecutionEnding = 6,
    ScenarioExecutionStarting = 7,
    ScenarioExecutionEnding = 8,
    StepExecutionStarting = 9,
    StepExecutionEnding = 10,
    
    // 缓存相关
    CacheFile = 11,
    StepPositions = 12,
    StepNames = 13,
    AllSteps = 14,
    
    // 实现相关
    ImplementationFileGlobPattern = 15,
    ImplementationFileList = 16,
    StubImplementationCode = 17,
    Refactor = 18,
    UnsupportedMessage = 19,
    
    // 生命周期
    Kill = 20,
    
    // 信息
    SuiteDataStoreFlush = 21,
    ConceptExecutionStarting = 22,
    ConceptExecutionEnding = 23
};

/**
 * Runner 消息
 */
struct RunnerMessage {
    int id = 0;
    RunnerMessageType type;
    std::string payload;  // JSON 格式的 payload
};

/**
 * Runner 响应
 */
struct RunnerResponse {
    int messageId = 0;
    bool success = true;
    std::string payload;  // JSON 格式的响应
    std::string error;
};

/**
 * Runner 进程管理
 */
class RunnerProcess {
public:
    RunnerProcess();
    ~RunnerProcess();

    /**
     * 启动 Runner 进程
     * @param command 启动命令
     * @param workingDir 工作目录
     * @return 是否成功
     */
    bool start(const std::string& command, const std::string& workingDir = "");

    /**
     * 停止 Runner 进程
     */
    void stop();

    /**
     * 检查进程是否运行中
     */
    bool isRunning() const;

    /**
     * 获取进程 ID
     */
    int getPid() const { return pid_; }

    /**
     * 发送消息
     * @param message 消息内容
     * @return 是否成功
     */
    bool sendMessage(const std::string& message);

    /**
     * 接收消息
     * @param timeout 超时时间（毫秒）
     * @return 消息内容（超时返回空）
     */
    std::string receiveMessage(int timeout = 5000);

private:
    int pid_ = 0;
    bool running_ = false;

#ifdef _WIN32
    HANDLE processHandle_ = NULL;
    HANDLE stdinRead_ = NULL;
    HANDLE stdinWrite_ = NULL;
    HANDLE stdoutRead_ = NULL;
    HANDLE stdoutWrite_ = NULL;
#else
    int stdinFd_ = -1;
    int stdoutFd_ = -1;
#endif

    /**
     * 创建管道
     */
    bool createPipes();

    /**
     * 关闭管道
     */
    void closePipes();
};

/**
 * Runner 桥接
 * 管理与 Gauge Runner 插件的通信
 */
class RunnerBridge {
public:
    RunnerBridge();
    ~RunnerBridge();

    /**
     * 启动 Runner
     * @param language Runner 语言（java, python, csharp 等）
     * @param projectPath 项目路径
     * @return 是否成功
     */
    bool startRunner(const std::string& language, const std::string& projectPath = "");

    /**
     * 停止 Runner
     */
    void stopRunner();

    /**
     * 重启 Runner
     */
    bool restartRunner();

    /**
     * 检查 Runner 是否连接
     */
    bool isConnected() const;

    /**
     * 获取 Runner 状态
     */
    RunnerStatus getStatus() const;

    /**
     * 执行步骤
     * @param stepText 步骤文本
     * @param args 步骤参数
     * @return 步骤结果
     */
    StepResult executeStep(const std::string& stepText, const std::vector<std::string>& args = {});

    /**
     * 获取所有已实现的步骤
     * @return 步骤值列表
     */
    std::vector<StepValue> getAllSteps();

    /**
     * 缓存文件
     * @param request 缓存请求
     * @return 是否成功
     */
    bool cacheFile(const CacheFileRequest& request);

    /**
     * 获取步骤位置
     * @param filePath 文件路径
     * @return 步骤位置列表
     */
    std::vector<std::pair<int, std::string>> getStepPositions(const std::string& filePath);

    /**
     * 获取实现文件列表
     * @return 文件路径列表
     */
    std::vector<std::string> getImplementationFiles();

    /**
     * 获取 Runner 语言 ID
     */
    std::string getLanguageId() const { return language_; }

private:
    // Runner 进程
    std::unique_ptr<RunnerProcess> process_;
    
    // Runner 状态
    RunnerState state_ = RunnerState::DISCONNECTED;
    
    // 语言
    std::string language_;
    
    // 项目路径
    std::string projectPath_;
    
    // 消息 ID 计数器
    int messageIdCounter_ = 0;
    
    // 待处理的响应
    std::map<int, RunnerResponse> pendingResponses_;
    
    // 互斥锁
    mutable std::mutex mutex_;
    
    // 接收线程
    std::thread receiveThread_;
    std::atomic<bool> stopReceiving_{false};
    
    // 心跳线程
    std::thread heartbeatThread_;
    std::atomic<bool> stopHeartbeat_{false};

    /**
     * 发送消息并等待响应
     * @param message 消息
     * @param timeout 超时时间（毫秒）
     * @return 响应
     */
    RunnerResponse sendAndWait(const RunnerMessage& message, int timeout = 30000);

    /**
     * 发送消息
     * @param message 消息
     * @return 消息 ID
     */
    int sendMessage(const RunnerMessage& message);

    /**
     * 接收消息循环
     */
    void receiveLoop();

    /**
     * 心跳循环
     */
    void heartbeatLoop();

    /**
     * 处理接收到的消息
     * @param message 消息内容
     */
    void handleMessage(const std::string& message);

    /**
     * 解析响应
     * @param responseStr 响应字符串
     * @return 响应对象
     */
    RunnerResponse parseResponse(const std::string& responseStr);

    /**
     * 序列化消息
     * @param message 消息对象
     * @return 消息字符串
     */
    std::string serializeMessage(const RunnerMessage& message);

    /**
     * 获取 Runner 启动命令
     * @param language 语言
     * @return 启动命令
     */
    std::string getRunnerCommand(const std::string& language) const;

    /**
     * 发布 Runner 事件
     */
    void publishRunnerEvent(const std::string& eventType, const std::string& detail = "");
};

} // namespace testhub
