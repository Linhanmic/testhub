/*
 * TestHub - Runner 抽象接口
 * Runner 负责真正执行步骤实现；TestHub 通过统一接口与不同 Runner 交互
 */

#pragma once

#include "../model/types.h"
#include "../spec/spec.h"
#include "../util/json.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace testhub {

/**
 * 执行上下文，随每个请求传给 Runner
 */
struct ExecutionContext {
    std::string testId;
    std::string specFile;
    std::string specName;
    std::string scenarioName;
    std::vector<std::string> tags;
    std::map<std::string, std::string> dataRow;
    std::map<std::string, std::string> environment;
};

/**
 * 步骤执行请求
 */
struct StepExecutionRequest {
    std::string stepText;
    std::string parameterizedText;
    std::vector<spec::StepArg> args;   // 动态参数已解析为静态值
    ExecutionContext context;
    int timeoutMs = 60000;
};

/**
 * 生命周期钩子
 */
enum class HookType {
    BeforeSuite,
    AfterSuite,
    BeforeSpec,
    AfterSpec,
    BeforeScenario,
    AfterScenario,
    BeforeStep,
    AfterStep
};

inline const char* hookTypeToString(HookType t) {
    switch (t) {
        case HookType::BeforeSuite: return "before_suite";
        case HookType::AfterSuite: return "after_suite";
        case HookType::BeforeSpec: return "before_spec";
        case HookType::AfterSpec: return "after_spec";
        case HookType::BeforeScenario: return "before_scenario";
        case HookType::AfterScenario: return "after_scenario";
        case HookType::BeforeStep: return "before_step";
        case HookType::AfterStep: return "after_step";
    }
    return "unknown";
}

/**
 * 钩子执行结果
 */
struct HookResult {
    bool success = true;
    std::string errorMessage;
    std::string stackTrace;
    double duration = 0.0;
    std::vector<std::string> messages;
};

/**
 * Runner 接口
 */
class Runner {
public:
    virtual ~Runner() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isAlive() const = 0;

    virtual std::string name() const = 0;
    virtual int pid() const { return 0; }
    virtual std::string version() const { return ""; }

    /**
     * 是否允许多个场景并发地调用 executeStep。
     * 单进程、有状态的 Runner（如 Python 参考实现）必须返回 false，
     * 桥接层会以场景为粒度串行化对其的访问。
     */
    virtual bool isConcurrencySafe() const { return false; }

    /**
     * 执行一个步骤
     */
    virtual StepResult executeStep(const StepExecutionRequest& request) = 0;

    /**
     * 触发钩子（可选，默认无操作）
     */
    virtual HookResult runHook(HookType, const ExecutionContext&) { return HookResult{}; }

    /**
     * 获取 Runner 已实现的步骤（参数化文本）
     */
    virtual std::vector<StepValue> getAllSteps() { return {}; }

    /**
     * 已缓存的步骤列表，禁止向 Runner 发协议消息。
     * GET /status 在 execute_step 期间也会调用；JSON-lines 一次只能处理一条请求，
     * 若此处再 get_steps，步骤里访问本进程 HTTP API 会与状态查询互相等待。
     */
    virtual std::vector<StepValue> cachedSteps() const { return {}; }

    /**
     * 询问 Runner 是否实现了某个步骤；默认基于 getAllSteps 缓存
     */
    virtual bool hasStep(const std::string& parameterizedText) {
        for (const auto& s : getAllSteps()) {
            if (s.parameterizedStepText == parameterizedText) return true;
        }
        return false;
    }
};

} // namespace testhub
