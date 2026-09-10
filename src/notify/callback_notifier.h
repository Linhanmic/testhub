/*
 * TestHub - 完成回调通知
 * 测试进入终态后，若请求带有 callback_url，则在后台线程向该 URL POST 一份 JSON 摘要，
 * 失败（网络错误或 5xx/429）时按指数退避重试。仅支持 http://。
 */

#pragma once

#include "../model/types.h"
#include "../util/json.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace testhub {

class ExecutionEngine;
struct TestRecord;

struct CallbackConfig {
    bool enabled = true;
    int timeoutMs = 10000;        // 单次请求超时
    int maxAttempts = 3;          // 总尝试次数（含首次）
    int retryBackoffMs = 1000;    // 首次重试等待，之后翻倍
    std::string publicBaseUrl;    // 用于拼接 links 的绝对地址，如 http://ci.example.com:8080；为空则给相对路径
};

struct CallbackStats {
    size_t pending = 0;
    size_t delivered = 0;
    size_t failed = 0;
    size_t attempts = 0;
};

class CallbackNotifier {
public:
    explicit CallbackNotifier(ExecutionEngine& engine);
    ~CallbackNotifier();

    CallbackNotifier(const CallbackNotifier&) = delete;
    CallbackNotifier& operator=(const CallbackNotifier&) = delete;

    void configure(const CallbackConfig& config);
    const CallbackConfig& config() const { return config_; }

    void start();
    void stop();

    CallbackStats stats() const;

    /** 把测试加入投递队列（引擎事件到达时调用；也可手动触发重发） */
    bool enqueue(const std::string& testId, const std::string& eventType);

    /** 回调 JSON 载荷（供测试与文档使用） */
    static Json buildPayload(const TestRecord& record, const std::string& eventType, const std::string& baseUrl);

    /** 判断状态码是否值得重试：5xx 与 429 */
    static bool isRetryableStatus(int status) { return status == 429 || (status >= 500 && status < 600); }

private:
    using Clock = std::chrono::steady_clock;

    struct Job {
        std::string testId;
        std::string url;
        std::string eventType;
        int attempt = 0;
        Clock::time_point due;
    };

    void workerLoop();
    void process(Job job);
    void schedule(Job job);

    ExecutionEngine& engine_;
    CallbackConfig config_;

    std::string completedHandlerId_;
    std::string cancelledHandlerId_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::atomic<bool> running_{false};
    std::thread worker_;

    CallbackStats stats_;
};

} // namespace testhub
