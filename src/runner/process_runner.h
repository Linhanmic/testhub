/*
 * TestHub - 外部进程 Runner
 * 通过子进程 stdin/stdout 以 JSON-lines 协议通信（每行一个 JSON 对象）。
 *
 * 协议（TestHub -> Runner）：
 *   {"id":1,"type":"execute_step","step_text":"...","parameterized_text":"... {}","args":[...],"context":{...}}
 *   {"id":2,"type":"hook","hook":"before_scenario","context":{...}}
 *   {"id":3,"type":"get_steps"}
 *   {"id":4,"type":"ping"}
 *   {"id":5,"type":"kill"}
 *
 * 协议（Runner -> TestHub）：
 *   {"id":1,"type":"step_result","status":"passed|failed|error|skipped","message":"...","stack_trace":"...","duration_ms":12,"messages":[...]}
 *   {"id":2,"type":"hook_result","status":"passed|failed","message":"..."}
 *   {"id":3,"type":"steps","steps":["... {}", ...]}
 *   {"id":4,"type":"pong","version":"..."}
 *   {"type":"log","level":"info","message":"..."}      (无 id，主动推送)
 */

#pragma once

#include "runner.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace testhub {

/**
 * 子进程封装（跨平台）
 */
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /**
     * 通过 shell 启动命令
     */
    bool start(const std::string& command, const std::string& workingDir,
               const std::map<std::string, std::string>& env = {});

    /**
     * 终止进程（先温和，超时后强制）
     */
    void terminate(int graceMs = 2000);

    bool isRunning() const;
    int pid() const { return pid_; }
    int exitCode() const { return exitCode_; }
    const std::string& lastError() const { return lastError_; }

    /**
     * 写一行到子进程 stdin
     */
    bool writeLine(const std::string& line);

    /**
     * 读取 stdout 一行（阻塞直到有数据或进程关闭）；返回 false 表示 EOF
     */
    bool readLine(std::string& line);

    /**
     * 读取 stderr 一行；返回 false 表示 EOF
     */
    bool readErrLine(std::string& line);

    void closeStdin();

private:
    int pid_ = 0;
    mutable int exitCode_ = -1;
    mutable std::atomic<bool> exited_{false};
    std::string lastError_;

#ifdef _WIN32
    HANDLE process_ = nullptr;
    HANDLE stdinWrite_ = nullptr;
    HANDLE stdoutRead_ = nullptr;
    HANDLE stderrRead_ = nullptr;
#else
    int stdinFd_ = -1;
    int stdoutFd_ = -1;
    int stderrFd_ = -1;
#endif
    std::string stdoutBuffer_;
    std::string stderrBuffer_;
    std::mutex writeMutex_;

    bool readLineFrom(std::string& buffer, std::string& line, bool err);
    void closeAll();
};

/**
 * JSON-lines 协议 Runner
 */
class ProcessRunner : public Runner {
public:
    using LogCallback = std::function<void(const std::string& level, const std::string& message)>;

    ProcessRunner(const std::string& command, const std::string& workingDir,
                  const std::map<std::string, std::string>& env = {});
    ~ProcessRunner() override;

    void setLogCallback(LogCallback cb) { logCallback_ = std::move(cb); }
    void setStartupTimeout(int ms) { startupTimeoutMs_ = ms; }

    bool start() override;
    void stop() override;
    bool isAlive() const override;
    std::string name() const override { return command_; }
    int pid() const override { return process_ ? process_->pid() : 0; }
    std::string version() const override { return version_; }

    StepResult executeStep(const StepExecutionRequest& request) override;
    HookResult runHook(HookType type, const ExecutionContext& context) override;
    std::vector<StepValue> getAllSteps() override;
    std::vector<StepValue> cachedSteps() const override;
    bool hasStep(const std::string& parameterizedText) override;

    const std::string& lastError() const { return lastError_; }

private:
    std::string command_;
    std::string workingDir_;
    std::map<std::string, std::string> env_;
    std::unique_ptr<ChildProcess> process_;
    LogCallback logCallback_;
    int startupTimeoutMs_ = 15000;
    std::string version_;
    std::string lastError_;

    std::thread readerThread_;
    std::thread stderrThread_;
    std::atomic<bool> running_{false};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<long long, Json> responses_;
    std::atomic<long long> nextId_{1};

    mutable std::mutex stepsMutex_;
    std::vector<StepValue> cachedSteps_;
    bool stepsLoaded_ = false;

    Json request(const std::string& type, Json payload, int timeoutMs, bool* timedOut = nullptr);
    void readerLoop();
    void stderrLoop();
    void emitLog(const std::string& level, const std::string& message);
    static Json contextToJson(const ExecutionContext& ctx);
};

} // namespace testhub
