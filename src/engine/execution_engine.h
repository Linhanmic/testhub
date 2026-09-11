/*
 * TestHub - 测试执行引擎
 * 从队列取任务，加载并过滤规范，驱动 Runner 执行步骤，维护状态与历史结果。
 */

#pragma once

#include "result_store.h"
#include "test_queue.h"
#include "tag_filter.h"
#include "trends.h"
#include "../model/types.h"
#include "../runner/runner_bridge.h"
#include "../spec/spec_repository.h"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace testhub {

/**
 * 引擎配置
 */
struct EngineConfig {
    int workerThreads = 1;              // 并发执行的测试数
    int defaultTimeoutMs = 300000;      // 单个测试的默认超时
    int stepTimeoutMs = 60000;          // 单个步骤超时
    size_t historyLimit = 200;          // 保留的历史结果数量
    std::string resultsDir;             // 结果持久化目录；为空表示仅保存在内存
    std::map<std::string, std::string> environment;  // 传递给 Runner 的环境
};

/**
 * 统计信息
 */
struct EngineStats {
    size_t queued = 0;
    size_t running = 0;
    size_t completed = 0;
    size_t passed = 0;
    size_t failed = 0;
    size_t cancelled = 0;
    size_t errored = 0;
    int totalScenarios = 0;
    int passedScenarios = 0;
    int failedScenarios = 0;
    int skippedScenarios = 0;
    double totalDuration = 0.0;
    TimePoint startedAt;
};

/**
 * 测试记录（请求 + 状态 + 结果）
 */
struct TestRecord {
    TestRequest request;
    TestStatus status;
    TestResult result;
    bool hasResult = false;
    std::vector<std::string> resolvedSpecs;
};

class ExecutionEngine {
public:
    ExecutionEngine(spec::SpecRepository& specs, RunnerBridge& runner);
    ~ExecutionEngine();

    void configure(const EngineConfig& config);
    const EngineConfig& config() const { return config_; }

    void start();
    void stop();
    bool isRunning() const { return running_; }

    /**
     * 提交测试；返回测试 ID。校验失败抛出 std::invalid_argument
     */
    std::string submit(const TestRequest& request);

    /**
     * 取消排队或运行中的测试
     */
    bool cancel(const std::string& testId);

    /**
     * 重新运行已存在的测试（复制请求）；不存在返回空
     */
    std::string rerun(const std::string& testId, bool onlyFailed = false);

    bool exists(const std::string& testId) const;
    std::optional<TestStatus> getStatus(const std::string& testId) const;
    std::optional<TestResult> getResult(const std::string& testId) const;
    std::optional<TestRecord> getRecord(const std::string& testId) const;

    /**
     * 列出测试（按提交时间倒序）
     * @param state 状态过滤（为空表示全部）
     */
    std::vector<TestInfo> list(const std::string& state = "", size_t limit = 100, size_t offset = 0) const;
    size_t count(const std::string& state = "") const;

    /**
     * 删除历史记录（仅终态）
     */
    bool remove(const std::string& testId);
    size_t clearHistory();

    EngineStats stats() const;
    /** 是否有排队中或运行中的测试（含已出队、尚未标为 RUNNING 的窗口） */
    bool hasActiveTests() const;
    const ResultStore& store() const { return store_; }
    size_t queueSize() const { return queue_.size(); }
    int queuePosition(const std::string& testId) const { return queue_.position(testId); }

    /** 终态记录按提交时间升序，供趋势聚合 */
    std::vector<TrendRun> trendRuns() const;
    /**
     * 对比两次终态结果。baselineId 为空则自动选最近一次规范有交集的终态。
     * 找不到时返回 nullopt 并填写 error。
     */
    std::optional<TestComparison> compareTests(const std::string& currentId, const std::string& baselineId,
                                               std::string& error) const;

    TestQueue& queue() { return queue_; }

private:
    spec::SpecRepository& specs_;
    RunnerBridge& runner_;
    EngineConfig config_;
    TestQueue queue_;
    ResultStore store_;
    bool historyLoaded_ = false;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::vector<std::thread> workers_;

    mutable std::mutex recordsMutex_;
    std::map<std::string, TestRecord> records_;
    std::deque<std::string> order_;  // 提交顺序
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> cancelFlags_;

    EngineStats stats_;
    mutable std::mutex statsMutex_;

    struct RunContext {
        std::string testId;
        TestRequest request;
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::chrono::steady_clock::time_point deadline;
        TagFilter tagFilter;
        TestStatus status;
        TestResult result;
        std::atomic<bool> timedOut{false};
        std::atomic<bool> failFastTriggered{false};
        mutable std::recursive_mutex runMutex;  // 并行流下保护 status / errors / 计数

        bool shouldStop() const {
            return cancelled->load() || timedOut.load() || failFastTriggered.load();
        }
    };

    struct LoadedSpec {
        std::string path;
        std::shared_ptr<spec::Specification> specification;
        std::vector<spec::ParseError> errors;
        std::vector<spec::ParseError> warnings;
    };

    void workerLoop();
    void execute(const TestTask& task);
    void executeSequential(RunContext& ctx, std::vector<LoadedSpec>& loaded);
    void executeParallel(RunContext& ctx, std::vector<LoadedSpec>& loaded, const std::vector<int>& slots);
    void applyScenarioOutcome(RunContext& ctx, const ScenarioResult& sr);
    SpecResult executeSpec(RunContext& ctx, const spec::Specification& specification);
    ScenarioResult executeScenario(RunContext& ctx, const spec::Specification& specification,
                                   const spec::Scenario& scenario, int dataRowIndex);
    StepResult executeStep(RunContext& ctx, const spec::Step& step, const ExecutionContext& execCtx,
                           const std::map<std::string, std::string>& dataRow, bool& stopScenario);
    static std::vector<std::string> effectiveTags(const spec::Specification& s, const spec::Scenario& sc);
    static void resolveArgs(std::vector<spec::StepArg>& args, const std::map<std::string, std::string>& dataRow,
                            const std::string& specDir);
    bool scenarioSelected(const RunContext& ctx, const spec::Specification& s, const spec::Scenario& sc) const;

    void updateStatus(const RunContext& ctx);
    void publish(const std::string& type, const std::string& testId, const std::map<std::string, std::string>& data = {});
    void trimHistory();
    void loadHistory();
    // 在终态对外可见之前落盘，保证观察到终态的客户端一定能读到结果文件
    void persist(const TestRecord& record);
    bool checkTimeout(RunContext& ctx);
};

} // namespace testhub
