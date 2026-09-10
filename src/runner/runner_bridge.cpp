/*
 * TestHub - Runner 桥接实现（Runner 池）
 */

#include "runner_bridge.h"
#include "mock_runner.h"
#include "process_runner.h"
#include "../util/logger.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace testhub {

namespace {

// 当前线程持有的会话所绑定的桥接与槽位
thread_local RunnerBridge* tlsBridge = nullptr;
thread_local int tlsSlot = -1;

std::string executableDir() {
#ifdef _WIN32
    return "";
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path().string();
#endif
}

std::string quoteShell(const std::string& s) {
    if (s.find_first_of(" \t\"'$`\\") == std::string::npos) return s;
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

/**
 * 在若干候选目录中查找随 TestHub 发布的 runner 脚本
 */
std::string locateBundledRunner(const std::string& relative) {
    std::vector<std::string> roots;
    if (const char* home = std::getenv("TESTHUB_HOME")) {
        if (*home) roots.push_back(home);
    }
    std::string exeDir = executableDir();
    if (!exeDir.empty()) {
        roots.push_back(exeDir);
        roots.push_back((std::filesystem::path(exeDir) / "..").string());
        roots.push_back((std::filesystem::path(exeDir) / ".." / "share" / "testhub").string());
    }
    roots.push_back(".");
    for (const auto& root : roots) {
        std::filesystem::path candidate = std::filesystem::path(root) / relative;
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return std::filesystem::weakly_canonical(candidate, ec).string();
        }
    }
    return "";
}

std::string slotTag(int index) { return "runner#" + std::to_string(index); }

} // namespace

// ============================================================
// Session
// ============================================================

RunnerBridge::Session::Session(RunnerBridge* bridge, int slot)
    : bridge_(bridge), slot_(slot), prevBridge_(tlsBridge), prevSlot_(tlsSlot) {
    tlsBridge = bridge;
    tlsSlot = slot;
}

RunnerBridge::Session::Session(Session&& other) noexcept
    : bridge_(other.bridge_), slot_(other.slot_), prevBridge_(other.prevBridge_), prevSlot_(other.prevSlot_) {
    other.bridge_ = nullptr;
    other.slot_ = -1;
}

RunnerBridge::Session& RunnerBridge::Session::operator=(Session&& other) noexcept {
    if (this != &other) {
        release();
        bridge_ = other.bridge_;
        slot_ = other.slot_;
        prevBridge_ = other.prevBridge_;
        prevSlot_ = other.prevSlot_;
        other.bridge_ = nullptr;
        other.slot_ = -1;
    }
    return *this;
}

RunnerBridge::Session::~Session() { release(); }

void RunnerBridge::Session::release() {
    if (!bridge_) return;
    tlsBridge = prevBridge_;
    tlsSlot = prevSlot_;
    bridge_->releaseSlot(slot_);
    bridge_ = nullptr;
    slot_ = -1;
}

// ============================================================
// 生命周期
// ============================================================

RunnerBridge::RunnerBridge() = default;
RunnerBridge::~RunnerBridge() { stopRunner(); }

void RunnerBridge::setRunnerFactory(RunnerFactory factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    factory_ = std::move(factory);
}

std::string RunnerBridge::defaultCommandForLanguage(const std::string& language) {
    if (const char* env = std::getenv("TESTHUB_RUNNER_CMD")) {
        if (*env) return env;
    }
    if (language == "python" || language == "py") {
        std::string script = locateBundledRunner("runners/python/testhub_runner.py");
        if (!script.empty()) return "python3 " + quoteShell(script);
        return "python3 -m testhub_runner";
    }
    if (language == "node" || language == "js" || language == "javascript") return "node testhub-runner.js";
    if (language == "mock" || language == "none" || language.empty()) return "";
    return "testhub-runner-" + language;
}

std::unique_ptr<Runner> RunnerBridge::createRunner(int slotIndex) {
    if (factory_) return factory_();
    if (config_.language == "mock" || config_.language == "none" || config_.language.empty()) {
        if (config_.command.empty()) return std::make_unique<MockRunner>(config_.mockDelayMs);
    }
    std::string command = config_.command.empty() ? defaultCommandForLanguage(config_.language) : config_.command;
    if (command.empty()) return std::make_unique<MockRunner>(config_.mockDelayMs);

    std::map<std::string, std::string> env = config_.env;
    env["TESTHUB_RUNNER_INDEX"] = std::to_string(slotIndex);
    env["TESTHUB_RUNNER_POOL_SIZE"] = std::to_string(static_cast<int>(slots_.size()));
    auto runner = std::make_unique<ProcessRunner>(command, config_.workingDir, env);
    runner->setStartupTimeout(config_.connectionTimeoutMs);
    std::string language = config_.language;
    bool pooled = slots_.size() > 1;
    runner->setLogCallback([slotIndex, language, pooled](const std::string& level, const std::string& message) {
        TH_LOG_INFO("runner", (pooled ? "[#" + std::to_string(slotIndex) + "] " : std::string()) + "[" + level + "] " + message);
        publishEvent(EventType::RUNNER_LOG, "", {{"level", level}, {"message", message}, {"language", language},
                                                 {"slot", std::to_string(slotIndex)}});
    });
    return runner;
}

void RunnerBridge::publish(const std::string& type, const std::string& detail, int slotIndex) {
    publishEvent(type, "", {{"language", config_.language}, {"detail", detail}, {"slot", std::to_string(slotIndex)}});
}

bool RunnerBridge::start(const RunnerConfig& config) {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    for (const auto& slot : slots_) {
        if (slot->state == RunnerState::CONNECTED || slot->state == RunnerState::BUSY) return true;
    }
    draining_ = true;
    waitForIdle(lock);

    config_ = config;
    int size = std::clamp(config.poolSize, 1, kMaxPoolSize);
    concurrencySafe_ = false;
    slots_.clear();
    for (int i = 0; i < size; ++i) {
        auto slot = std::make_unique<Slot>();
        slot->index = i;
        slots_.push_back(std::move(slot));
    }
    startAllSlots(lock, false);

    bool anyConnected = false;
    for (const auto& slot : slots_) anyConnected = anyConnected || slot->state == RunnerState::CONNECTED;
    if (slots_.size() > 1) {
        TH_LOG_INFO("runner", "Runner pool started with " + std::to_string(slots_.size()) + " " + config_.language + " runners");
    }
    draining_ = false;
    slotCv_.notify_all();
    return anyConnected;
}

bool RunnerBridge::startRunner(const std::string& language, const std::string& projectPath) {
    RunnerConfig cfg;
    cfg.language = language;
    cfg.workingDir = projectPath;
    return start(cfg);
}

bool RunnerBridge::startSlot(Slot& slot, std::unique_ptr<Runner> probe) {
    // 调用方保证此时没有其他线程会替换 slot.runner（槽位被独占，或池处于排空状态）
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slot.state = RunnerState::CONNECTING;
        slot.restarting = true;
    }
    publish(EventType::RUNNER_CONNECTING, "Starting " + config_.language + " runner", slot.index);

    std::shared_ptr<Runner> runner = probe ? std::shared_ptr<Runner>(std::move(probe)) : createRunner(slot.index);
    bool ok = runner->start();
    std::string error;
    if (!ok) {
        auto* pr = dynamic_cast<ProcessRunner*>(runner.get());
        error = pr ? pr->lastError() : "Failed to start runner";
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ok) {
            slot.runner = runner;
            slot.state = RunnerState::CONNECTED;
            slot.startedAt = TimeUtil::now();
            slot.lastHeartbeat = slot.startedAt;
            slot.lastError.clear();
        } else {
            slot.runner.reset();
            slot.state = RunnerState::RUNNER_ERROR;
            slot.lastError = error;
        }
        slot.restarting = false;
    }
    slotCv_.notify_all();
    if (ok) publish(EventType::RUNNER_CONNECTED, runner->name() + " connected", slot.index);
    else publish(EventType::RUNNER_ERROR, error, slot.index);
    return ok;
}

void RunnerBridge::startAllSlots(std::unique_lock<std::mutex>& lock, bool restart) {
    // 前置条件：持有 lock，池已排空（没有活动会话）
    if (slots_.empty()) return;
    if (restart) {
        for (auto& slot : slots_) ++slot->restartCount;
    }
    Slot* first = slots_[0].get();
    lock.unlock();
    // 先启动首个 Runner 以探测其并发安全性：并发安全的 Runner 无需多进程
    std::unique_ptr<Runner> probe = createRunner(0);
    bool safe = probe->isConcurrencySafe();
    startSlot(*first, std::move(probe));
    lock.lock();
    if (safe) {
        concurrencySafe_ = true;
        if (slots_.size() > 1) slots_.resize(1);
        return;
    }
    if (slots_.size() == 1) return;

    std::vector<Slot*> rest;
    for (size_t i = 1; i < slots_.size(); ++i) rest.push_back(slots_[i].get());
    lock.unlock();
    std::vector<std::thread> starters;
    starters.reserve(rest.size());
    for (Slot* slot : rest) starters.emplace_back([this, slot] { startSlot(*slot); });
    for (auto& t : starters) t.join();
    lock.lock();
}

void RunnerBridge::stopAllSlots(std::unique_lock<std::mutex>& lock) {
    // 前置条件：持有 lock，池已排空
    std::vector<std::shared_ptr<Runner>> runners;
    std::vector<int> stopped;
    for (auto& slot : slots_) {
        if (slot->runner) runners.push_back(std::move(slot->runner));
        slot->runner.reset();
        if (slot->state != RunnerState::DISCONNECTED) {
            slot->state = RunnerState::DISCONNECTED;
            stopped.push_back(slot->index);
        }
    }
    lock.unlock();
    if (runners.size() > 1) {
        std::vector<std::thread> stoppers;
        for (auto& r : runners) stoppers.emplace_back([r] { r->stop(); });
        for (auto& t : stoppers) t.join();
    } else {
        for (auto& r : runners) r->stop();
    }
    for (int index : stopped) publish(EventType::RUNNER_DISCONNECTED, "Runner stopped", index);
    lock.lock();
}

void RunnerBridge::waitForIdle(std::unique_lock<std::mutex>& lock) {
    slotCv_.wait(lock, [&] {
        for (const auto& slot : slots_) {
            if (slot->users > 0 || slot->restarting) return false;
        }
        return true;
    });
}

void RunnerBridge::stopRunner() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (slots_.empty()) return;
    draining_ = true;
    waitForIdle(lock);
    stopAllSlots(lock);
    draining_ = false;
    slotCv_.notify_all();
}

bool RunnerBridge::restartRunner() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (slots_.empty()) return false;
    draining_ = true;
    waitForIdle(lock);
    stopAllSlots(lock);
    startAllSlots(lock, true);
    bool allConnected = true;
    for (const auto& slot : slots_) allConnected = allConnected && slot->state == RunnerState::CONNECTED;
    draining_ = false;
    slotCv_.notify_all();
    return allConnected;
}

// ============================================================
// 槽位分配
// ============================================================

int RunnerBridge::acquireSlot() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (slots_.empty()) return -1;
    if (concurrencySafe_) {
        slotCv_.wait(lock, [&] { return !draining_; });
        if (slots_.empty()) return -1;
        ++slots_[0]->users;
        return 0;
    }
    auto usable = [&](const Slot& s) {
        // 永久失效（重启次数耗尽）的槽位只有在没有任何可用槽位时才会被分配，让调用方拿到明确的错误而不是无限等待
        return s.state != RunnerState::RUNNER_ERROR || (config_.autoRestart && s.restartCount < config_.maxRestarts);
    };
    int chosen = -1;
    slotCv_.wait(lock, [&] {
        if (draining_) return false;
        chosen = -1;
        bool anyUsable = false;
        int firstFree = -1, firstFreeUsable = -1, firstFreeConnected = -1;
        for (const auto& slot : slots_) {
            bool ok = usable(*slot);
            anyUsable = anyUsable || ok;
            if (slot->users > 0) continue;
            if (firstFree < 0) firstFree = slot->index;
            if (ok && firstFreeUsable < 0) firstFreeUsable = slot->index;
            if (ok && slot->state == RunnerState::CONNECTED && firstFreeConnected < 0) firstFreeConnected = slot->index;
        }
        if (firstFreeConnected >= 0) chosen = firstFreeConnected;
        else if (firstFreeUsable >= 0) chosen = firstFreeUsable;
        else if (!anyUsable) chosen = firstFree;
        return chosen >= 0;
    });
    slots_[static_cast<size_t>(chosen)]->users = 1;
    return chosen;
}

void RunnerBridge::releaseSlot(int index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= 0 && static_cast<size_t>(index) < slots_.size() && slots_[static_cast<size_t>(index)]->users > 0) {
            --slots_[static_cast<size_t>(index)]->users;
        }
    }
    slotCv_.notify_all();
}

RunnerBridge::Session RunnerBridge::acquireSession() {
    int index = acquireSlot();
    if (index < 0) return Session();
    return Session(this, index);
}

RunnerBridge::Slot* RunnerBridge::boundSlot() {
    if (tlsBridge != this || tlsSlot < 0) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<size_t>(tlsSlot) >= slots_.size()) return nullptr;
    return slots_[static_cast<size_t>(tlsSlot)].get();
}

int RunnerBridge::poolSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.empty() ? 1 : static_cast<int>(slots_.size());
}

// ============================================================
// 状态
// ============================================================

bool RunnerBridge::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& slot : slots_) {
        if ((slot->state == RunnerState::CONNECTED || slot->state == RunnerState::BUSY) && slot->runner && slot->runner->isAlive()) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<Runner> RunnerBridge::anyAliveRunner() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& slot : slots_) {
        if (slot->state == RunnerState::CONNECTED && slot->runner && slot->runner->isAlive()) return slot->runner;
    }
    return nullptr;
}

RunnerStatus RunnerBridge::getStatus() const {
    RunnerStatus status;
    std::shared_ptr<Runner> stepsSource;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status.language = config_.language;
        status.command = config_.command.empty() ? defaultCommandForLanguage(config_.language) : config_.command;
        if (status.command.empty()) status.command = "builtin:mock";
        status.poolSize = slots_.empty() ? 1 : static_cast<int>(slots_.size());

        bool anyConnecting = false, anyError = false;
        std::string errorFromDead;
        for (const auto& slot : slots_) {
            RunnerSlotStatus ss;
            ss.index = slot->index;
            bool alive = slot->runner && slot->runner->isAlive();
            ss.state = slot->state;
            ss.lastError = slot->lastError;
            if (ss.state == RunnerState::CONNECTED && !alive) {
                // 进程在两次使用之间退出：槽位按需重启（下次被分配时），此处只报告事实
                ss.state = RunnerState::RUNNER_ERROR;
                if (ss.lastError.empty()) {
                    ss.lastError = config_.autoRestart && slot->restartCount < config_.maxRestarts
                                       ? "Runner process exited (will be restarted on next use)"
                                       : "Runner process exited";
                }
            }
            ss.busy = slot->users > 0;
            if (alive && ss.busy) ss.state = RunnerState::BUSY;
            ss.pid = slot->runner ? slot->runner->pid() : 0;
            ss.version = slot->runner ? slot->runner->version() : "";
            ss.restartCount = slot->restartCount;
            ss.stepsExecuted = slot->stepsExecuted;
            ss.startedAt = slot->startedAt;
            ss.lastHeartbeat = slot->lastHeartbeat;

            if (alive) {
                ++status.aliveCount;
                if (ss.busy) ++status.busyCount;
                if (!stepsSource) {
                    stepsSource = slot->runner;
                    status.pid = ss.pid;
                    status.version = ss.version;
                    status.startedAt = ss.startedAt;
                }
            }
            anyConnecting = anyConnecting || ss.state == RunnerState::CONNECTING;
            if (ss.state == RunnerState::RUNNER_ERROR) {
                anyError = true;
                if (errorFromDead.empty()) errorFromDead = ss.lastError.empty() ? "Runner is not alive" : ss.lastError;
            }
            status.restartCount += slot->restartCount;
            if (slot->lastHeartbeat > status.lastHeartbeat) status.lastHeartbeat = slot->lastHeartbeat;
            if (status.lastError.empty()) status.lastError = slot->lastError;
            status.slots.push_back(std::move(ss));
        }
        if (!errorFromDead.empty()) status.lastError = errorFromDead;

        if (status.aliveCount > 0) status.state = status.busyCount > 0 ? RunnerState::BUSY : RunnerState::CONNECTED;
        else if (anyConnecting) status.state = RunnerState::CONNECTING;
        else if (anyError) status.state = RunnerState::RUNNER_ERROR;
        else status.state = RunnerState::DISCONNECTED;
    }
    if (stepsSource) {
        // 步骤列表在首次查询后由 Runner 缓存，此处通常不会阻塞在网络往返上
        for (const auto& s : stepsSource->getAllSteps()) status.implementedSteps.push_back(s.parameterizedStepText);
    }
    return status;
}

// ============================================================
// 执行
// ============================================================

bool RunnerBridge::ensureAlive(Slot& slot) {
    // 调用方持有该槽位（独占会话；共享模式下用 restarting 标志避免重复重启）
    std::unique_lock<std::mutex> lock(mutex_);
    slotCv_.wait(lock, [&] { return !slot.restarting; });
    if (slot.runner && slot.runner->isAlive()) return true;
    if (!config_.autoRestart) {
        if (slot.state != RunnerState::DISCONNECTED) {
            slot.state = RunnerState::RUNNER_ERROR;
            slot.lastError = "Runner is not alive";
        }
        return false;
    }
    if (slot.restartCount >= config_.maxRestarts) {
        std::string error = "Runner restart limit reached (" + std::to_string(config_.maxRestarts) + ")";
        bool changed = slot.lastError != error;
        slot.state = RunnerState::RUNNER_ERROR;
        slot.lastError = error;
        lock.unlock();
        if (changed) publish(EventType::RUNNER_ERROR, error, slot.index);
        return false;
    }
    ++slot.restartCount;
    slot.restarting = true;
    std::shared_ptr<Runner> old = std::move(slot.runner);
    slot.runner.reset();
    slot.state = RunnerState::CONNECTING;
    int attempt = slot.restartCount;
    lock.unlock();

    TH_LOG_WARN("runner", slotTag(slot.index) + " not alive; restarting (attempt " + std::to_string(attempt) + ")");
    if (old) old->stop();
    return startSlot(slot);
}

StepResult RunnerBridge::executeStep(const StepExecutionRequest& request) {
    Session temp;
    Slot* slot = boundSlot();
    if (!slot) {
        temp = acquireSession();
        if (temp.active()) slot = boundSlot();
    }
    auto failed = [&](const std::string& message) {
        StepResult r;
        r.stepText = request.stepText;
        r.parameterizedText = request.parameterizedText;
        r.state = TestState::TEST_ERROR;
        r.errorMessage = message;
        return r;
    };
    if (!slot) return failed("Runner not available");
    if (!ensureAlive(*slot)) {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed(slot->lastError.empty() ? "Runner not connected" : slot->lastError);
    }

    std::shared_ptr<Runner> runner;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runner = slot->runner;
    }
    StepExecutionRequest req = request;
    if (req.timeoutMs <= 0) req.timeoutMs = config_.requestTimeoutMs;
    StepResult result = runner->executeStep(req);

    std::string died;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slot->lastHeartbeat = TimeUtil::now();
        ++slot->stepsExecuted;
        if (slot->runner == runner && !runner->isAlive()) {
            slot->state = RunnerState::RUNNER_ERROR;
            slot->lastError = result.errorMessage.empty() ? "Runner died" : result.errorMessage;
            died = slot->lastError;
        }
    }
    if (!died.empty()) publish(EventType::RUNNER_ERROR, died, slot->index);
    return result;
}

HookResult RunnerBridge::runHook(HookType type, const ExecutionContext& context) {
    auto runOn = [&](Slot& slot) {
        HookResult r;
        if (!ensureAlive(slot)) {
            std::lock_guard<std::mutex> lock(mutex_);
            r.success = false;
            r.errorMessage = slot.lastError.empty() ? "Runner not connected" : slot.lastError;
            return r;
        }
        std::shared_ptr<Runner> runner;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            runner = slot.runner;
        }
        r = runner->runHook(type, context);
        std::string died;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            slot.lastHeartbeat = TimeUtil::now();
            if (slot.runner == runner && !runner->isAlive()) {
                slot.state = RunnerState::RUNNER_ERROR;
                slot.lastError = r.errorMessage.empty() ? "Runner died" : r.errorMessage;
                died = slot.lastError;
            }
        }
        if (!died.empty()) publish(EventType::RUNNER_ERROR, died, slot.index);
        return r;
    };

    if (Slot* bound = boundSlot()) return runOn(*bound);

    Session session = acquireSession();
    Slot* slot = session.active() ? boundSlot() : nullptr;
    if (!slot) {
        HookResult r;
        r.success = false;
        r.errorMessage = "Runner not available";
        return r;
    }
    return runOn(*slot);
}

std::vector<StepValue> RunnerBridge::getAllSteps() {
    if (auto runner = anyAliveRunner()) return runner->getAllSteps();
    Session session = acquireSession();
    Slot* slot = session.active() ? boundSlot() : nullptr;
    if (!slot || !ensureAlive(*slot)) return {};
    std::shared_ptr<Runner> runner;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runner = slot->runner;
    }
    return runner->getAllSteps();
}

bool RunnerBridge::hasStep(const std::string& parameterizedText) {
    if (auto runner = anyAliveRunner()) return runner->hasStep(parameterizedText);
    Session session = acquireSession();
    Slot* slot = session.active() ? boundSlot() : nullptr;
    if (!slot || !ensureAlive(*slot)) return true;
    std::shared_ptr<Runner> runner;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runner = slot->runner;
    }
    return runner->hasStep(parameterizedText);
}

} // namespace testhub
