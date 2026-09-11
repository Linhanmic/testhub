/*
 * TestHub - 内置 Mock Runner
 * 无需外部进程即可完成端到端流程，用于演示、开发与自动化测试。
 * 规则：
 *   - 步骤文本包含 "error"（不区分大小写）→ 错误
 *   - 步骤文本包含 "flaky" → 该步骤文案第一次失败、之后通过（用于验证步骤重试）
 *   - 步骤文本包含 "fail"（不区分大小写）→ 失败
 *   - 步骤文本包含 "sleep <ms>" 形式的参数 → 延迟对应毫秒
 *   - 其他 → 通过
 */

#pragma once

#include "runner.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace testhub {

class MockRunner : public Runner {
public:
    explicit MockRunner(int defaultDelayMs = 0) : defaultDelayMs_(defaultDelayMs) {}

    bool start() override { alive_ = true; return true; }
    void stop() override { alive_ = false; }
    bool isAlive() const override { return alive_; }
    std::string name() const override { return "mock"; }
    std::string version() const override { return "1.0"; }
    bool isConcurrencySafe() const override { return true; }

    StepResult executeStep(const StepExecutionRequest& request) override;
    HookResult runHook(HookType type, const ExecutionContext& context) override;
    std::vector<StepValue> getAllSteps() override;
    bool hasStep(const std::string&) override { return true; }

private:
    std::atomic<bool> alive_{false};
    int defaultDelayMs_;
    mutable std::mutex flakyMutex_;
    std::unordered_map<std::string, int> flakyCounts_;
};

} // namespace testhub
