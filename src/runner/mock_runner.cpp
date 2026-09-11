/*
 * TestHub - 内置 Mock Runner 实现
 */

#include "mock_runner.h"
#include "../util/string_util.h"

#include <chrono>
#include <thread>

namespace testhub {

StepResult MockRunner::executeStep(const StepExecutionRequest& request) {
    StepResult result;
    result.stepText = request.stepText;
    result.parameterizedText = request.parameterizedText;

    auto start = std::chrono::steady_clock::now();
    std::string lower = StringUtil::toLower(request.parameterizedText);

    int delay = defaultDelayMs_;
    // "sleep {}" 形式：第一个静态参数为毫秒数
    if (StringUtil::contains(lower, "sleep") && !request.args.empty()) {
        for (const auto& a : request.args) {
            if (a.type == spec::ArgType::Static) {
                int ms = StringUtil::toInt(a.value);
                if (ms > 0) delay = ms;
                break;
            }
        }
    }
    if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));

    if (StringUtil::contains(lower, "error")) {
        result.state = TestState::TEST_ERROR;
        result.errorMessage = "Mock runner raised an error for step: " + request.stepText;
        result.stackTrace = "MockRunner.executeStep\n  at mock_runner.cpp";
    } else if (StringUtil::contains(lower, "flaky")) {
        int n = 0;
        {
            std::lock_guard<std::mutex> lock(flakyMutex_);
            n = ++flakyCounts_[request.parameterizedText.empty() ? request.stepText : request.parameterizedText];
        }
        if (n == 1) {
            result.state = TestState::FAILED;
            result.errorMessage = "Mock runner flaky first attempt: " + request.stepText;
            result.stackTrace = "MockRunner.executeStep\n  at mock_runner.cpp";
        } else {
            result.state = TestState::PASSED;
        }
    } else if (StringUtil::contains(lower, "fail")) {
        result.state = TestState::FAILED;
        result.errorMessage = "Mock runner failed step: " + request.stepText;
        result.stackTrace = "MockRunner.executeStep\n  at mock_runner.cpp";
    } else {
        result.state = TestState::PASSED;
    }

    for (const auto& a : request.args) {
        if (a.type == spec::ArgType::Table) {
            result.messages.push_back("table arg with " + std::to_string(a.table.rowCount()) + " rows");
        } else {
            result.messages.push_back("arg[" + spec::argTypeToString(a.type) + "]=" + a.value);
        }
    }

    auto end = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration<double>(end - start).count();
    return result;
}

HookResult MockRunner::runHook(HookType, const ExecutionContext&) {
    return HookResult{};
}

std::vector<StepValue> MockRunner::getAllSteps() {
    return {};
}

} // namespace testhub
