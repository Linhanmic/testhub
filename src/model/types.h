/*
 * TestHub - 持久化自动化测试系统
 * 数据模型定义
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <optional>
#include <functional>
#include <any>

namespace testhub {

// ============================================================
// 枚举类型
// ============================================================

/**
 * 测试状态
 */
enum class TestState {
    QUEUED,      // 排队中
    RUNNING,     // 运行中
    PASSED,      // 通过
    FAILED,      // 失败
    CANCELLED,   // 已取消
    TEST_ERROR   // 错误
};

/**
 * 任务优先级
 */
enum class Priority {
    LOW = 0,
    NORMAL = 1,
    HIGH = 2,
    URGENT = 3
};

/**
 * Runner 状态
 */
enum class RunnerState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    BUSY,
    RUNNER_ERROR
};

// ============================================================
// 辅助函数
// ============================================================

inline std::string testStateToString(TestState state) {
    switch (state) {
        case TestState::QUEUED: return "queued";
        case TestState::RUNNING: return "running";
        case TestState::PASSED: return "passed";
        case TestState::FAILED: return "failed";
        case TestState::CANCELLED: return "cancelled";
        case TestState::TEST_ERROR: return "error";
        default: return "unknown";
    }
}

inline TestState stringToTestState(const std::string& str) {
    if (str == "queued") return TestState::QUEUED;
    if (str == "running") return TestState::RUNNING;
    if (str == "passed") return TestState::PASSED;
    if (str == "failed") return TestState::FAILED;
    if (str == "cancelled") return TestState::CANCELLED;
    if (str == "error") return TestState::TEST_ERROR;
    return TestState::TEST_ERROR;
}

inline std::string priorityToString(Priority p) {
    switch (p) {
        case Priority::LOW: return "low";
        case Priority::NORMAL: return "normal";
        case Priority::HIGH: return "high";
        case Priority::URGENT: return "urgent";
        default: return "normal";
    }
}

inline Priority stringToPriority(const std::string& str) {
    if (str == "low") return Priority::LOW;
    if (str == "high") return Priority::HIGH;
    if (str == "urgent") return Priority::URGENT;
    return Priority::NORMAL;
}

// ============================================================
// 数据模型
// ============================================================

/**
 * 测试请求
 */
struct TestRequest {
    std::string id;
    std::vector<std::string> specFiles;
    std::vector<std::string> tags;
    std::string environment = "default";
    int parallelStreams = 1;
    Priority priority = Priority::NORMAL;
    std::map<std::string, std::string> metadata;
    
    // 回调 URL（可选）
    std::string callbackUrl;
};

/**
 * 步骤结果
 */
struct StepResult {
    std::string stepText;
    TestState state = TestState::PASSED;
    std::string errorMessage;
    std::string stackTrace;
    double duration = 0.0;
};

/**
 * 场景结果
 */
struct ScenarioResult {
    std::string scenarioName;
    TestState state = TestState::PASSED;
    std::vector<StepResult> stepResults;
    std::string errorMessage;
    double duration = 0.0;
};

/**
 * 规范结果
 */
struct SpecResult {
    std::string specFile;
    std::string specName;
    TestState state = TestState::PASSED;
    std::vector<ScenarioResult> scenarioResults;
    std::string errorMessage;
    double duration = 0.0;
};

/**
 * 测试状态
 */
struct TestStatus {
    std::string testId;
    TestState state = TestState::QUEUED;
    int totalSpecs = 0;
    int executedSpecs = 0;
    int passedSpecs = 0;
    int failedSpecs = 0;
    int totalScenarios = 0;
    int executedScenarios = 0;
    int passedScenarios = 0;
    int failedScenarios = 0;
    double progress = 0.0;
    std::string currentSpec;
    std::string currentScenario;
    std::string currentStep;
    std::chrono::system_clock::time_point startTime;
    std::chrono::system_clock::time_point endTime;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/**
 * 测试结果
 */
struct TestResult {
    std::string testId;
    TestState finalState = TestState::PASSED;
    std::vector<SpecResult> specResults;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    double totalDuration = 0.0;
    std::chrono::system_clock::time_point startTime;
    std::chrono::system_clock::time_point endTime;
};

/**
 * 测试信息（用于列表）
 */
struct TestInfo {
    std::string testId;
    TestState state;
    std::vector<std::string> specFiles;
    std::vector<std::string> tags;
    double progress;
    std::chrono::system_clock::time_point submitTime;
    std::chrono::system_clock::time_point startTime;
};

/**
 * Runner 状态
 */
struct RunnerStatus {
    RunnerState state = RunnerState::DISCONNECTED;
    std::string language;
    int pid = 0;
    std::string version;
    std::vector<std::string> implementedSteps;
    std::chrono::system_clock::time_point lastHeartbeat;
};

/**
 * 步骤值（从 Runner 获取）
 */
struct StepValue {
    std::string stepText;
    std::string parameterizedStepText;
    std::vector<std::string> parameters;
};

/**
 * 事件
 */
struct Event {
    std::string type;
    std::string testId;
    std::chrono::system_clock::time_point timestamp;
    std::map<std::string, std::string> data;
};

/**
 * 缓存文件请求
 */
struct CacheFileRequest {
    enum class Status {
        OPENED,
        CHANGED,
        CLOSED,
        CREATED,
        DELETED
    };
    
    std::string filePath;
    std::string content;
    Status status;
};

// ============================================================
// 事件类型常量
// ============================================================

namespace EventType {
    const std::string TEST_SUBMITTED = "test.submitted";
    const std::string TEST_STARTED = "test.started";
    const std::string TEST_COMPLETED = "test.completed";
    const std::string TEST_CANCELLED = "test.cancelled";
    const std::string TEST_PROGRESS = "test.progress";
    const std::string SPEC_STARTED = "spec.started";
    const std::string SPEC_COMPLETED = "spec.completed";
    const std::string SCENARIO_STARTED = "scenario.started";
    const std::string SCENARIO_COMPLETED = "scenario.completed";
    const std::string STEP_STARTED = "step.started";
    const std::string STEP_COMPLETED = "step.completed";
    const std::string RUNNER_CONNECTED = "runner.connected";
    const std::string RUNNER_DISCONNECTED = "runner.disconnected";
    const std::string RUNNER_ERROR = "runner.error";
    const std::string QUEUE_UPDATED = "queue.updated";
}

// ============================================================
// 回调类型
// ============================================================

using EventHandler = std::function<void(const Event&)>;
using StepExecutor = std::function<StepResult(const std::string& stepText, const std::vector<std::string>& args)>;

} // namespace testhub
