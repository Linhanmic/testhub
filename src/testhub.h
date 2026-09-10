/*
 * TestHub - 持久化自动化测试系统
 * 主服务器类
 */

#pragma once

#include "model/types.h"
#include "server/http_server.h"
#include "engine/test_queue.h"
#include "runner/runner_bridge.h"
#include "event/event_bus.h"

// 复用 Gauge 数据模型
#include "gauge/specification.h"

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <map>
#include <mutex>

namespace testhub {

/**
 * TestHub 配置
 */
struct TestHubConfig {
    // 服务器配置
    std::string host = "0.0.0.0";
    int port = 8080;
    
    // Runner 配置
    std::string runnerLanguage = "java";
    std::string projectPath = "";
    int runnerConnectionTimeout = 30000;
    int runnerRequestTimeout = 60000;
    bool autoRestartRunner = true;
    
    // 执行配置
    int maxConcurrentTests = 5;
    int maxParallelStreams = 4;
    int defaultTimeout = 300000;
    
    // 规范配置
    std::string specsDir = "specs";
    std::string conceptsDir = "concepts";
    bool watchChanges = true;
    
    // 日志配置
    std::string logLevel = "info";
    std::string logFile = "";
};

/**
 * TestHub 服务器
 * 持久化自动化测试系统
 */
class TestHub {
public:
    /**
     * 获取单例实例
     */
    static TestHub& getInstance() {
        static TestHub instance;
        return instance;
    }

    /**
     * 初始化服务器
     * @param config 配置
     * @return 是否成功
     */
    bool initialize(const TestHubConfig& config);

    /**
     * 启动服务器
     * @return 是否成功
     */
    bool start();

    /**
     * 停止服务器
     */
    void stop();

    /**
     * 检查服务器是否运行中
     */
    bool isRunning() const { return running_; }

    /**
     * 获取配置
     */
    const TestHubConfig& getConfig() const { return config_; }

    /**
     * 获取 HTTP 服务器
     */
    HttpServer& getHttpServer() { return *httpServer_; }

    /**
     * 获取测试队列
     */
    TestQueue& getTestQueue() { return *testQueue_; }

    /**
     * 获取 Runner 桥接
     */
    RunnerBridge& getRunnerBridge() { return *runnerBridge_; }

    /**
     * 获取事件总线
     */
    EventBus& getEventBus() { return EventBus::getInstance(); }

    /**
     * 提交测试
     * @param request 测试请求
     * @return 测试 ID
     */
    std::string submitTest(const TestRequest& request);

    /**
     * 取消测试
     * @param testId 测试 ID
     * @return 是否成功
     */
    bool cancelTest(const std::string& testId);

    /**
     * 获取测试状态
     * @param testId 测试 ID
     * @return 测试状态
     */
    TestStatus getTestStatus(const std::string& testId) const;

    /**
     * 获取测试结果
     * @param testId 测试 ID
     * @return 测试结果
     */
    TestResult getTestResult(const std::string& testId) const;

    /**
     * 列出所有测试
     * @return 测试信息列表
     */
    std::vector<TestInfo> listTests() const;

    /**
     * 获取服务器状态
     * @return 状态信息
     */
    std::map<std::string, std::string> getStatus() const;

private:
    TestHub() = default;
    ~TestHub();
    TestHub(const TestHub&) = delete;
    TestHub& operator=(const TestHub&) = delete;

    // 配置
    TestHubConfig config_;
    
    // 组件
    std::unique_ptr<HttpServer> httpServer_;
    std::unique_ptr<TestQueue> testQueue_;
    std::unique_ptr<RunnerBridge> runnerBridge_;
    
    // 运行状态
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    
    // 执行线程
    std::thread executionThread_;
    std::atomic<bool> stopExecution_{false};
    
    // 活跃测试
    std::map<std::string, TestStatus> activeTests_;
    std::map<std::string, TestResult> completedTests_;
    mutable std::mutex testsMutex_;
    
    // 事件处理器 ID
    std::string eventHandlerId_;

    /**
     * 执行循环
     * 从队列中取出任务并执行
     */
    void executionLoop();

    /**
     * 执行单个测试
     * @param task 测试任务
     */
    void executeTest(const TestTask& task);

    /**
     * 执行规范
     * @param testId 测试 ID
     * @param specFile 规范文件路径
     * @return 规范结果
     */
    SpecResult executeSpec(const std::string& testId, const std::string& specFile);

    /**
     * 执行场景
     * @param testId 测试 ID
     * @param scenario 场景对象
     * @return 场景结果
     */
    ScenarioResult executeScenario(const std::string& testId, std::shared_ptr<gauge::Scenario> scenario);

    /**
     * 执行步骤
     * @param testId 测试 ID
     * @param step 步骤对象
     * @return 步骤结果
     */
    StepResult executeStep(const std::string& testId, std::shared_ptr<gauge::Step> step);

    /**
     * 更新测试状态
     * @param testId 测试 ID
     * @param status 新状态
     */
    void updateTestStatus(const std::string& testId, const TestStatus& status);

    /**
     * 发布测试事件
     * @param eventType 事件类型
     * @param testId 测试 ID
     * @param data 事件数据
     */
    void publishTestEvent(const std::string& eventType, const std::string& testId,
                         const std::map<std::string, std::string>& data = {});

    /**
     * 加载规范文件
     * @param specFile 规范文件路径
     * @return 规范对象
     */
    std::shared_ptr<gauge::Specification> loadSpec(const std::string& specFile);

    /**
     * 生成测试 ID
     * @return 测试 ID
     */
    std::string generateTestId();
};

} // namespace testhub
