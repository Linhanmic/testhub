/*
 * TestHub - 测试执行引擎实现
 */

#include "execution_engine.h"
#include "../util/file_util.h"
#include "../util/logger.h"
#include "../util/string_util.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace testhub {

namespace {

std::string joinTags(const std::vector<std::string>& tags) { return StringUtil::join(tags, ","); }

TestState aggregateState(int failed, int errored, int skipped, int total, bool cancelled) {
    if (cancelled) return TestState::CANCELLED;
    if (errored > 0 && failed == 0) return TestState::TEST_ERROR;
    if (failed > 0 || errored > 0) return TestState::FAILED;
    if (total > 0 && skipped == total) return TestState::SKIPPED;
    return TestState::PASSED;
}

thread_local int tlsStream = -1;

} // namespace

ExecutionEngine::ExecutionEngine(spec::SpecRepository& specs, RunnerBridge& runner)
    : specs_(specs), runner_(runner) {
    stats_.startedAt = TimeUtil::now();
}

ExecutionEngine::~ExecutionEngine() { stop(); }

void ExecutionEngine::configure(const EngineConfig& config) {
    config_ = config;
    if (config_.workerThreads < 1) config_.workerThreads = 1;
    if (config_.historyLimit < 1) config_.historyLimit = 1;
    if (store_.dir() != config_.resultsDir) {
        store_ = ResultStore(config_.resultsDir);
        historyLoaded_ = false;
    }
}

void ExecutionEngine::loadHistory() {
    if (historyLoaded_) return;
    historyLoaded_ = true;
    if (!store_.enabled()) return;
    store_.prepare();
    std::vector<TestRecord> records = store_.loadAll();
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        for (auto& r : records) {
            const std::string id = r.status.testId;
            if (records_.count(id)) continue;
            records_[id] = std::move(r);
            order_.push_back(id);
        }
    }
    {
        // 历史记录计入统计，使总览页在重启后仍反映累计情况
        std::lock_guard<std::mutex> lock(statsMutex_);
        for (const auto& r : records) {
            if (!r.hasResult) continue;
            stats_.completed++;
            switch (r.result.finalState) {
                case TestState::PASSED: case TestState::SKIPPED: stats_.passed++; break;
                case TestState::FAILED: stats_.failed++; break;
                case TestState::CANCELLED: stats_.cancelled++; break;
                default: stats_.errored++; break;
            }
            stats_.totalScenarios += r.result.totalScenarios;
            stats_.passedScenarios += r.result.passedScenarios;
            stats_.failedScenarios += r.result.failedScenarios;
            stats_.skippedScenarios += r.result.skippedScenarios;
            stats_.totalDuration += r.result.totalDuration;
        }
    }
    trimHistory();
}

void ExecutionEngine::persist(const TestRecord& record) {
    if (!store_.enabled()) return;
    if (!isTerminalState(record.status.state)) return;
    store_.save(record);
}

void ExecutionEngine::start() {
    if (running_) return;
    stopRequested_ = false;
    loadHistory();
    queue_.reopen();
    running_ = true;
    for (int i = 0; i < config_.workerThreads; ++i) {
        workers_.emplace_back(&ExecutionEngine::workerLoop, this);
    }
    TH_LOG_INFO("engine", "Execution engine started with " + std::to_string(config_.workerThreads) + " worker(s)");
}

void ExecutionEngine::stop() {
    if (!running_) return;
    stopRequested_ = true;
    // 取消所有运行中的测试
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        for (auto& kv : cancelFlags_) kv.second->store(true);
    }
    queue_.close();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
    running_ = false;
    TH_LOG_INFO("engine", "Execution engine stopped");
}

void ExecutionEngine::publish(const std::string& type, const std::string& testId,
                              const std::map<std::string, std::string>& data) {
    publishEvent(type, testId, data);
}

// ============================================================
// 提交 / 取消 / 查询
// ============================================================

std::string ExecutionEngine::submit(const TestRequest& input) {
    TestRequest request = input;
    std::vector<std::string> missing;
    std::vector<std::string> resolved = specs_.resolve(request.specFiles, &missing);
    if (!missing.empty()) {
        throw std::invalid_argument("Spec path(s) not found: " + StringUtil::join(missing, ", "));
    }
    if (resolved.empty()) {
        throw std::invalid_argument("No spec files found for the given paths");
    }
    std::string tagError;
    TagFilter filter = TagFilter::all(request.tags, &tagError);
    if (!tagError.empty()) {
        throw std::invalid_argument("Invalid tag expression: " + tagError);
    }
    if (!request.id.empty()) {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        if (records_.count(request.id)) throw std::invalid_argument("Test ID already exists: " + request.id);
    }

    TestRecord record;
    record.request = request;
    record.resolvedSpecs = resolved;
    record.status.state = TestState::QUEUED;
    record.status.submitTime = TimeUtil::now();
    record.status.totalSpecs = static_cast<int>(resolved.size());

    std::string testId;
    {
        // 先占位再入队，避免 worker 取到任务时找不到记录
        std::lock_guard<std::mutex> lock(recordsMutex_);
        if (request.id.empty()) {
            request.id = queue_.generateTaskId(record.status.submitTime);
            while (records_.count(request.id)) request.id = queue_.generateTaskId(record.status.submitTime);
        }
        testId = request.id;
        record.request.id = testId;
        record.status.testId = testId;
        records_[testId] = record;
        order_.push_back(testId);
        cancelFlags_[testId] = std::make_shared<std::atomic<bool>>(false);
    }
    trimHistory();

    queue_.enqueue(record.request);
    publish(EventType::TEST_SUBMITTED, testId, {
        {"spec_files", StringUtil::join(request.specFiles, ",")},
        {"resolved_specs", std::to_string(resolved.size())},
        {"tags", joinTags(request.tags)},
        {"priority", priorityToString(request.priority)},
        {"name", request.name}
    });
    TH_LOG_INFO("engine", "Submitted " + testId + " (" + std::to_string(resolved.size()) + " spec(s))");
    return testId;
}

bool ExecutionEngine::cancel(const std::string& testId) {
    bool wasQueued = queue_.cancel(testId);
    std::shared_ptr<std::atomic<bool>> flag;
    TestRecord cancelled;
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        auto it = records_.find(testId);
        if (it == records_.end()) return false;
        if (isTerminalState(it->second.status.state)) return false;
        if (wasQueued) {
            // 已出队，不会再有工作线程执行它：先在副本上构造终态并落盘，再发布到 records_
            cancelled = it->second;
            cancelled.status.state = TestState::CANCELLED;
            cancelled.status.endTime = TimeUtil::now();
            cancelled.result.testId = testId;
            cancelled.result.finalState = TestState::CANCELLED;
            cancelled.result.startTime = cancelled.status.submitTime;
            cancelled.result.endTime = cancelled.status.endTime;
            cancelled.hasResult = true;
        } else {
            auto f = cancelFlags_.find(testId);
            if (f != cancelFlags_.end()) flag = f->second;
        }
    }
    if (wasQueued) {
        persist(cancelled);
        {
            std::lock_guard<std::mutex> lock(recordsMutex_);
            auto it = records_.find(testId);
            if (it != records_.end()) it->second = cancelled;
            cancelFlags_.erase(testId);
        }
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.completed++;
            stats_.cancelled++;
        }
        publish(EventType::TEST_CANCELLED, testId, {{"stage", "queued"}});
        return true;
    }
    if (flag) {
        flag->store(true);
        publish(EventType::TEST_CANCELLED, testId, {{"stage", "running"}});
        return true;
    }
    return false;
}

std::string ExecutionEngine::rerun(const std::string& testId, bool onlyFailed) {
    TestRequest request;
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        auto it = records_.find(testId);
        if (it == records_.end()) return "";
        request = it->second.request;
        if (onlyFailed && it->second.hasResult) {
            std::vector<std::string> failedSpecs;
            for (const auto& s : it->second.result.specResults) {
                if (s.state == TestState::FAILED || s.state == TestState::TEST_ERROR) failedSpecs.push_back(s.specFile);
            }
            if (!failedSpecs.empty()) request.specFiles = failedSpecs;
        }
    }
    request.id.clear();
    request.metadata["rerun_of"] = testId;
    return submit(request);
}

bool ExecutionEngine::exists(const std::string& testId) const {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    return records_.count(testId) > 0;
}

std::optional<TestStatus> ExecutionEngine::getStatus(const std::string& testId) const {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    auto it = records_.find(testId);
    if (it == records_.end()) return std::nullopt;
    return it->second.status;
}

std::optional<TestResult> ExecutionEngine::getResult(const std::string& testId) const {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    auto it = records_.find(testId);
    if (it == records_.end() || !it->second.hasResult) return std::nullopt;
    return it->second.result;
}

std::optional<TestRecord> ExecutionEngine::getRecord(const std::string& testId) const {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    auto it = records_.find(testId);
    if (it == records_.end()) return std::nullopt;
    return it->second;
}

std::vector<TestInfo> ExecutionEngine::list(const std::string& state, size_t limit, size_t offset) const {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    std::vector<TestInfo> out;
    size_t skipped = 0;
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        auto rit = records_.find(*it);
        if (rit == records_.end()) continue;
        const TestRecord& r = rit->second;
        if (!state.empty()) {
            std::string s = testStateToString(r.status.state);
            if (state == "active") {
                if (r.status.state != TestState::QUEUED && r.status.state != TestState::RUNNING) continue;
            } else if (state == "finished") {
                if (!isTerminalState(r.status.state)) continue;
            } else if (s != state) {
                continue;
            }
        }
        if (skipped < offset) { ++skipped; continue; }
        if (out.size() >= limit) break;
        TestInfo info;
        info.testId = r.request.id;
        info.name = r.request.name;
        info.state = r.status.state;
        info.specFiles = r.request.specFiles;
        info.tags = r.request.tags;
        info.priority = r.request.priority;
        info.progress = r.status.progress;
        info.submitTime = r.status.submitTime;
        info.startTime = r.status.startTime;
        info.endTime = r.status.endTime;
        info.totalScenarios = r.hasResult ? r.result.totalScenarios : r.status.totalScenarios;
        info.passedScenarios = r.hasResult ? r.result.passedScenarios : r.status.passedScenarios;
        info.failedScenarios = r.hasResult ? r.result.failedScenarios : r.status.failedScenarios;
        info.skippedScenarios = r.hasResult ? r.result.skippedScenarios : r.status.skippedScenarios;
        info.duration = r.hasResult ? r.result.totalDuration : 0.0;
        out.push_back(info);
    }
    return out;
}

size_t ExecutionEngine::count(const std::string& state) const {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    if (state.empty()) return records_.size();
    size_t n = 0;
    for (const auto& kv : records_) {
        if (testStateToString(kv.second.status.state) == state) ++n;
    }
    return n;
}

bool ExecutionEngine::remove(const std::string& testId) {
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        auto it = records_.find(testId);
        if (it == records_.end() || !isTerminalState(it->second.status.state)) return false;
        records_.erase(it);
        order_.erase(std::remove(order_.begin(), order_.end(), testId), order_.end());
        cancelFlags_.erase(testId);
    }
    store_.remove(testId);
    return true;
}

size_t ExecutionEngine::clearHistory() {
    std::vector<std::string> removedIds;
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        for (auto it = order_.begin(); it != order_.end();) {
            auto rit = records_.find(*it);
            if (rit != records_.end() && isTerminalState(rit->second.status.state)) {
                records_.erase(rit);
                cancelFlags_.erase(*it);
                removedIds.push_back(*it);
                it = order_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& id : removedIds) store_.remove(id);
    return removedIds.size();
}

void ExecutionEngine::trimHistory() {
    std::vector<std::string> removedIds;
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        while (order_.size() > config_.historyLimit) {
            // 移除最早的终态记录
            bool removed = false;
            for (auto it = order_.begin(); it != order_.end(); ++it) {
                auto rit = records_.find(*it);
                if (rit == records_.end()) { order_.erase(it); removed = true; break; }
                if (isTerminalState(rit->second.status.state)) {
                    removedIds.push_back(*it);
                    records_.erase(rit);
                    cancelFlags_.erase(*it);
                    order_.erase(it);
                    removed = true;
                    break;
                }
            }
            if (!removed) break;
        }
    }
    for (const auto& id : removedIds) store_.remove(id);
}

EngineStats ExecutionEngine::stats() const {
    EngineStats s;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        s = stats_;
    }
    s.queued = queue_.size();
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        s.running = 0;
        for (const auto& kv : records_) {
            if (kv.second.status.state == TestState::RUNNING) ++s.running;
        }
    }
    return s;
}

// ============================================================
// 执行
// ============================================================

void ExecutionEngine::workerLoop() {
    while (!stopRequested_) {
        auto task = queue_.waitAndDequeue(250);
        if (!task) continue;
        if (stopRequested_) {
            // 停机时把任务标记为取消
            std::lock_guard<std::mutex> lock(recordsMutex_);
            auto it = records_.find(task->request.id);
            if (it != records_.end()) {
                it->second.status.state = TestState::CANCELLED;
                it->second.status.endTime = TimeUtil::now();
            }
            continue;
        }
        try {
            execute(*task);
        } catch (const std::exception& e) {
            TH_LOG_ERROR("engine", "Unhandled exception while executing " + task->request.id + ": " + e.what());
            std::lock_guard<std::mutex> lock(recordsMutex_);
            auto it = records_.find(task->request.id);
            if (it != records_.end()) {
                it->second.status.state = TestState::TEST_ERROR;
                it->second.status.errors.push_back(e.what());
                it->second.status.endTime = TimeUtil::now();
                it->second.result.finalState = TestState::TEST_ERROR;
                it->second.result.errors.push_back(e.what());
                it->second.hasResult = true;
            }
        }
    }
}

bool ExecutionEngine::checkTimeout(RunContext& ctx) {
    if (ctx.timedOut.load()) return true;
    if (std::chrono::steady_clock::now() >= ctx.deadline) {
        bool already = false;
        {
            std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
            already = ctx.timedOut.exchange(true);
            if (!already) ctx.result.errors.push_back("Test timed out");
        }
        return true;
    }
    return false;
}

void ExecutionEngine::updateStatus(const RunContext& ctx) {
    TestStatus snap;
    {
        std::lock_guard<std::recursive_mutex> run(ctx.runMutex);
        snap = ctx.status;
    }
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        auto it = records_.find(ctx.testId);
        if (it != records_.end()) it->second.status = snap;
    }
    publish(EventType::TEST_PROGRESS, ctx.testId, {
        {"progress", std::to_string(snap.progress)},
        {"current_spec", snap.currentSpec},
        {"current_scenario", snap.currentScenario},
        {"current_step", snap.currentStep},
        {"executed_scenarios", std::to_string(snap.executedScenarios)},
        {"total_scenarios", std::to_string(snap.totalScenarios)},
        {"passed_scenarios", std::to_string(snap.passedScenarios)},
        {"failed_scenarios", std::to_string(snap.failedScenarios)}
    });
}

void ExecutionEngine::applyScenarioOutcome(RunContext& ctx, const ScenarioResult& sr) {
    std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
    switch (sr.state) {
        case TestState::PASSED: ctx.status.passedScenarios++; ctx.status.executedScenarios++; break;
        case TestState::FAILED: ctx.status.failedScenarios++; ctx.status.executedScenarios++; break;
        case TestState::TEST_ERROR: ctx.status.failedScenarios++; ctx.status.executedScenarios++; break;
        case TestState::CANCELLED: ctx.status.skippedScenarios++; break;
        default: ctx.status.skippedScenarios++; break;
    }
    int processed = ctx.status.executedScenarios + ctx.status.skippedScenarios;
    if (ctx.status.totalScenarios > 0) {
        ctx.status.progress = std::min(1.0, static_cast<double>(processed) / ctx.status.totalScenarios);
    }
    if (ctx.request.failFast && (sr.state == TestState::FAILED || sr.state == TestState::TEST_ERROR)) {
        ctx.failFastTriggered.store(true);
    }
    updateStatus(ctx);
}

std::vector<std::string> ExecutionEngine::effectiveTags(const spec::Specification& s, const spec::Scenario& sc) {
    std::vector<std::string> tags = s.tags;
    for (const auto& t : sc.tags) {
        if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
    }
    return tags;
}

bool ExecutionEngine::scenarioSelected(const RunContext& ctx, const spec::Specification& s, const spec::Scenario& sc) const {
    if (!ctx.tagFilter.matches(effectiveTags(s, sc))) return false;
    if (!ctx.request.scenarios.empty()) {
        std::string lowerName = StringUtil::toLower(sc.name);
        bool any = false;
        for (const auto& want : ctx.request.scenarios) {
            std::string w = StringUtil::toLower(StringUtil::trim(want));
            if (w.empty()) continue;
            if (lowerName == w || StringUtil::contains(lowerName, w)) { any = true; break; }
        }
        if (!any) return false;
    }
    return true;
}

void ExecutionEngine::resolveArgs(std::vector<spec::StepArg>& args, const std::map<std::string, std::string>& dataRow,
                                  const std::string& specDir) {
    for (auto& a : args) {
        if (a.type == spec::ArgType::Dynamic) {
            auto it = dataRow.find(a.value);
            if (it != dataRow.end()) {
                a.type = spec::ArgType::Static;
                a.value = it->second;
            }
        } else if (a.type == spec::ArgType::SpecialString) {
            std::string path = a.value;
            if (!std::filesystem::path(path).is_absolute()) path = FileUtil::joinPath(specDir, path);
            if (FileUtil::fileExists(path)) {
                a.type = spec::ArgType::Static;
                a.value = FileUtil::readFile(path);
            }
        } else if (a.type == spec::ArgType::SpecialTable) {
            std::string path = a.value;
            if (!std::filesystem::path(path).is_absolute()) path = FileUtil::joinPath(specDir, path);
            if (FileUtil::fileExists(path)) {
                // CSV: 首行表头
                spec::Table table;
                std::string content = FileUtil::readFile(path);
                bool header = true;
                for (auto& line : StringUtil::split(content, "\n")) {
                    std::string t = StringUtil::trim(line);
                    if (t.empty()) continue;
                    std::vector<std::string> cells;
                    for (auto& c : StringUtil::split(t, ",")) cells.push_back(StringUtil::trim(c));
                    if (header) { table.headers = cells; header = false; }
                    else table.rows.push_back(cells);
                }
                a.type = spec::ArgType::Table;
                a.table = table;
            }
        }
    }
}

void ExecutionEngine::execute(const TestTask& task) {
    RunContext ctx;
    ctx.testId = task.request.id;
    ctx.request = task.request;

    std::vector<std::string> resolved;
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        auto it = records_.find(ctx.testId);
        if (it == records_.end()) {
            // 记录可能已被清理（例如提交后立即被删除）
            return;
        }
        if (it->second.status.state == TestState::CANCELLED) return;
        ctx.cancelled = cancelFlags_[ctx.testId];
        if (!ctx.cancelled) ctx.cancelled = cancelFlags_[ctx.testId] = std::make_shared<std::atomic<bool>>(false);
        resolved = it->second.resolvedSpecs;
        ctx.status = it->second.status;
    }

    // 先解析规范（不占用 Runner），再按 parallel_streams 原子预约槽位。
    // 预约期间状态仍为 queued，避免"先拿 1 个再等其余"造成的池死锁。
    int timeoutMs = ctx.request.timeoutMs > 0 ? ctx.request.timeoutMs : config_.defaultTimeoutMs;
    ctx.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    ctx.tagFilter = TagFilter::all(ctx.request.tags);

    std::vector<LoadedSpec> loaded;
    for (const auto& path : resolved) {
        LoadedSpec l;
        l.path = path;
        spec::ParseResult pr = specs_.load(path);
        l.specification = pr.specification;
        l.errors = pr.errors;
        l.warnings = pr.warnings;
        for (const auto& w : pr.warnings) {
            ctx.result.warnings.push_back(w.fileName + ":" + std::to_string(w.lineNumber) + ": " + w.message);
        }
        if (l.specification && l.errors.empty()) {
            int rows = l.specification->isDataDriven() ? static_cast<int>(l.specification->dataTable.rowCount()) : 1;
            for (const auto& sc : l.specification->scenarios) {
                if (scenarioSelected(ctx, *l.specification, sc)) ctx.status.totalScenarios += rows;
            }
        }
        loaded.push_back(std::move(l));
    }
    ctx.status.warnings = ctx.result.warnings;
    ctx.status.totalSpecs = static_cast<int>(resolved.size());

    int streams = ctx.request.parallelStreams < 1 ? 1 : ctx.request.parallelStreams;
    int cap = runner_.isConcurrencySafe() ? RunnerBridge::kMaxPoolSize : runner_.poolSize();
    if (cap < 1) cap = 1;
    if (streams > cap) streams = cap;
    if (ctx.status.totalScenarios > 0 && streams > ctx.status.totalScenarios) streams = ctx.status.totalScenarios;
    if (streams < 1) streams = 1;

    std::vector<int> reserved = runner_.reserveSlots(streams);
    if (reserved.empty()) reserved = runner_.reserveSlots(1);
    if (static_cast<int>(reserved.size()) < streams) streams = reserved.empty() ? 1 : static_cast<int>(reserved.size());

    ctx.status.state = TestState::RUNNING;
    ctx.status.startTime = TimeUtil::now();
    ctx.result.testId = ctx.testId;
    ctx.result.startTime = ctx.status.startTime;
    updateStatus(ctx);
    publish(EventType::TEST_STARTED, ctx.testId, {
        {"total_specs", std::to_string(ctx.status.totalSpecs)},
        {"total_scenarios", std::to_string(ctx.status.totalScenarios)},
        {"name", ctx.request.name},
        {"streams", std::to_string(streams)}
    });
    TH_LOG_INFO("engine", "Executing " + ctx.testId + ": " + std::to_string(ctx.status.totalSpecs) + " spec(s), " +
                          std::to_string(ctx.status.totalScenarios) + " scenario(s), " +
                          std::to_string(streams) + " stream(s)");

    if (streams <= 1) {
        RunnerBridge::Session session = reserved.empty() ? runner_.acquireSession()
                                                         : runner_.attachReserved(reserved[0]);
        executeSequential(ctx, loaded);
    } else {
        executeParallel(ctx, loaded, reserved);
    }

    ctx.result.endTime = TimeUtil::now();
    ctx.result.totalDuration = std::chrono::duration<double>(ctx.result.endTime - ctx.result.startTime).count();
    ctx.result.totalScenarios = ctx.status.totalScenarios;
    ctx.result.passedScenarios = ctx.status.passedScenarios;
    ctx.result.failedScenarios = ctx.status.failedScenarios;
    ctx.result.skippedScenarios = ctx.status.skippedScenarios;

    int passedSpecs = 0, failedSpecs = 0, erroredSpecs = 0;
    bool suiteFailed = false;
    for (const auto& e : ctx.result.errors) {
        if (e.find("before_suite") != std::string::npos) suiteFailed = true;
    }
    for (const auto& s : ctx.result.specResults) {
        if (s.state == TestState::TEST_ERROR) erroredSpecs++;
        if (s.state == TestState::PASSED || s.state == TestState::SKIPPED) passedSpecs++;
        else if (s.state != TestState::CANCELLED) failedSpecs++;
    }
    if (ctx.cancelled->load()) {
        ctx.result.finalState = TestState::CANCELLED;
    } else if (ctx.timedOut.load()) {
        ctx.result.finalState = TestState::TEST_ERROR;
    } else if (suiteFailed) {
        ctx.result.finalState = TestState::TEST_ERROR;
    } else {
        ctx.result.finalState = aggregateState(failedSpecs - erroredSpecs, erroredSpecs,
                                               0, ctx.status.totalSpecs, false);
        if (ctx.result.finalState == TestState::PASSED &&
            ctx.status.passedScenarios == 0 && ctx.status.failedScenarios == 0) {
            // 没有任何场景真正执行：不能算通过，避免 CI 把“什么都没跑”当成成功
            ctx.result.finalState = TestState::SKIPPED;
            if (ctx.status.totalScenarios == 0) {
                ctx.result.warnings.push_back("No scenarios matched the requested specs/tags/scenario filters");
            }
        }
    }

    ctx.status.state = ctx.result.finalState;
    ctx.status.endTime = ctx.result.endTime;
    ctx.status.progress = 1.0;
    ctx.status.currentSpec.clear();
    ctx.status.currentScenario.clear();
    ctx.status.currentStep.clear();
    ctx.status.errors = ctx.result.errors;
    ctx.status.warnings = ctx.result.warnings;

    {
        // 先落盘再对外可见：轮询到终态的客户端随即读取结果文件/报表时不会落空
        TestRecord finished;
        {
            std::lock_guard<std::mutex> lock(recordsMutex_);
            auto it = records_.find(ctx.testId);
            if (it != records_.end()) finished = it->second;
        }
        finished.status = ctx.status;
        finished.result = ctx.result;
        finished.hasResult = true;
        persist(finished);
        std::lock_guard<std::mutex> lock(recordsMutex_);
        auto it = records_.find(ctx.testId);
        if (it != records_.end()) {
            it->second.status = ctx.status;
            it->second.result = ctx.result;
            it->second.hasResult = true;
        }
        cancelFlags_.erase(ctx.testId);
    }
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.completed++;
        switch (ctx.result.finalState) {
            case TestState::PASSED: case TestState::SKIPPED: stats_.passed++; break;
            case TestState::FAILED: stats_.failed++; break;
            case TestState::CANCELLED: stats_.cancelled++; break;
            default: stats_.errored++; break;
        }
        stats_.totalScenarios += ctx.result.totalScenarios;
        stats_.passedScenarios += ctx.result.passedScenarios;
        stats_.failedScenarios += ctx.result.failedScenarios;
        stats_.skippedScenarios += ctx.result.skippedScenarios;
        stats_.totalDuration += ctx.result.totalDuration;
    }

    publish(EventType::TEST_COMPLETED, ctx.testId, {
        {"state", testStateToString(ctx.result.finalState)},
        {"duration", std::to_string(ctx.result.totalDuration)},
        {"total_scenarios", std::to_string(ctx.result.totalScenarios)},
        {"passed_scenarios", std::to_string(ctx.result.passedScenarios)},
        {"failed_scenarios", std::to_string(ctx.result.failedScenarios)},
        {"skipped_scenarios", std::to_string(ctx.result.skippedScenarios)},
        {"passed_specs", std::to_string(passedSpecs)},
        {"failed_specs", std::to_string(failedSpecs)},
        {"name", ctx.request.name}
    });
    TH_LOG_INFO("engine", "Test " + ctx.testId + " completed: " + testStateToString(ctx.result.finalState) +
                          " (" + std::to_string(ctx.result.passedScenarios) + "/" +
                          std::to_string(ctx.result.totalScenarios) + " scenarios passed)");
}

void ExecutionEngine::executeSequential(RunContext& ctx, std::vector<LoadedSpec>& loaded) {
    ExecutionContext suiteCtx;
    suiteCtx.testId = ctx.testId;
    suiteCtx.environment = config_.environment;
    suiteCtx.environment["TESTHUB_ENVIRONMENT"] = ctx.request.environment;
    HookResult beforeSuite = runner_.runHook(HookType::BeforeSuite, suiteCtx);
    if (!beforeSuite.success) {
        std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
        ctx.result.errors.push_back("before_suite hook failed: " + beforeSuite.errorMessage);
    }

    int passedSpecs = 0, failedSpecs = 0;
    for (auto& l : loaded) {
        if (ctx.shouldStop() || checkTimeout(ctx) || !beforeSuite.success) {
            SpecResult sr;
            sr.specFile = specs_.toRelative(l.path);
            sr.specName = l.specification ? l.specification->heading : sr.specFile;
            sr.state = ctx.cancelled->load() ? TestState::CANCELLED : TestState::SKIPPED;
            sr.errorMessage = !beforeSuite.success ? "Skipped because before_suite hook failed"
                              : ctx.timedOut.load() ? "Skipped because the test timed out"
                              : ctx.failFastTriggered.load() ? "Skipped due to fail_fast" : "Skipped because the test was cancelled";
            ctx.result.specResults.push_back(sr);
            continue;
        }
        {
            std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
            ctx.status.currentSpec = specs_.toRelative(l.path);
            ctx.status.currentScenario.clear();
            ctx.status.currentStep.clear();
        }
        updateStatus(ctx);

        SpecResult sr;
        if (!l.specification || !l.errors.empty()) {
            sr.specFile = specs_.toRelative(l.path);
            sr.specName = l.specification ? l.specification->heading : sr.specFile;
            sr.state = TestState::TEST_ERROR;
            std::vector<std::string> msgs;
            for (const auto& e : l.errors) {
                msgs.push_back(e.fileName + ":" + std::to_string(e.lineNumber) + ": " + e.message);
            }
            sr.errorMessage = msgs.empty() ? "Failed to load spec" : StringUtil::join(msgs, "\n");
            ctx.result.errors.push_back(sr.errorMessage);
            publish(EventType::SPEC_COMPLETED, ctx.testId, {{"spec", sr.specFile}, {"state", "error"}, {"error", sr.errorMessage}});
        } else {
            sr = executeSpec(ctx, *l.specification);
        }
        ctx.result.specResults.push_back(sr);
        if (sr.state == TestState::PASSED || sr.state == TestState::SKIPPED) passedSpecs++;
        else if (sr.state != TestState::CANCELLED) failedSpecs++;
        ctx.status.executedSpecs++;
        ctx.status.passedSpecs = passedSpecs;
        ctx.status.failedSpecs = failedSpecs;
        updateStatus(ctx);
    }

    HookResult afterSuite = runner_.runHook(HookType::AfterSuite, suiteCtx);
    if (!afterSuite.success) {
        std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
        ctx.result.errors.push_back("after_suite hook failed: " + afterSuite.errorMessage);
    }
}

void ExecutionEngine::executeParallel(RunContext& ctx, std::vector<LoadedSpec>& loaded, const std::vector<int>& slots) {
    struct Work {
        size_t specIndex = 0;
        const spec::Scenario* scenario = nullptr;
        int row = -1;
        size_t order = 0;
    };

    std::vector<Work> items;
    std::vector<SpecResult> specResults(loaded.size());
    for (size_t i = 0; i < loaded.size(); ++i) {
        auto& l = loaded[i];
        specResults[i].specFile = specs_.toRelative(l.path);
        specResults[i].specName = l.specification ? l.specification->heading : specResults[i].specFile;
        if (l.specification) specResults[i].tags = l.specification->tags;
        if (!l.specification || !l.errors.empty()) {
            specResults[i].state = TestState::TEST_ERROR;
            std::vector<std::string> msgs;
            for (const auto& e : l.errors) {
                msgs.push_back(e.fileName + ":" + std::to_string(e.lineNumber) + ": " + e.message);
            }
            specResults[i].errorMessage = msgs.empty() ? "Failed to load spec" : StringUtil::join(msgs, "\n");
            std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
            ctx.result.errors.push_back(specResults[i].errorMessage);
            publish(EventType::SPEC_COMPLETED, ctx.testId,
                    {{"spec", specResults[i].specFile}, {"state", "error"}, {"error", specResults[i].errorMessage}});
            continue;
        }
        int rows = l.specification->isDataDriven() ? static_cast<int>(l.specification->dataTable.rowCount()) : 1;
        for (int row = 0; row < rows; ++row) {
            for (const auto& scenario : l.specification->scenarios) {
                if (!scenarioSelected(ctx, *l.specification, scenario)) continue;
                Work w;
                w.specIndex = i;
                w.scenario = &scenario;
                w.row = l.specification->isDataDriven() ? row : -1;
                w.order = items.size();
                items.push_back(w);
            }
        }
    }

    const int n = static_cast<int>(slots.size());
    std::vector<std::vector<Work>> buckets(static_cast<size_t>(n));
    for (size_t i = 0; i < items.size(); ++i) buckets[i % static_cast<size_t>(n)].push_back(items[i]);

    std::vector<std::vector<std::pair<size_t, ScenarioResult>>> collected(loaded.size());
    std::mutex collectMutex;
    std::vector<std::string> specHookErrors(loaded.size());
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(n));

    for (int stream = 0; stream < n; ++stream) {
        threads.emplace_back([this, &ctx, &loaded, &slots, &buckets, &collected, &collectMutex, &specHookErrors, stream] {
            tlsStream = stream;
            RunnerBridge::Session session = runner_.attachReserved(slots[static_cast<size_t>(stream)]);
            ExecutionContext suiteCtx;
            suiteCtx.testId = ctx.testId;
            suiteCtx.environment = config_.environment;
            suiteCtx.environment["TESTHUB_ENVIRONMENT"] = ctx.request.environment;
            HookResult beforeSuite = runner_.runHook(HookType::BeforeSuite, suiteCtx);
            if (!beforeSuite.success) {
                std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
                ctx.result.errors.push_back("before_suite hook failed on stream " + std::to_string(stream) +
                                            ": " + beforeSuite.errorMessage);
            }

            // 按规范下标分组，保持每个流上 before_spec → 场景 → after_spec 的 Gauge 语义
            std::map<size_t, std::vector<Work>> bySpec;
            for (const auto& w : buckets[static_cast<size_t>(stream)]) bySpec[w.specIndex].push_back(w);

            for (auto& kv : bySpec) {
                size_t specIndex = kv.first;
                const LoadedSpec& l = loaded[specIndex];
                if (!l.specification) continue;
                if (beforeSuite.success == false) {
                    for (const auto& w : kv.second) {
                        ScenarioResult sr;
                        sr.scenarioName = w.scenario->name;
                        sr.tags = effectiveTags(*l.specification, *w.scenario);
                        sr.lineNumber = w.scenario->lineNumber;
                        sr.dataRowIndex = w.row;
                        sr.state = TestState::SKIPPED;
                        sr.errorMessage = "Skipped because before_suite hook failed";
                        applyScenarioOutcome(ctx, sr);
                        std::lock_guard<std::mutex> lock(collectMutex);
                        collected[specIndex].push_back({w.order, std::move(sr)});
                    }
                    continue;
                }

                ExecutionContext specCtx;
                specCtx.testId = ctx.testId;
                specCtx.specFile = l.specification->fileName;
                specCtx.specName = l.specification->heading;
                specCtx.tags = l.specification->tags;
                specCtx.environment = config_.environment;
                {
                    std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
                    ctx.status.currentSpec = l.specification->fileName;
                }
                publish(EventType::SPEC_STARTED, ctx.testId, {
                    {"spec", l.specification->fileName},
                    {"name", l.specification->heading},
                    {"stream", std::to_string(stream)},
                    {"scenarios", std::to_string(kv.second.size())},
                    {"tags", joinTags(l.specification->tags)}
                });
                HookResult beforeSpec = runner_.runHook(HookType::BeforeSpec, specCtx);
                if (!beforeSpec.success) {
                    std::lock_guard<std::mutex> lock(collectMutex);
                    if (specHookErrors[specIndex].empty()) {
                        specHookErrors[specIndex] = "before_spec hook failed: " + beforeSpec.errorMessage;
                    }
                }
                for (const auto& w : kv.second) {
                    ScenarioResult sr;
                    if (!beforeSpec.success || ctx.shouldStop() || checkTimeout(ctx)) {
                        sr.scenarioName = w.scenario->name;
                        sr.tags = effectiveTags(*l.specification, *w.scenario);
                        sr.lineNumber = w.scenario->lineNumber;
                        sr.dataRowIndex = w.row;
                        sr.state = ctx.cancelled->load() ? TestState::CANCELLED : TestState::SKIPPED;
                        if (!beforeSpec.success) sr.errorMessage = "Skipped because before_spec hook failed";
                        else if (ctx.cancelled->load()) sr.errorMessage = "Skipped because the test was cancelled";
                        else if (ctx.timedOut.load()) sr.errorMessage = "Skipped because the test timed out";
                        else sr.errorMessage = "Skipped due to fail_fast";
                    } else {
                        {
                            std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
                            ctx.status.currentScenario = w.scenario->name;
                        }
                        sr = executeScenario(ctx, *l.specification, *w.scenario, w.row);
                    }
                    applyScenarioOutcome(ctx, sr);
                    std::lock_guard<std::mutex> lock(collectMutex);
                    collected[specIndex].push_back({w.order, std::move(sr)});
                }
                HookResult afterSpec = runner_.runHook(HookType::AfterSpec, specCtx);
                if (!afterSpec.success) {
                    std::lock_guard<std::mutex> lock(collectMutex);
                    specHookErrors[specIndex] += std::string(specHookErrors[specIndex].empty() ? "" : "\n") +
                                                 "after_spec hook failed: " + afterSpec.errorMessage;
                }
            }

            HookResult afterSuite = runner_.runHook(HookType::AfterSuite, suiteCtx);
            if (!afterSuite.success) {
                std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
                ctx.result.errors.push_back("after_suite hook failed on stream " + std::to_string(stream) +
                                            ": " + afterSuite.errorMessage);
            }
            tlsStream = -1;
        });
    }
    for (auto& t : threads) t.join();

    int passedSpecs = 0, failedSpecs = 0;
    for (size_t i = 0; i < loaded.size(); ++i) {
        SpecResult& sr = specResults[i];
        if (sr.state != TestState::TEST_ERROR) {
            std::sort(collected[i].begin(), collected[i].end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            int failed = 0, errored = 0, skipped = 0, passed = 0;
            double specDur = 0;
            for (auto& item : collected[i]) {
                switch (item.second.state) {
                    case TestState::PASSED: passed++; break;
                    case TestState::FAILED: failed++; break;
                    case TestState::TEST_ERROR: errored++; break;
                    default: skipped++; break;
                }
                if (item.second.duration > specDur) specDur = item.second.duration;
                sr.scenarioResults.push_back(std::move(item.second));
            }
            sr.totalScenarios = static_cast<int>(sr.scenarioResults.size());
            sr.passedScenarios = passed;
            sr.failedScenarios = failed + errored;
            sr.skippedScenarios = skipped;
            if (!specHookErrors[i].empty()) {
                sr.errorMessage = specHookErrors[i];
                sr.state = TestState::TEST_ERROR;
            } else if (sr.totalScenarios == 0) {
                sr.state = TestState::SKIPPED;
            } else {
                sr.state = aggregateState(failed, errored, skipped, sr.totalScenarios,
                                          ctx.cancelled->load() && (passed + failed + errored) == 0);
            }
            sr.duration = specDur;
            publish(EventType::SPEC_COMPLETED, ctx.testId, {
                {"spec", sr.specFile},
                {"name", sr.specName},
                {"state", testStateToString(sr.state)},
                {"duration", std::to_string(sr.duration)},
                {"total_scenarios", std::to_string(sr.totalScenarios)},
                {"passed_scenarios", std::to_string(sr.passedScenarios)},
                {"failed_scenarios", std::to_string(sr.failedScenarios)},
                {"skipped_scenarios", std::to_string(sr.skippedScenarios)}
            });
        }
        ctx.result.specResults.push_back(std::move(sr));
        if (ctx.result.specResults.back().state == TestState::PASSED ||
            ctx.result.specResults.back().state == TestState::SKIPPED) passedSpecs++;
        else if (ctx.result.specResults.back().state != TestState::CANCELLED) failedSpecs++;
    }
    ctx.status.executedSpecs = static_cast<int>(loaded.size());
    ctx.status.passedSpecs = passedSpecs;
    ctx.status.failedSpecs = failedSpecs;
    updateStatus(ctx);
}

SpecResult ExecutionEngine::executeSpec(RunContext& ctx, const spec::Specification& specification) {
    SpecResult result;
    result.specFile = specification.fileName;
    result.specName = specification.heading;
    result.tags = specification.tags;
    auto start = std::chrono::steady_clock::now();

    publish(EventType::SPEC_STARTED, ctx.testId, {
        {"spec", specification.fileName},
        {"name", specification.heading},
        {"scenarios", std::to_string(specification.scenarios.size())},
        {"tags", joinTags(specification.tags)}
    });

    ExecutionContext specCtx;
    specCtx.testId = ctx.testId;
    specCtx.specFile = specification.fileName;
    specCtx.specName = specification.heading;
    specCtx.tags = specification.tags;
    specCtx.environment = config_.environment;

    HookResult beforeSpec = runner_.runHook(HookType::BeforeSpec, specCtx);
    if (!beforeSpec.success) {
        result.errorMessage = "before_spec hook failed: " + beforeSpec.errorMessage;
    }

    int rows = specification.isDataDriven() ? static_cast<int>(specification.dataTable.rowCount()) : 1;
    int failed = 0, errored = 0, skipped = 0, passed = 0, total = 0;

    for (int row = 0; row < rows; ++row) {
        for (const auto& scenario : specification.scenarios) {
            if (!scenarioSelected(ctx, specification, scenario)) continue;
            total++;
            ScenarioResult sr;
            if (!beforeSpec.success || ctx.shouldStop() || checkTimeout(ctx)) {
                sr.scenarioName = scenario.name;
                sr.tags = effectiveTags(specification, scenario);
                sr.lineNumber = scenario.lineNumber;
                sr.dataRowIndex = specification.isDataDriven() ? row : -1;
                sr.state = ctx.cancelled->load() ? TestState::CANCELLED : TestState::SKIPPED;
                if (!beforeSpec.success) sr.errorMessage = "Skipped because before_spec hook failed";
                else if (ctx.cancelled->load()) sr.errorMessage = "Skipped because the test was cancelled";
                else if (ctx.timedOut.load()) sr.errorMessage = "Skipped because the test timed out";
                else sr.errorMessage = "Skipped";
                // 保留步骤列表（全部标记为跳过），便于结果视图展示完整场景结构
                for (const auto& step : scenario.steps) {
                    StepResult skippedStep;
                    skippedStep.stepText = step.text;
                    skippedStep.parameterizedText = step.parameterizedText;
                    skippedStep.isConcept = step.isConcept;
                    skippedStep.state = TestState::SKIPPED;
                    sr.stepResults.push_back(skippedStep);
                }
            } else {
                ctx.status.currentScenario = scenario.name;
                sr = executeScenario(ctx, specification, scenario, specification.isDataDriven() ? row : -1);
            }
            result.scenarioResults.push_back(sr);
            switch (sr.state) {
                case TestState::PASSED: passed++; break;
                case TestState::FAILED: failed++; break;
                case TestState::TEST_ERROR: errored++; break;
                default: skipped++; break;
            }
            applyScenarioOutcome(ctx, sr);
        }
    }

    HookResult afterSpec = runner_.runHook(HookType::AfterSpec, specCtx);
    if (!afterSpec.success) {
        result.errorMessage += std::string(result.errorMessage.empty() ? "" : "\n") + "after_spec hook failed: " + afterSpec.errorMessage;
    }

    result.totalScenarios = total;
    result.passedScenarios = passed;
    result.failedScenarios = failed + errored;
    result.skippedScenarios = skipped;
    if (!beforeSpec.success) result.state = TestState::TEST_ERROR;
    else result.state = aggregateState(failed, errored, skipped, total, ctx.cancelled->load() && (passed + failed + errored) == 0);
    if (total == 0) result.state = TestState::SKIPPED;
    result.duration = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    publish(EventType::SPEC_COMPLETED, ctx.testId, {
        {"spec", specification.fileName},
        {"name", specification.heading},
        {"state", testStateToString(result.state)},
        {"duration", std::to_string(result.duration)},
        {"total_scenarios", std::to_string(total)},
        {"passed_scenarios", std::to_string(passed)},
        {"failed_scenarios", std::to_string(failed + errored)},
        {"skipped_scenarios", std::to_string(skipped)}
    });
    return result;
}

ScenarioResult ExecutionEngine::executeScenario(RunContext& ctx, const spec::Specification& specification,
                                                const spec::Scenario& scenario, int dataRowIndex) {
    ScenarioResult result;
    result.scenarioName = scenario.name;
    result.tags = effectiveTags(specification, scenario);
    result.lineNumber = scenario.lineNumber;
    result.dataRowIndex = dataRowIndex;
    if (dataRowIndex >= 0) {
        for (const auto& h : specification.dataTable.headers) {
            result.dataRow[h] = specification.dataTable.cell(static_cast<size_t>(dataRowIndex), h);
        }
    }

    // 场景运行在测试级会话绑定的 Runner 上（见 executeTest），不同测试的步骤不会在同一有状态 Runner 中交错
    auto start = std::chrono::steady_clock::now();
    std::map<std::string, std::string> eventData = {
        {"spec", specification.fileName},
        {"scenario", scenario.name},
        {"tags", joinTags(result.tags)},
        {"steps", std::to_string(scenario.steps.size())}
    };
    if (tlsStream >= 0) eventData["stream"] = std::to_string(tlsStream);
    if (dataRowIndex >= 0) eventData["data_row"] = std::to_string(dataRowIndex);
    publish(EventType::SCENARIO_STARTED, ctx.testId, eventData);

    ExecutionContext execCtx;
    execCtx.testId = ctx.testId;
    execCtx.specFile = specification.fileName;
    execCtx.specName = specification.heading;
    execCtx.scenarioName = scenario.name;
    execCtx.tags = result.tags;
    execCtx.dataRow = result.dataRow;
    execCtx.environment = config_.environment;

    HookResult beforeScenario = runner_.runHook(HookType::BeforeScenario, execCtx);
    bool stop = !beforeScenario.success;
    if (stop) {
        result.errorMessage = "before_scenario hook failed: " + beforeScenario.errorMessage;
        result.state = TestState::TEST_ERROR;
    }

    auto runSteps = [&](const std::vector<spec::Step>& steps, std::vector<StepResult>& out, bool& stopFlag) {
        for (const auto& step : steps) {
            if (stopFlag || ctx.cancelled->load() || checkTimeout(ctx)) {
                StepResult skipped;
                skipped.stepText = step.text;
                skipped.parameterizedText = step.parameterizedText;
                skipped.state = TestState::SKIPPED;
                out.push_back(skipped);
                continue;
            }
            bool stopScenario = false;
            StepResult sr = executeStep(ctx, step, execCtx, result.dataRow, stopScenario);
            out.push_back(sr);
            if (stopScenario) {
                stopFlag = true;
                if (result.errorMessage.empty()) result.errorMessage = sr.errorMessage;
                if (sr.state == TestState::TEST_ERROR) result.state = TestState::TEST_ERROR;
                else if (result.state != TestState::TEST_ERROR) result.state = TestState::FAILED;
            }
        }
    };

    runSteps(specification.contexts, result.contextSteps, stop);
    runSteps(scenario.steps, result.stepResults, stop);
    // 清理步骤总是执行（除非取消/超时）
    bool teardownStop = false;
    runSteps(specification.teardowns, result.teardownSteps, teardownStop);
    if (teardownStop && result.state == TestState::PASSED) {
        result.state = TestState::FAILED;
    }

    HookResult afterScenario = runner_.runHook(HookType::AfterScenario, execCtx);
    if (!afterScenario.success && result.state == TestState::PASSED) {
        result.state = TestState::FAILED;
        result.errorMessage = "after_scenario hook failed: " + afterScenario.errorMessage;
    }

    if (ctx.cancelled->load() && result.state == TestState::PASSED) {
        // 若因取消导致有步骤被跳过，则标记为取消
        bool anySkipped = false;
        for (const auto& s : result.stepResults) if (s.state == TestState::SKIPPED) anySkipped = true;
        if (anySkipped || scenario.steps.empty()) result.state = TestState::CANCELLED;
    } else if (ctx.timedOut.load() && result.state == TestState::PASSED) {
        bool anySkipped = false;
        for (const auto& s : result.stepResults) if (s.state == TestState::SKIPPED) anySkipped = true;
        if (anySkipped) {
            result.state = TestState::TEST_ERROR;
            result.errorMessage = "Test timed out";
        }
    }

    result.duration = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    eventData["state"] = testStateToString(result.state);
    eventData["duration"] = std::to_string(result.duration);
    if (!result.errorMessage.empty()) eventData["error"] = result.errorMessage;
    publish(EventType::SCENARIO_COMPLETED, ctx.testId, eventData);
    return result;
}

StepResult ExecutionEngine::executeStep(RunContext& ctx, const spec::Step& step, const ExecutionContext& execCtx,
                                        const std::map<std::string, std::string>& dataRow, bool& stopScenario) {
    StepResult result;
    result.stepText = step.text;
    result.parameterizedText = step.parameterizedText;
    stopScenario = false;

    {
        std::lock_guard<std::recursive_mutex> lock(ctx.runMutex);
        ctx.status.currentStep = step.text;
    }
    publish(EventType::STEP_STARTED, ctx.testId, {
        {"spec", execCtx.specFile},
        {"scenario", execCtx.scenarioName},
        {"step", step.text},
        {"parameterized_text", step.parameterizedText},
        {"is_concept", step.isConcept ? "true" : "false"}
    });

    auto start = std::chrono::steady_clock::now();
    std::string specDir = FileUtil::getDirectoryPath(specs_.toAbsoluteInside(execCtx.specFile));
    if (specDir.empty()) specDir = specs_.specsDir();

    if (step.isConcept) {
        result.isConcept = true;
        bool stop = false;
        for (const auto& inner : step.conceptSteps) {
            if (stop || ctx.cancelled->load()) {
                StepResult skipped;
                skipped.stepText = inner.text;
                skipped.parameterizedText = inner.parameterizedText;
                skipped.state = TestState::SKIPPED;
                result.conceptSteps.push_back(skipped);
                continue;
            }
            bool innerStop = false;
            StepResult ir = executeStep(ctx, inner, execCtx, dataRow, innerStop);
            result.conceptSteps.push_back(ir);
            if (innerStop) {
                stop = true;
                result.state = ir.state;
                result.errorMessage = ir.errorMessage;
                result.stackTrace = ir.stackTrace;
            }
        }
        stopScenario = stop;
    } else {
        StepExecutionRequest req;
        req.stepText = step.text;
        req.parameterizedText = step.parameterizedText;
        req.args = step.args;
        resolveArgs(req.args, dataRow, specDir);
        req.context = execCtx;
        req.timeoutMs = config_.stepTimeoutMs;
        // 剩余测试时间不足时收紧步骤超时
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(ctx.deadline - std::chrono::steady_clock::now()).count();
        if (remaining > 0 && remaining < req.timeoutMs) req.timeoutMs = static_cast<int>(remaining);

        StepResult rr = runner_.executeStep(req);
        result.state = rr.state;
        result.errorMessage = rr.errorMessage;
        result.stackTrace = rr.stackTrace;
        result.messages = rr.messages;
        if (rr.duration > 0) result.duration = rr.duration;
        stopScenario = (result.state == TestState::FAILED || result.state == TestState::TEST_ERROR);
    }

    if (result.duration <= 0) {
        result.duration = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    std::map<std::string, std::string> data = {
        {"spec", execCtx.specFile},
        {"scenario", execCtx.scenarioName},
        {"step", step.text},
        {"state", testStateToString(result.state)},
        {"duration", std::to_string(result.duration)}
    };
    if (!result.errorMessage.empty()) data["error"] = result.errorMessage;
    publish(EventType::STEP_COMPLETED, ctx.testId, data);
    return result;
}

} // namespace testhub
