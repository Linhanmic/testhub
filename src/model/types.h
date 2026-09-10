/*
 * TestHub - 持久化自动化测试系统
 * 数据模型定义
 */

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

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
    SKIPPED,     // 跳过
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
        case TestState::SKIPPED: return "skipped";
        case TestState::CANCELLED: return "cancelled";
        case TestState::TEST_ERROR: return "error";
    }
    return "unknown";
}

inline TestState stringToTestState(const std::string& str) {
    if (str == "queued") return TestState::QUEUED;
    if (str == "running") return TestState::RUNNING;
    if (str == "passed") return TestState::PASSED;
    if (str == "failed") return TestState::FAILED;
    if (str == "skipped") return TestState::SKIPPED;
    if (str == "cancelled") return TestState::CANCELLED;
    return TestState::TEST_ERROR;
}

inline bool isTerminalState(TestState state) {
    return state == TestState::PASSED || state == TestState::FAILED || state == TestState::SKIPPED ||
           state == TestState::CANCELLED || state == TestState::TEST_ERROR;
}

inline std::string priorityToString(Priority p) {
    switch (p) {
        case Priority::LOW: return "low";
        case Priority::NORMAL: return "normal";
        case Priority::HIGH: return "high";
        case Priority::URGENT: return "urgent";
    }
    return "normal";
}

inline Priority stringToPriority(const std::string& str) {
    if (str == "low") return Priority::LOW;
    if (str == "high") return Priority::HIGH;
    if (str == "urgent") return Priority::URGENT;
    return Priority::NORMAL;
}

inline std::string runnerStateToString(RunnerState state) {
    switch (state) {
        case RunnerState::DISCONNECTED: return "disconnected";
        case RunnerState::CONNECTING: return "connecting";
        case RunnerState::CONNECTED: return "connected";
        case RunnerState::BUSY: return "busy";
        case RunnerState::RUNNER_ERROR: return "error";
    }
    return "unknown";
}

using TimePoint = std::chrono::system_clock::time_point;

// ============================================================
// 数据模型
// ============================================================

/**
 * 测试请求
 */
struct TestRequest {
    std::string id;
    std::string name;                        // 可选的人类可读名称
    std::vector<std::string> specFiles;      // 文件或目录（相对于 specs 目录或绝对路径）
    std::vector<std::string> tags;           // 标签过滤表达式（全部匹配）
    std::vector<std::string> scenarios;      // 场景名过滤（任一匹配）
    std::string environment = "default";
    int parallelStreams = 1;
    Priority priority = Priority::NORMAL;
    int timeoutMs = 0;                       // 0 表示使用默认值
    bool failFast = false;                   // 首个场景失败即停止
    std::map<std::string, std::string> metadata;
    std::string callbackUrl;                 // 可选的完成回调 URL
    std::string submittedBy;
};

/**
 * 步骤结果
 */
struct StepResult {
    std::string stepText;
    std::string parameterizedText;
    TestState state = TestState::PASSED;
    std::string errorMessage;
    std::string stackTrace;
    double duration = 0.0;  // 秒
    std::vector<std::string> messages;   // Runner 写回的日志
    std::vector<StepResult> conceptSteps;
    bool isConcept = false;
};

/**
 * 场景结果
 */
struct ScenarioResult {
    std::string scenarioName;
    std::vector<std::string> tags;
    TestState state = TestState::PASSED;
    std::vector<StepResult> contextSteps;
    std::vector<StepResult> stepResults;
    std::vector<StepResult> teardownSteps;
    std::string errorMessage;
    double duration = 0.0;
    int dataRowIndex = -1;  // 数据表驱动时的行号（-1 表示无）
    std::map<std::string, std::string> dataRow;
    int lineNumber = 0;
};

/**
 * 规范结果
 */
struct SpecResult {
    std::string specFile;
    std::string specName;
    std::vector<std::string> tags;
    TestState state = TestState::PASSED;
    std::vector<ScenarioResult> scenarioResults;
    std::string errorMessage;
    double duration = 0.0;
    int totalScenarios = 0;
    int passedScenarios = 0;
    int failedScenarios = 0;
    int skippedScenarios = 0;
};

/**
 * 测试状态（运行中的快照）
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
    int skippedScenarios = 0;
    double progress = 0.0;
    std::string currentSpec;
    std::string currentScenario;
    std::string currentStep;
    TimePoint submitTime;
    TimePoint startTime;
    TimePoint endTime;
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
    TimePoint startTime;
    TimePoint endTime;
    int totalScenarios = 0;
    int passedScenarios = 0;
    int failedScenarios = 0;
    int skippedScenarios = 0;
};

/**
 * 测试信息（用于列表）
 */
struct TestInfo {
    std::string testId;
    std::string name;
    TestState state = TestState::QUEUED;
    std::vector<std::string> specFiles;
    std::vector<std::string> tags;
    Priority priority = Priority::NORMAL;
    double progress = 0.0;
    TimePoint submitTime;
    TimePoint startTime;
    TimePoint endTime;
    int totalScenarios = 0;
    int passedScenarios = 0;
    int failedScenarios = 0;
    int skippedScenarios = 0;
    double duration = 0.0;
};

/**
 * Runner 状态
 */
struct RunnerStatus {
    RunnerState state = RunnerState::DISCONNECTED;
    std::string language;
    std::string command;
    int pid = 0;
    std::string version;
    std::vector<std::string> implementedSteps;
    TimePoint lastHeartbeat;
    TimePoint startedAt;
    int restartCount = 0;
    std::string lastError;
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
    TimePoint timestamp;
    std::map<std::string, std::string> data;
};

// ============================================================
// 事件类型常量
// ============================================================

namespace EventType {
    const std::string SERVER_STARTED = "server.started";
    const std::string SERVER_STOPPING = "server.stopping";
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
    const std::string RUNNER_CONNECTING = "runner.connecting";
    const std::string RUNNER_CONNECTED = "runner.connected";
    const std::string RUNNER_DISCONNECTED = "runner.disconnected";
    const std::string RUNNER_ERROR = "runner.error";
    const std::string RUNNER_LOG = "runner.log";
    const std::string QUEUE_UPDATED = "queue.updated";
    const std::string SPECS_RELOADED = "specs.reloaded";
    const std::string CALLBACK_DELIVERED = "callback.delivered";
    const std::string CALLBACK_FAILED = "callback.failed";
}

// ============================================================
// 回调类型
// ============================================================

using EventHandler = std::function<void(const Event&)>;

} // namespace testhub
