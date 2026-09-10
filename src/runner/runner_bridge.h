/*
 * TestHub - Runner 桥接
 * 管理 Runner 的生命周期（启动/停止/重启/自愈），并向执行引擎暴露统一的步骤执行接口。
 */

#pragma once

#include "runner.h"
#include "../event/event_bus.h"
#include "../model/types.h"

#include <atomic>
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
    int maxRestarts = 5;
    int mockDelayMs = 0;
};

class RunnerBridge {
public:
    RunnerBridge();
    ~RunnerBridge();

    /**
     * 使用配置启动 Runner
     */
    bool start(const RunnerConfig& config);

    /**
     * 兼容旧接口：按语言启动
     */
    bool startRunner(const std::string& language, const std::string& projectPath = "");

    void stopRunner();
    bool restartRunner();

    bool isConnected() const;
    RunnerStatus getStatus() const;
    const RunnerConfig& getConfig() const { return config_; }

    /**
     * 执行步骤（线程安全，串行化访问 Runner；Runner 崩溃时按配置自动重启）
     */
    StepResult executeStep(const StepExecutionRequest& request);

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
    RunnerConfig config_;
    std::unique_ptr<Runner> runner_;
    mutable std::mutex mutex_;          // 保护状态
    std::mutex execMutex_;              // 串行化执行
    RunnerState state_ = RunnerState::DISCONNECTED;
    int restartCount_ = 0;
    std::string lastError_;
    TimePoint startedAt_;
    TimePoint lastHeartbeat_;

    std::unique_ptr<Runner> createRunner();
    bool startLocked();
    void stopLocked();
    bool ensureAlive();
    void publish(const std::string& type, const std::string& detail);
};

} // namespace testhub
