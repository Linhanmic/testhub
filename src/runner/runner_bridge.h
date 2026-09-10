/*
 * TestHub - Runner 桥接
 * 管理一组 Runner 进程（Runner 池）的生命周期（启动/停止/重启/自愈），
 * 并向执行引擎暴露统一的步骤执行接口。
 *
 * 池模型：
 *   - 池中每个槽位对应一个独立的 Runner 进程；执行引擎以"会话"为粒度独占一个槽位——
 *     一个测试从 before_suite 到 after_suite 的全部钩子与场景都落在同一进程上，
 *     不同测试则在不同进程上真正并行（与 Gauge 并行流的语义一致）。
 *   - 并发安全的 Runner（如内置 mock）无需多进程，池自动收缩为 1 个共享槽位。
 *   - 每个槽位独立自愈：某个进程崩溃只会重启该槽位，不影响其他正在执行的测试。
 */

#pragma once

#include "runner.h"
#include "../event/event_bus.h"
#include "../model/types.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace testhub {

/**
 * Runner 配置
 */
struct RunnerConfig {
    std::string language = "mock";            // mock | python | <任意名称>
    std::string command;                      // 显式命令，优先于 language 推导
    std::string workingDir;                   // Runner 工作目录（通常为项目目录）
    std::map<std::string, std::string> env;   // 附加环境变量
    int connectionTimeoutMs = 15000;
    int requestTimeoutMs = 60000;
    bool autoRestart = true;
    int maxRestarts = 5;                      // 每个槽位的重启上限
    int mockDelayMs = 0;
    int poolSize = 1;                         // Runner 进程数（并发安全的 Runner 会被收缩为 1）
};

class RunnerBridge {
public:
    /**
     * 自定义 Runner 工厂（用于测试或嵌入场景）；未设置时按配置创建 mock/进程 Runner
     */
    using RunnerFactory = std::function<std::unique_ptr<Runner>()>;

    static constexpr int kMaxPoolSize = 64;

    RunnerBridge();
    ~RunnerBridge();

    RunnerBridge(const RunnerBridge&) = delete;
    RunnerBridge& operator=(const RunnerBridge&) = delete;

    void setRunnerFactory(RunnerFactory factory);

    /**
     * 使用配置启动 Runner 池
     */
    bool start(const RunnerConfig& config);

    /**
     * 兼容旧接口：按语言启动
     */
    bool startRunner(const std::string& language, const std::string& projectPath = "");

    /**
     * 停止所有 Runner（等待正在执行的场景结束）
     */
    void stopRunner();

    /**
     * 重启所有 Runner（等待正在执行的场景结束）
     */
    bool restartRunner();

    bool isConnected() const;
    RunnerStatus getStatus() const;
    const RunnerConfig& getConfig() const { return config_; }

    /**
     * 实际生效的池大小（start 之后有效）
     */
    int poolSize() const;

    /**
     * 会话：持有期间独占池中的一个 Runner 槽位，并把该槽位绑定到当前线程——
     * 此后本线程调用 executeStep/runHook 都会落在这个 Runner 上，保证同一测试的钩子与步骤
     * 不会与其他测试交错执行（Runner 内部的状态因此保持一致）。
     * 池中没有空闲槽位时 acquireSession 阻塞等待。
     * 对并发安全的 Runner（如内置 mock）返回共享槽位的会话，不做互斥。
     */
    class Session {
    public:
        Session() = default;
        Session(Session&& other) noexcept;
        Session& operator=(Session&& other) noexcept;
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        bool active() const { return bridge_ != nullptr; }
        int slotIndex() const { return slot_; }

    private:
        friend class RunnerBridge;
        Session(RunnerBridge* bridge, int slot);
        void release();

        RunnerBridge* bridge_ = nullptr;
        int slot_ = -1;
        RunnerBridge* prevBridge_ = nullptr;
        int prevSlot_ = -1;
    };
    Session acquireSession();

    /**
     * 执行步骤（线程安全；在会话内执行时使用会话绑定的 Runner，否则临时占用一个空闲槽位；
     * Runner 崩溃时按配置自动重启）
     */
    StepResult executeStep(const StepExecutionRequest& request);

    /**
     * 执行钩子（会话内落在会话绑定的 Runner 上，否则临时占用一个空闲槽位）
     */
    HookResult runHook(HookType type, const ExecutionContext& context);

    std::vector<StepValue> getAllSteps();

    /**
     * 校验步骤是否有实现；Runner 未报告步骤列表时返回 true
     */
    bool hasStep(const std::string& parameterizedText);

    /**
     * 根据语言推导默认命令（可被 TESTHUB_RUNNER_CMD 环境变量覆盖）
     */
    static std::string defaultCommandForLanguage(const std::string& language);

private:
    struct Slot {
        int index = 0;
        std::shared_ptr<Runner> runner;   // shared：执行线程持有引用期间即使被 stop/restart 也不会悬空
        RunnerState state = RunnerState::DISCONNECTED;
        int restartCount = 0;
        std::string lastError;
        TimePoint startedAt;
        TimePoint lastHeartbeat;
        unsigned long long stepsExecuted = 0;
        int users = 0;                    // 持有该槽位的会话数（独占模式下最多 1）
        bool restarting = false;          // 正在（重）启动，其他线程需等待
    };

    RunnerConfig config_;
    RunnerFactory factory_;
    std::vector<std::unique_ptr<Slot>> slots_;
    mutable std::mutex mutex_;            // 保护 slots_ 元数据、config_ 与下列标志
    std::mutex lifecycleMutex_;           // 串行化 start/stop/restart（它们会在中途释放 mutex_）
    std::condition_variable slotCv_;      // 槽位释放 / 排空完成 / 重启完成
    bool draining_ = false;               // stop/restart 期间：新会话等待，直到所有槽位释放并完成操作
    std::atomic<bool> concurrencySafe_{false};

    std::unique_ptr<Runner> createRunner(int slotIndex);
    bool startSlot(Slot& slot, std::unique_ptr<Runner> probe = nullptr);
    void startAllSlots(std::unique_lock<std::mutex>& lock, bool restart);
    void stopAllSlots(std::unique_lock<std::mutex>& lock);
    void waitForIdle(std::unique_lock<std::mutex>& lock);
    int acquireSlot();
    void releaseSlot(int index);
    Slot* boundSlot();
    bool ensureAlive(Slot& slot);
    std::shared_ptr<Runner> anyAliveRunner() const;
    void publish(const std::string& type, const std::string& detail, int slotIndex);
};

} // namespace testhub
