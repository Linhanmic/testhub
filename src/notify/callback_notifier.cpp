/*
 * TestHub - 完成回调通知实现
 */

#include "callback_notifier.h"

#include "../engine/execution_engine.h"
#include "../event/event_bus.h"
#include "../model/json_convert.h"
#include "../util/http_client.h"
#include "../util/logger.h"
#include "../util/time_util.h"

#include <algorithm>

namespace testhub {

CallbackNotifier::CallbackNotifier(ExecutionEngine& engine) : engine_(engine) {}

CallbackNotifier::~CallbackNotifier() { stop(); }

void CallbackNotifier::configure(const CallbackConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    if (config_.maxAttempts < 1) config_.maxAttempts = 1;
    if (config_.retryBackoffMs < 0) config_.retryBackoffMs = 0;
    if (config_.timeoutMs <= 0) config_.timeoutMs = 10000;
    while (!config_.publicBaseUrl.empty() && config_.publicBaseUrl.back() == '/') config_.publicBaseUrl.pop_back();
}

void CallbackNotifier::start() {
    if (running_.exchange(true)) return;
    if (!config_.enabled) {
        running_ = false;
        return;
    }
    worker_ = std::thread([this] { workerLoop(); });
    completedHandlerId_ = EventBus::getInstance().subscribe(EventType::TEST_COMPLETED, [this](const Event& e) {
        enqueue(e.testId, e.type);
    });
    // 运行中被取消的测试最终仍会发出 test.completed；只有在排队阶段被取消的测试需要单独处理
    cancelledHandlerId_ = EventBus::getInstance().subscribe(EventType::TEST_CANCELLED, [this](const Event& e) {
        auto it = e.data.find("stage");
        if (it != e.data.end() && it->second == "queued") enqueue(e.testId, e.type);
    });
}

void CallbackNotifier::stop() {
    if (!running_.exchange(false)) return;
    if (!completedHandlerId_.empty()) EventBus::getInstance().unsubscribe(completedHandlerId_);
    if (!cancelledHandlerId_.empty()) EventBus::getInstance().unsubscribe(cancelledHandlerId_);
    completedHandlerId_.clear();
    cancelledHandlerId_.clear();
    {
        // 持锁通知：workerLoop 在锁内检查 running_ 后才 wait，否则可能丢失唤醒导致 join 卡死
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.clear();
    stats_.pending = 0;
}

CallbackStats CallbackNotifier::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool CallbackNotifier::enqueue(const std::string& testId, const std::string& eventType) {
    auto record = engine_.getRecord(testId);
    if (!record || record->request.callbackUrl.empty()) return false;
    Job job;
    job.testId = testId;
    job.url = record->request.callbackUrl;
    job.eventType = eventType;
    job.attempt = 0;
    job.due = Clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return false;
        stats_.pending++;
    }
    schedule(std::move(job));
    return true;
}

void CallbackNotifier::schedule(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void CallbackNotifier::workerLoop() {
    while (running_) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (running_) {
                if (jobs_.empty()) {
                    cv_.wait(lock);
                    continue;
                }
                auto next = std::min_element(jobs_.begin(), jobs_.end(), [](const Job& a, const Job& b) { return a.due < b.due; });
                if (next->due <= Clock::now()) {
                    job = std::move(*next);
                    jobs_.erase(next);
                    break;
                }
                cv_.wait_until(lock, next->due);
            }
            if (!running_) return;
        }
        process(std::move(job));
    }
}

void CallbackNotifier::process(Job job) {
    auto record = engine_.getRecord(job.testId);
    if (!record) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stats_.pending > 0) stats_.pending--;
        return;
    }
    CallbackConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = config_;
        stats_.attempts++;
    }
    job.attempt++;

    Json payload = buildPayload(*record, job.eventType, cfg.publicBaseUrl);
    payload["attempt"] = job.attempt;
    std::map<std::string, std::string> headers;
    headers["X-TestHub-Event"] = job.eventType;
    headers["X-TestHub-Test-Id"] = job.testId;
    headers["X-TestHub-Attempt"] = std::to_string(job.attempt);
    HttpClientResponse resp = HttpClient::post(job.url, payload.dump(), "application/json; charset=utf-8", cfg.timeoutMs, headers);

    bool success = resp.ok && resp.status >= 200 && resp.status < 300;
    bool retryable = !resp.ok || isRetryableStatus(resp.status);
    std::string detail = resp.ok ? "HTTP " + std::to_string(resp.status) : resp.error;

    if (success) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.delivered++;
            if (stats_.pending > 0) stats_.pending--;
        }
        TH_LOG_INFO("callback", "Delivered " + job.eventType + " for " + job.testId + " to " + job.url + " (" + detail +
                                    ", attempt " + std::to_string(job.attempt) + ")");
        publishEvent(EventType::CALLBACK_DELIVERED, job.testId,
                     {{"url", job.url}, {"status", std::to_string(resp.status)}, {"attempts", std::to_string(job.attempt)}});
        return;
    }

    if (retryable && job.attempt < cfg.maxAttempts && running_) {
        int backoff = cfg.retryBackoffMs;
        for (int i = 1; i < job.attempt; ++i) backoff *= 2;
        TH_LOG_WARN("callback", "Callback to " + job.url + " for " + job.testId + " failed (" + detail + "); retry " +
                                    std::to_string(job.attempt + 1) + "/" + std::to_string(cfg.maxAttempts) + " in " +
                                    std::to_string(backoff) + " ms");
        job.due = Clock::now() + std::chrono::milliseconds(backoff);
        schedule(std::move(job));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.failed++;
        if (stats_.pending > 0) stats_.pending--;
    }
    TH_LOG_ERROR("callback", "Giving up on callback to " + job.url + " for " + job.testId + " after " +
                                 std::to_string(job.attempt) + " attempt(s): " + detail);
    publishEvent(EventType::CALLBACK_FAILED, job.testId,
                 {{"url", job.url}, {"error", detail}, {"attempts", std::to_string(job.attempt)},
                  {"status", std::to_string(resp.status)}});
}

Json CallbackNotifier::buildPayload(const TestRecord& record, const std::string& eventType, const std::string& baseUrl) {
    const TestRequest& req = record.request;
    const TestStatus& st = record.status;
    Json j = Json::object();
    j["event"] = eventType;
    j["test_id"] = req.id;
    j["name"] = req.name;
    j["state"] = testStateToString(st.state);
    j["submitted_by"] = req.submittedBy;
    j["environment"] = req.environment;
    j["spec_files"] = toJson(req.specFiles);
    j["tags"] = toJson(req.tags);
    j["metadata"] = toJson(req.metadata);
    j["total_scenarios"] = st.totalScenarios;
    j["executed_scenarios"] = st.executedScenarios;
    j["passed_scenarios"] = st.passedScenarios;
    j["failed_scenarios"] = st.failedScenarios;
    j["skipped_scenarios"] = st.skippedScenarios;
    j["submit_time"] = TimeUtil::toIso8601(st.submitTime);
    j["start_time"] = TimeUtil::toIso8601(st.startTime);
    j["end_time"] = TimeUtil::toIso8601(st.endTime);
    j["duration"] = record.hasResult ? record.result.totalDuration : 0.0;
    j["errors"] = toJson(record.hasResult ? record.result.errors : st.errors);
    j["warnings"] = toJson(record.hasResult ? record.result.warnings : st.warnings);

    // 失败场景清单：便于接收方直接展示，不必再拉取完整结果树
    Json failed = Json::array();
    if (record.hasResult) {
        for (const auto& spec : record.result.specResults) {
            for (const auto& sc : spec.scenarioResults) {
                if (sc.state != TestState::FAILED && sc.state != TestState::TEST_ERROR) continue;
                Json f = Json::object();
                f["spec"] = spec.specFile;
                f["scenario"] = sc.scenarioName;
                if (sc.dataRowIndex >= 0) f["data_row_index"] = sc.dataRowIndex;
                f["state"] = testStateToString(sc.state);
                f["error"] = sc.errorMessage;
                failed.push(f);
            }
        }
    }
    j["failed_scenarios_detail"] = failed;

    std::string base = baseUrl + "/api/v1/tests/" + req.id;
    Json links = Json::object();
    links["status"] = base;
    links["result"] = base + "/result";
    links["report_junit"] = base + "/report?format=junit";
    links["report_html"] = base + "/report?format=html";
    links["ui"] = baseUrl + "/#/tests/" + req.id;
    j["links"] = links;
    j["sent_at"] = TimeUtil::toIso8601(TimeUtil::now());
    return j;
}

} // namespace testhub
