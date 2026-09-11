/*
 * TestHub - 外部进程 Runner 实现
 */

#include "process_runner.h"
#include "../model/json_convert.h"
#include "../util/logger.h"

#include <chrono>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#endif

namespace testhub {

// ============================================================
// ChildProcess
// ============================================================

ChildProcess::~ChildProcess() {
    terminate(500);
    closeAll();
}

#ifdef _WIN32

bool ChildProcess::start(const std::string& command, const std::string& workingDir,
                         const std::map<std::string, std::string>& env) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr, stdoutWrite = nullptr, stderrWrite = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWrite_, &sa, 0)) { lastError_ = "CreatePipe(stdin) failed"; return false; }
    SetHandleInformation(stdinWrite_, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stdoutRead_, &stdoutWrite, &sa, 0)) { lastError_ = "CreatePipe(stdout) failed"; closeAll(); return false; }
    SetHandleInformation(stdoutRead_, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stderrRead_, &stderrWrite, &sa, 0)) { lastError_ = "CreatePipe(stderr) failed"; closeAll(); return false; }
    SetHandleInformation(stderrRead_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.hStdInput = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError = stderrWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;

    // 环境块
    std::string envBlock;
    LPCH parentEnv = GetEnvironmentStringsA();
    for (LPCH p = parentEnv; *p; p += strlen(p) + 1) {
        std::string entry(p);
        size_t eq = entry.find('=');
        std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
        if (env.find(key) == env.end()) { envBlock += entry; envBlock.push_back('\0'); }
    }
    FreeEnvironmentStringsA(parentEnv);
    for (const auto& kv : env) { envBlock += kv.first + "=" + kv.second; envBlock.push_back('\0'); }
    envBlock.push_back('\0');

    std::string cmdLine = "cmd.exe /C " + command;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             envBlock.data(), workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi);
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    if (!ok) {
        lastError_ = "CreateProcess failed: " + std::to_string(GetLastError());
        closeAll();
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = static_cast<int>(pi.dwProcessId);
    exited_ = false;
    return true;
}

bool ChildProcess::isRunning() const {
    if (!process_ || exited_) return false;
    DWORD code = 0;
    if (GetExitCodeProcess(process_, &code) && code != STILL_ACTIVE) {
        exitCode_ = static_cast<int>(code);
        exited_ = true;
        return false;
    }
    return true;
}

void ChildProcess::terminate(int graceMs) {
    if (!process_) return;
    closeStdin();
    if (WaitForSingleObject(process_, graceMs) == WAIT_TIMEOUT) {
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, 2000);
    }
    isRunning();
}

bool ChildProcess::writeLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (!stdinWrite_) return false;
    std::string data = line + "\n";
    DWORD written = 0;
    return WriteFile(stdinWrite_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != 0;
}

bool ChildProcess::readLineFrom(std::string& buffer, std::string& line, bool err) {
    HANDLE h = err ? stderrRead_ : stdoutRead_;
    while (true) {
        size_t nl = buffer.find('\n');
        if (nl != std::string::npos) {
            line = buffer.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buffer.erase(0, nl + 1);
            return true;
        }
        if (!h) return false;
        char chunk[4096];
        DWORD n = 0;
        if (!ReadFile(h, chunk, sizeof(chunk), &n, nullptr) || n == 0) {
            if (!buffer.empty()) { line = buffer; buffer.clear(); return true; }
            return false;
        }
        buffer.append(chunk, n);
    }
}

void ChildProcess::closeStdin() {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (stdinWrite_) { CloseHandle(stdinWrite_); stdinWrite_ = nullptr; }
}

void ChildProcess::closeAll() {
    closeStdin();
    if (stdoutRead_) { CloseHandle(stdoutRead_); stdoutRead_ = nullptr; }
    if (stderrRead_) { CloseHandle(stderrRead_); stderrRead_ = nullptr; }
    if (process_) { CloseHandle(process_); process_ = nullptr; }
}

#else  // POSIX

bool ChildProcess::start(const std::string& command, const std::string& workingDir,
                         const std::map<std::string, std::string>& env) {
    int inPipe[2], outPipe[2], errPipe[2];
    if (pipe(inPipe) != 0) { lastError_ = std::string("pipe(stdin): ") + strerror(errno); return false; }
    if (pipe(outPipe) != 0) { lastError_ = std::string("pipe(stdout): ") + strerror(errno); close(inPipe[0]); close(inPipe[1]); return false; }
    if (pipe(errPipe) != 0) {
        lastError_ = std::string("pipe(stderr): ") + strerror(errno);
        close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        lastError_ = std::string("fork: ") + strerror(errno);
        close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]); close(errPipe[0]); close(errPipe[1]);
        return false;
    }
    if (pid == 0) {
        // 子进程
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]); close(errPipe[0]); close(errPipe[1]);
        if (!workingDir.empty()) {
            if (chdir(workingDir.c_str()) != 0) {
                _exit(126);
            }
        }
        for (const auto& kv : env) setenv(kv.first.c_str(), kv.second.c_str(), 1);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);
    stdinFd_ = inPipe[1];
    stdoutFd_ = outPipe[0];
    stderrFd_ = errPipe[0];
    pid_ = static_cast<int>(pid);
    exited_ = false;
    return true;
}

bool ChildProcess::isRunning() const {
    if (pid_ <= 0 || exited_) return false;
    int status = 0;
    pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == pid_) {
        exited_ = true;
        if (WIFEXITED(status)) exitCode_ = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exitCode_ = 128 + WTERMSIG(status);
        return false;
    }
    if (r < 0) {
        exited_ = true;
        return false;
    }
    return true;
}

void ChildProcess::terminate(int graceMs) {
    if (pid_ <= 0) return;
    closeStdin();
    if (!exited_) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
        while (std::chrono::steady_clock::now() < deadline && isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (isRunning()) {
            kill(-pid_, SIGTERM);
            kill(pid_, SIGTERM);
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
            while (std::chrono::steady_clock::now() < deadline && isRunning()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (isRunning()) {
            kill(-pid_, SIGKILL);
            kill(pid_, SIGKILL);
            int status = 0;
            waitpid(pid_, &status, 0);
            exited_ = true;
            exitCode_ = 137;
        }
    }
    // 命令经 `sh -c` 启动：直接子进程（sh）退出后，真正的 Runner 可能作为孤儿进程仍持有
    // stdout/stderr 管道，读线程将永远等不到 EOF。子进程启动时自成进程组，这里清理整个组。
    if (kill(-pid_, 0) == 0) {
        kill(-pid_, SIGTERM);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline && kill(-pid_, 0) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (kill(-pid_, 0) == 0) kill(-pid_, SIGKILL);
    }
}

bool ChildProcess::writeLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (stdinFd_ < 0) return false;
    std::string data = line + "\n";
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(stdinFd_, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool ChildProcess::readLineFrom(std::string& buffer, std::string& line, bool err) {
    int fd = err ? stderrFd_ : stdoutFd_;
    while (true) {
        size_t nl = buffer.find('\n');
        if (nl != std::string::npos) {
            line = buffer.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buffer.erase(0, nl + 1);
            return true;
        }
        if (fd < 0) return false;
        char chunk[4096];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            if (!buffer.empty()) { line = buffer; buffer.clear(); return true; }
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(n));
    }
}

void ChildProcess::closeStdin() {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (stdinFd_ >= 0) { close(stdinFd_); stdinFd_ = -1; }
}

void ChildProcess::closeAll() {
    closeStdin();
    if (stdoutFd_ >= 0) { close(stdoutFd_); stdoutFd_ = -1; }
    if (stderrFd_ >= 0) { close(stderrFd_); stderrFd_ = -1; }
}

#endif

bool ChildProcess::readLine(std::string& line) { return readLineFrom(stdoutBuffer_, line, false); }
bool ChildProcess::readErrLine(std::string& line) { return readLineFrom(stderrBuffer_, line, true); }

// ============================================================
// ProcessRunner
// ============================================================

ProcessRunner::ProcessRunner(const std::string& command, const std::string& workingDir,
                             const std::map<std::string, std::string>& env)
    : command_(command), workingDir_(workingDir), env_(env) {}

ProcessRunner::~ProcessRunner() { stop(); }

void ProcessRunner::emitLog(const std::string& level, const std::string& message) {
    if (logCallback_) logCallback_(level, message);
    else TH_LOG_INFO("runner", "[" + level + "] " + message);
}

bool ProcessRunner::start() {
    if (running_) return true;
    process_ = std::make_unique<ChildProcess>();
    if (!process_->start(command_, workingDir_, env_)) {
        lastError_ = process_->lastError();
        TH_LOG_ERROR("runner", "Failed to start runner '" + command_ + "': " + lastError_);
        process_.reset();
        return false;
    }
    running_ = true;
    readerThread_ = std::thread(&ProcessRunner::readerLoop, this);
    stderrThread_ = std::thread(&ProcessRunner::stderrLoop, this);

    // 握手：ping
    bool timedOut = false;
    Json pong = request("ping", Json::object(), startupTimeoutMs_, &timedOut);
    if (pong.isNull()) {
        lastError_ = timedOut ? "Runner did not respond to ping within " + std::to_string(startupTimeoutMs_) + "ms"
                              : "Runner exited during handshake";
        if (process_ && !process_->isRunning()) {
            lastError_ += " (exit code " + std::to_string(process_->exitCode()) + ")";
        }
        TH_LOG_ERROR("runner", lastError_);
        stop();
        return false;
    }
    version_ = pong["version"].asString("");
    TH_LOG_INFO("runner", "Runner connected: " + command_ + (version_.empty() ? "" : " (" + version_ + ")") +
                          " pid=" + std::to_string(pid()));
    getAllSteps();
    return true;
}

void ProcessRunner::stop() {
    if (!process_) {
        running_ = false;
        return;
    }
    if (process_->isRunning()) {
        Json kill = Json::object();
        kill["id"] = static_cast<long long>(nextId_++);
        kill["type"] = "kill";
        process_->writeLine(kill.dump());
    }
    // 先礼后兵：等待 Runner 自行退出，超时后向进程组发 SIGTERM/SIGKILL；
    // 即使直接子进程已退出也要执行，以清理仍持有管道的孤儿进程，否则下面的 join 会永久阻塞
    process_->terminate(1500);
    running_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }
    // 子进程退出后管道写端关闭，读线程因 EOF 结束
    if (readerThread_.joinable()) {
        if (readerThread_.get_id() == std::this_thread::get_id()) readerThread_.detach();
        else readerThread_.join();
    }
    if (stderrThread_.joinable()) {
        if (stderrThread_.get_id() == std::this_thread::get_id()) stderrThread_.detach();
        else stderrThread_.join();
    }
    process_.reset();
}

bool ProcessRunner::isAlive() const {
    return running_ && process_ && process_->isRunning();
}

Json ProcessRunner::contextToJson(const ExecutionContext& ctx) {
    Json obj = Json::object();
    obj["test_id"] = ctx.testId;
    obj["spec_file"] = ctx.specFile;
    obj["spec_name"] = ctx.specName;
    obj["scenario_name"] = ctx.scenarioName;
    obj["tags"] = toJson(ctx.tags);
    obj["data_row"] = toJson(ctx.dataRow);
    obj["environment"] = toJson(ctx.environment);
    return obj;
}

Json ProcessRunner::request(const std::string& type, Json payload, int timeoutMs, bool* timedOut) {
    if (timedOut) *timedOut = false;
    if (!process_) return Json();
    long long id = nextId_++;
    payload["id"] = id;
    payload["type"] = type;
    if (!process_->writeLine(payload.dump())) {
        lastError_ = "Failed to write to runner stdin";
        return Json();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        auto it = responses_.find(id);
        if (it != responses_.end()) {
            Json r = it->second;
            responses_.erase(it);
            return r;
        }
        if (!running_) return Json();
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            auto again = responses_.find(id);
            if (again != responses_.end()) {
                Json r = again->second;
                responses_.erase(again);
                return r;
            }
            if (timedOut) *timedOut = true;
            return Json();
        }
    }
}

void ProcessRunner::readerLoop() {
    std::string line;
    while (process_ && process_->readLine(line)) {
        if (line.empty()) continue;
        std::string err;
        Json msg = Json::tryParse(line, &err);
        if (!msg.isObject()) {
            // 非 JSON 输出视为日志
            emitLog("stdout", line);
            continue;
        }
        std::string type = msg["type"].asString();
        if (type == "log") {
            emitLog(msg["level"].asString("info"), msg["message"].asString());
            continue;
        }
        if (msg["id"].isNumber()) {
            std::lock_guard<std::mutex> lock(mutex_);
            responses_[msg["id"].asInt64()] = msg;
            cv_.notify_all();
        } else {
            emitLog("stdout", line);
        }
    }
    running_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
}

void ProcessRunner::stderrLoop() {
    std::string line;
    while (process_ && process_->readErrLine(line)) {
        if (!line.empty()) emitLog("stderr", line);
    }
}

StepResult ProcessRunner::executeStep(const StepExecutionRequest& req) {
    StepResult result;
    result.stepText = req.stepText;
    result.parameterizedText = req.parameterizedText;

    if (!isAlive()) {
        result.state = TestState::TEST_ERROR;
        result.errorMessage = "Runner is not running";
        return result;
    }

    Json payload = Json::object();
    payload["step_text"] = req.stepText;
    payload["parameterized_text"] = req.parameterizedText;
    Json args = Json::array();
    for (const auto& a : req.args) args.push(toJson(a));
    payload["args"] = args;
    payload["context"] = contextToJson(req.context);

    auto start = std::chrono::steady_clock::now();
    bool timedOut = false;
    Json resp = request("execute_step", payload, req.timeoutMs > 0 ? req.timeoutMs : 60000, &timedOut);
    auto end = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration<double>(end - start).count();

    if (resp.isNull()) {
        result.state = TestState::TEST_ERROR;
        result.errorMessage = timedOut ? "Step timed out after " + std::to_string(req.timeoutMs) + "ms"
                                       : "Runner disconnected while executing step";
        if (timedOut) {
            // 超时的 Runner 状态不可信，终止以便桥接层重启
            stop();
        }
        return result;
    }

    std::string status = resp["status"].asString("failed");
    if (status == "passed") result.state = TestState::PASSED;
    else if (status == "skipped") result.state = TestState::SKIPPED;
    else if (status == "error") result.state = TestState::TEST_ERROR;
    else result.state = TestState::FAILED;
    result.errorMessage = resp["message"].asString("");
    result.stackTrace = resp["stack_trace"].asString("");
    if (resp["duration_ms"].isNumber()) result.duration = resp["duration_ms"].asNumber() / 1000.0;
    for (const auto& m : resp["messages"].asArray()) {
        if (m.isString()) result.messages.push_back(m.asString());
    }
    return result;
}

HookResult ProcessRunner::runHook(HookType type, const ExecutionContext& context) {
    HookResult result;
    if (!isAlive()) {
        result.success = false;
        result.errorMessage = "Runner is not running";
        return result;
    }
    Json payload = Json::object();
    payload["hook"] = hookTypeToString(type);
    payload["context"] = contextToJson(context);
    auto start = std::chrono::steady_clock::now();
    bool timedOut = false;
    Json resp = request("hook", payload, 60000, &timedOut);
    result.duration = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (resp.isNull()) {
        result.success = false;
        result.errorMessage = timedOut ? "Hook timed out" : "Runner disconnected during hook";
        return result;
    }
    std::string status = resp["status"].asString("passed");
    result.success = (status == "passed" || status == "skipped");
    result.errorMessage = resp["message"].asString("");
    result.stackTrace = resp["stack_trace"].asString("");
    for (const auto& m : resp["messages"].asArray()) {
        if (m.isString()) result.messages.push_back(m.asString());
    }
    return result;
}

std::vector<StepValue> ProcessRunner::getAllSteps() {
    {
        std::lock_guard<std::mutex> lock(stepsMutex_);
        if (stepsLoaded_) return cachedSteps_;
    }
    if (!isAlive()) return {};
    Json resp = request("get_steps", Json::object(), 10000);
    if (resp.isNull()) return {};
    std::vector<StepValue> loaded;
    for (const auto& s : resp["steps"].asArray()) {
        StepValue v;
        if (s.isString()) {
            v.parameterizedStepText = s.asString();
            v.stepText = s.asString();
        } else if (s.isObject()) {
            v.parameterizedStepText = s["parameterized_text"].asString();
            v.stepText = s["text"].asString(v.parameterizedStepText);
            for (const auto& p : s["params"].asArray()) if (p.isString()) v.parameters.push_back(p.asString());
        }
        if (!v.parameterizedStepText.empty()) loaded.push_back(v);
    }
    std::lock_guard<std::mutex> lock(stepsMutex_);
    cachedSteps_ = loaded;
    stepsLoaded_ = true;
    return cachedSteps_;
}

std::vector<StepValue> ProcessRunner::cachedSteps() const {
    std::lock_guard<std::mutex> lock(stepsMutex_);
    return cachedSteps_;
}

bool ProcessRunner::hasStep(const std::string& parameterizedText) {
    auto steps = getAllSteps();
    if (steps.empty()) return true;  // Runner 未报告步骤列表时不做校验
    for (const auto& s : steps) if (s.parameterizedStepText == parameterizedText) return true;
    return false;
}

} // namespace testhub
