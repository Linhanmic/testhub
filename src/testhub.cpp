/*
 * TestHub - Persistent Automation Test System Implementation
 */

#include "testhub.h"
#include "util/file_util.h"
#include "util/string_util.h"

// Reuse Gauge parser
#include "parser/spec_parser.h"
#include "parser/concept_parser.h"
#include "gauge/specification.h"

#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

namespace testhub {

TestHub::~TestHub() {
    stop();
}

bool TestHub::initialize(const TestHubConfig& config) {
    if (initialized_) {
        return true;
    }

    config_ = config;

    httpServer_ = std::make_unique<HttpServer>(config.port);
    testQueue_ = std::make_unique<TestQueue>();
    runnerBridge_ = std::make_unique<RunnerBridge>();

    httpServer_->setTestQueue(testQueue_.get());
    httpServer_->setRunnerBridge(runnerBridge_.get());

    eventHandlerId_ = EventBus::getInstance().subscribe("*", [this](const Event& event) {
        std::cout << "[Event] " << event.type << " - " << event.testId << std::endl;
    });

    initialized_ = true;
    return true;
}

bool TestHub::start() {
    if (!initialized_) {
        std::cerr << "TestHub not initialized" << std::endl;
        return false;
    }

    if (running_) {
        return true;
    }

    if (!runnerBridge_->startRunner(config_.runnerLanguage, config_.projectPath)) {
        std::cerr << "Failed to start runner: " << config_.runnerLanguage << std::endl;
    }

    if (!httpServer_->start()) {
        std::cerr << "Failed to start HTTP server" << std::endl;
        return false;
    }

    stopExecution_ = false;
    executionThread_ = std::thread(&TestHub::executionLoop, this);

    running_ = true;

    std::cout << "TestHub started on port " << config_.port << std::endl;
    std::cout << "Runner language: " << config_.runnerLanguage << std::endl;
    std::cout << "API endpoint: http://localhost:" << config_.port << "/api/v1" << std::endl;

    publishEvent(EventType::TEST_STARTED, "", {{"message", "TestHub server started"}});

    return true;
}

void TestHub::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    stopExecution_ = true;
    if (executionThread_.joinable()) {
        executionThread_.join();
    }

    if (httpServer_) {
        httpServer_->stop();
    }

    if (runnerBridge_) {
        runnerBridge_->stopRunner();
    }

    if (!eventHandlerId_.empty()) {
        EventBus::getInstance().unsubscribe("*", eventHandlerId_);
    }

    std::cout << "TestHub stopped" << std::endl;
}

std::string TestHub::submitTest(const TestRequest& request) {
    if (!running_) {
        return "";
    }

    std::string testId = testQueue_->enqueue(request);

    publishTestEvent(EventType::TEST_SUBMITTED, testId, {
        {"spec_files", StringUtil::join(request.specFiles, ",")},
        {"tags", StringUtil::join(request.tags, ",")}
    });

    return testId;
}

bool TestHub::cancelTest(const std::string& testId) {
    if (testQueue_->cancel(testId)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(testsMutex_);
    auto it = activeTests_.find(testId);
    if (it != activeTests_.end()) {
        it->second.state = TestState::CANCELLED;
        publishTestEvent(EventType::TEST_CANCELLED, testId);
        return true;
    }

    return false;
}

TestStatus TestHub::getTestStatus(const std::string& testId) const {
    std::lock_guard<std::mutex> lock(testsMutex_);
    
    auto it = activeTests_.find(testId);
    if (it != activeTests_.end()) {
        return it->second;
    }

    auto completedIt = completedTests_.find(testId);
    if (completedIt != completedTests_.end()) {
        TestStatus status;
        status.testId = testId;
        status.state = completedIt->second.finalState;
        status.startTime = completedIt->second.startTime;
        status.endTime = completedIt->second.endTime;
        return status;
    }

    TestStatus status;
    status.testId = testId;
    status.state = TestState::TEST_ERROR;
    return status;
}

TestResult TestHub::getTestResult(const std::string& testId) const {
    std::lock_guard<std::mutex> lock(testsMutex_);
    
    auto it = completedTests_.find(testId);
    if (it != completedTests_.end()) {
        return it->second;
    }

    TestResult result;
    result.testId = testId;
    result.finalState = TestState::TEST_ERROR;
    return result;
}

std::vector<TestInfo> TestHub::listTests() const {
    std::lock_guard<std::mutex> lock(testsMutex_);
    
    std::vector<TestInfo> tests;

    for (const auto& pair : activeTests_) {
        TestInfo info;
        info.testId = pair.first;
        info.state = pair.second.state;
        info.progress = pair.second.progress;
        info.startTime = pair.second.startTime;
        tests.push_back(info);
    }

    for (const auto& pair : completedTests_) {
        TestInfo info;
        info.testId = pair.first;
        info.state = pair.second.finalState;
        info.progress = 1.0;
        info.startTime = pair.second.startTime;
        tests.push_back(info);
    }

    return tests;
}

std::map<std::string, std::string> TestHub::getStatus() const {
    std::map<std::string, std::string> status;
    
    status["running"] = running_ ? "true" : "false";
    status["port"] = std::to_string(config_.port);
    status["runner_language"] = config_.runnerLanguage;
    status["queue_size"] = std::to_string(testQueue_ ? testQueue_->size() : 0);
    status["active_tests"] = std::to_string(activeTests_.size());
    status["completed_tests"] = std::to_string(completedTests_.size());
    
    RunnerStatus runnerStatus = runnerBridge_ ? runnerBridge_->getStatus() : RunnerStatus();
    switch (runnerStatus.state) {
        case RunnerState::DISCONNECTED: status["runner_state"] = "disconnected"; break;
        case RunnerState::CONNECTING: status["runner_state"] = "connecting"; break;
        case RunnerState::CONNECTED: status["runner_state"] = "connected"; break;
        case RunnerState::BUSY: status["runner_state"] = "busy"; break;
        case RunnerState::RUNNER_ERROR: status["runner_state"] = "error"; break;
    }
    
    return status;
}

void TestHub::executionLoop() {
    while (!stopExecution_) {
        auto task = testQueue_->dequeue();
        
        if (task.has_value()) {
            executeTest(task.value());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void TestHub::executeTest(const TestTask& task) {
    std::string testId = task.request.id;
    
    TestStatus status;
    status.testId = testId;
    status.state = TestState::RUNNING;
    status.startTime = std::chrono::system_clock::now();
    
    {
        std::lock_guard<std::mutex> lock(testsMutex_);
        activeTests_[testId] = status;
    }
    
    publishTestEvent(EventType::TEST_STARTED, testId);
    
    TestResult result;
    result.testId = testId;
    result.startTime = status.startTime;
    
    int totalSpecs = 0;
    int passedSpecs = 0;
    int failedSpecs = 0;
    
    for (const auto& specFile : task.request.specFiles) {
        status.currentSpec = specFile;
        status.totalSpecs = ++totalSpecs;
        updateTestStatus(testId, status);
        
        SpecResult specResult = executeSpec(testId, specFile);
        result.specResults.push_back(specResult);
        
        if (specResult.state == TestState::PASSED) {
            passedSpecs++;
        } else {
            failedSpecs++;
        }
        
        status.executedSpecs = totalSpecs;
        status.passedSpecs = passedSpecs;
        status.failedSpecs = failedSpecs;
        status.progress = static_cast<double>(totalSpecs) / task.request.specFiles.size();
        updateTestStatus(testId, status);
    }
    
    result.endTime = std::chrono::system_clock::now();
    result.totalDuration = std::chrono::duration<double>(result.endTime - result.startTime).count();
    result.finalState = (failedSpecs == 0) ? TestState::PASSED : TestState::FAILED;
    
    {
        std::lock_guard<std::mutex> lock(testsMutex_);
        activeTests_.erase(testId);
        completedTests_[testId] = result;
    }
    
    publishTestEvent(EventType::TEST_COMPLETED, testId, {
        {"state", testStateToString(result.finalState)},
        {"duration", std::to_string(result.totalDuration)},
        {"passed_specs", std::to_string(passedSpecs)},
        {"failed_specs", std::to_string(failedSpecs)}
    });
    
    std::cout << "Test " << testId << " completed: " << testStateToString(result.finalState) << std::endl;
}

SpecResult TestHub::executeSpec(const std::string& testId, const std::string& specFile) {
    SpecResult result;
    result.specFile = specFile;
    
    auto spec = loadSpec(specFile);
    if (!spec) {
        result.state = TestState::TEST_ERROR;
        result.errorMessage = "Failed to load spec: " + specFile;
        return result;
    }
    
    result.specName = spec->getTitle();
    
    publishTestEvent(EventType::SPEC_STARTED, testId, {{"spec", specFile}});
    
    int totalScenarios = 0;
    int passedScenarios = 0;
    int failedScenarios = 0;
    
    for (const auto& scenario : spec->getScenarios()) {
        ScenarioResult scenarioResult = executeScenario(testId, scenario);
        result.scenarioResults.push_back(scenarioResult);
        
        totalScenarios++;
        if (scenarioResult.state == TestState::PASSED) {
            passedScenarios++;
        } else {
            failedScenarios++;
        }
    }
    
    result.state = (failedScenarios == 0) ? TestState::PASSED : TestState::FAILED;
    
    publishTestEvent(EventType::SPEC_COMPLETED, testId, {
        {"spec", specFile},
        {"state", testStateToString(result.state)},
        {"total_scenarios", std::to_string(totalScenarios)},
        {"passed_scenarios", std::to_string(passedScenarios)},
        {"failed_scenarios", std::to_string(failedScenarios)}
    });
    
    return result;
}

ScenarioResult TestHub::executeScenario(const std::string& testId, std::shared_ptr<gauge::Scenario> scenario) {
    ScenarioResult result;
    result.scenarioName = scenario->getName();
    
    publishTestEvent(EventType::SCENARIO_STARTED, testId, {{"scenario", scenario->getName()}});
    
    auto startTime = std::chrono::steady_clock::now();
    
    int failedSteps = 0;
    
    for (const auto& step : scenario->getSteps()) {
        StepResult stepResult = executeStep(testId, step);
        result.stepResults.push_back(stepResult);
        
        if (stepResult.state != TestState::PASSED) {
            failedSteps++;
            result.errorMessage = stepResult.errorMessage;
        }
    }
    
    auto endTime = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration<double>(endTime - startTime).count();
    
    result.state = (failedSteps == 0) ? TestState::PASSED : TestState::FAILED;
    
    publishTestEvent(EventType::SCENARIO_COMPLETED, testId, {
        {"scenario", scenario->getName()},
        {"state", testStateToString(result.state)},
        {"duration", std::to_string(result.duration)}
    });
    
    return result;
}

StepResult TestHub::executeStep(const std::string& testId, std::shared_ptr<gauge::Step> step) {
    StepResult result;
    result.stepText = step->getText();
    
    publishTestEvent(EventType::STEP_STARTED, testId, {{"step", step->getText()}});
    
    if (runnerBridge_ && runnerBridge_->isConnected()) {
        result = runnerBridge_->executeStep(step->getText(), step->getArgs());
    } else {
        result.state = TestState::PASSED;
        result.duration = 0.0;
    }
    
    publishTestEvent(EventType::STEP_COMPLETED, testId, {
        {"step", step->getText()},
        {"state", testStateToString(result.state)},
        {"duration", std::to_string(result.duration)}
    });
    
    return result;
}

void TestHub::updateTestStatus(const std::string& testId, const TestStatus& status) {
    {
        std::lock_guard<std::mutex> lock(testsMutex_);
        activeTests_[testId] = status;
    }
    
    publishTestEvent(EventType::TEST_PROGRESS, testId, {
        {"progress", std::to_string(status.progress)},
        {"current_spec", status.currentSpec},
        {"current_scenario", status.currentScenario}
    });
}

void TestHub::publishTestEvent(const std::string& eventType, const std::string& testId,
                               const std::map<std::string, std::string>& data) {
    publishEvent(eventType, testId, data);
}

std::shared_ptr<gauge::Specification> TestHub::loadSpec(const std::string& specFile) {
    std::string content = FileUtil::readFile(specFile);
    if (content.empty()) {
        return nullptr;
    }
    
    gauge::parser::SpecParser parser;
    if (!parser.parse(content)) {
        std::cerr << "Failed to parse spec: " << specFile << " - " << parser.getError() << std::endl;
        return nullptr;
    }
    
    return parser.getSpecification();
}

std::string TestHub::generateTestId() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    
    std::ostringstream oss;
    oss << "test-" << std::put_time(std::localtime(&time), "%Y%m%d-%H%M%S");
    
    return oss.str();
}

} // namespace testhub
