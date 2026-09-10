/*
 * TestHub - Runner 桥接实现
 */

#include "runner_bridge.h"
#include "mock_runner.h"
#include "process_runner.h"
#include "../util/logger.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace testhub {

namespace {

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

} // namespace

RunnerBridge::RunnerBridge() = default;
RunnerBridge::~RunnerBridge() { stopRunner(); }

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

std::unique_ptr<Runner> RunnerBridge::createRunner() {
    if (config_.language == "mock" || config_.language == "none" || config_.language.empty()) {
        if (config_.command.empty()) return std::make_unique<MockRunner>(config_.mockDelayMs);
    }
    std::string command = config_.command.empty() ? defaultCommandForLanguage(config_.language) : config_.command;
    if (command.empty()) return std::make_unique<MockRunner>(config_.mockDelayMs);

    auto runner = std::make_unique<ProcessRunner>(command, config_.workingDir, config_.env);
    runner->setStartupTimeout(config_.connectionTimeoutMs);
    runner->setLogCallback([this](const std::string& level, const std::string& message) {
        TH_LOG_INFO("runner", "[" + level + "] " + message);
        publishEvent(EventType::RUNNER_LOG, "", {{"level", level}, {"message", message}, {"language", config_.language}});
    });
    return runner;
}

void RunnerBridge::publish(const std::string& type, const std::string& detail) {
    publishEvent(type, "", {{"language", config_.language}, {"detail", detail}});
}

bool RunnerBridge::start(const RunnerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == RunnerState::CONNECTED || state_ == RunnerState::BUSY) return true;
    config_ = config;
    restartCount_ = 0;
    return startLocked();
}

bool RunnerBridge::startRunner(const std::string& language, const std::string& projectPath) {
    RunnerConfig cfg;
    cfg.language = language;
    cfg.workingDir = projectPath;
    return start(cfg);
}

bool RunnerBridge::startLocked() {
    state_ = RunnerState::CONNECTING;
    publish(EventType::RUNNER_CONNECTING, "Starting " + config_.language + " runner");
    runner_ = createRunner();
    if (!runner_->start()) {
        auto* pr = dynamic_cast<ProcessRunner*>(runner_.get());
        lastError_ = pr ? pr->lastError() : "Failed to start runner";
        state_ = RunnerState::RUNNER_ERROR;
        publish(EventType::RUNNER_ERROR, lastError_);
        runner_.reset();
        return false;
    }
    state_ = RunnerState::CONNECTED;
    concurrencySafe_ = runner_->isConcurrencySafe();
    startedAt_ = TimeUtil::now();
    lastHeartbeat_ = startedAt_;
    lastError_.clear();
    publish(EventType::RUNNER_CONNECTED, runner_->name() + " connected");
    return true;
}

RunnerBridge::Session RunnerBridge::acquireSession() {
    if (concurrencySafe_) return Session();
    return Session(execMutex_);
}

void RunnerBridge::stopLocked() {
    if (runner_) {
        runner_->stop();
        runner_.reset();
    }
    if (state_ != RunnerState::DISCONNECTED) {
        state_ = RunnerState::DISCONNECTED;
        publish(EventType::RUNNER_DISCONNECTED, "Runner stopped");
    }
}

void RunnerBridge::stopRunner() {
    std::lock_guard<std::recursive_mutex> execLock(execMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    stopLocked();
}

bool RunnerBridge::restartRunner() {
    std::lock_guard<std::recursive_mutex> execLock(execMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    stopLocked();
    ++restartCount_;
    return startLocked();
}

bool RunnerBridge::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (state_ == RunnerState::CONNECTED || state_ == RunnerState::BUSY) && runner_ && runner_->isAlive();
}

RunnerStatus RunnerBridge::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RunnerStatus status;
    status.state = state_;
    if (runner_ && (state_ == RunnerState::CONNECTED || state_ == RunnerState::BUSY) && !runner_->isAlive()) {
        status.state = RunnerState::RUNNER_ERROR;
    }
    status.language = config_.language;
    status.command = config_.command.empty() ? defaultCommandForLanguage(config_.language) : config_.command;
    if (status.command.empty()) status.command = "builtin:mock";
    status.pid = runner_ ? runner_->pid() : 0;
    status.version = runner_ ? runner_->version() : "";
    status.lastHeartbeat = lastHeartbeat_;
    status.startedAt = startedAt_;
    status.restartCount = restartCount_;
    status.lastError = lastError_;
    if (runner_ && runner_->isAlive()) {
        // 步骤列表在首次查询后由 Runner 缓存，此处不会阻塞在网络往返上
        for (const auto& s : runner_->getAllSteps()) status.implementedSteps.push_back(s.parameterizedStepText);
    }
    return status;
}

bool RunnerBridge::ensureAlive() {
    // 调用方持有 execMutex_（并发安全的 Runner 除外，此时不会发生重启竞争：mock 永不退出）
    std::lock_guard<std::mutex> lock(mutex_);
    if (runner_ && runner_->isAlive()) return true;
    if (!config_.autoRestart) {
        if (state_ != RunnerState::DISCONNECTED) {
            state_ = RunnerState::RUNNER_ERROR;
            lastError_ = "Runner is not alive";
        }
        return false;
    }
    if (restartCount_ >= config_.maxRestarts) {
        state_ = RunnerState::RUNNER_ERROR;
        lastError_ = "Runner restart limit reached (" + std::to_string(config_.maxRestarts) + ")";
        publish(EventType::RUNNER_ERROR, lastError_);
        return false;
    }
    ++restartCount_;
    TH_LOG_WARN("runner", "Runner not alive; restarting (attempt " + std::to_string(restartCount_) + ")");
    if (runner_) { runner_->stop(); runner_.reset(); }
    return startLocked();
}

StepResult RunnerBridge::executeStep(const StepExecutionRequest& request) {
    Session execLock = acquireSession();
    if (!ensureAlive()) {
        StepResult r;
        r.stepText = request.stepText;
        r.parameterizedText = request.parameterizedText;
        r.state = TestState::TEST_ERROR;
        r.errorMessage = lastError_.empty() ? "Runner not connected" : lastError_;
        return r;
    }
    std::shared_ptr<Runner> runner;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = RunnerState::BUSY;
        runner = runner_;
    }
    StepExecutionRequest req = request;
    if (req.timeoutMs <= 0) req.timeoutMs = config_.requestTimeoutMs;
    StepResult result = runner->executeStep(req);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastHeartbeat_ = TimeUtil::now();
        if (runner_ && runner_->isAlive()) {
            state_ = RunnerState::CONNECTED;
        } else {
            state_ = RunnerState::RUNNER_ERROR;
            lastError_ = result.errorMessage.empty() ? "Runner died" : result.errorMessage;
            publish(EventType::RUNNER_ERROR, lastError_);
        }
    }
    return result;
}

HookResult RunnerBridge::runHook(HookType type, const ExecutionContext& context) {
    Session execLock = acquireSession();
    if (!ensureAlive()) {
        HookResult r;
        r.success = false;
        r.errorMessage = lastError_.empty() ? "Runner not connected" : lastError_;
        return r;
    }
    std::shared_ptr<Runner> runner;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runner = runner_;
    }
    return runner->runHook(type, context);
}

std::vector<StepValue> RunnerBridge::getAllSteps() {
    std::lock_guard<std::recursive_mutex> execLock(execMutex_);
    if (!ensureAlive()) return {};
    std::shared_ptr<Runner> runner;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runner = runner_;
    }
    return runner->getAllSteps();
}

bool RunnerBridge::hasStep(const std::string& parameterizedText) {
    std::lock_guard<std::recursive_mutex> execLock(execMutex_);
    if (!ensureAlive()) return true;
    std::shared_ptr<Runner> runner;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runner = runner_;
    }
    return runner->hasStep(parameterizedText);
}

} // namespace testhub
