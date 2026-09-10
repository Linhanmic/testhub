/*
 * TestHub - 测试队列
 * 线程安全的优先级队列（同优先级 FIFO），支持取消与阻塞等待
 */

#pragma once

#include "../model/types.h"
#include "../event/event_bus.h"
#include "../util/time_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace testhub {

/**
 * 测试任务
 */
struct TestTask {
    TestRequest request;
    TimePoint submitTime;
    unsigned long long sequence = 0;
};

/**
 * 测试队列
 */
class TestQueue {
public:
    TestQueue() = default;
    ~TestQueue() { close(); }

    /**
     * 入队；返回任务 ID（request.id 为空时自动生成）
     */
    std::string enqueue(const TestRequest& request) {
        TestTask task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task.request = request;
            task.submitTime = std::chrono::system_clock::now();
            task.sequence = ++sequence_;
            if (task.request.id.empty()) task.request.id = generateTaskId(task.submitTime);
            // 按优先级降序、序号升序插入
            auto pos = std::find_if(items_.begin(), items_.end(), [&](const TestTask& t) {
                return static_cast<int>(t.request.priority) < static_cast<int>(task.request.priority);
            });
            items_.insert(pos, task);
        }
        cv_.notify_one();
        publishEvent(EventType::QUEUE_UPDATED, task.request.id, {
            {"queue_size", std::to_string(size())},
            {"action", "enqueued"}
        });
        return task.request.id;
    }

    /**
     * 非阻塞出队
     */
    std::optional<TestTask> dequeue() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) return std::nullopt;
        TestTask t = items_.front();
        items_.pop_front();
        return t;
    }

    /**
     * 阻塞出队，直到有任务、超时或队列关闭
     */
    std::optional<TestTask> waitAndDequeue(int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return closed_ || !items_.empty(); });
        if (items_.empty()) return std::nullopt;
        TestTask t = items_.front();
        items_.pop_front();
        return t;
    }

    /**
     * 取消排队中的任务
     */
    bool cancel(const std::string& taskId) {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = std::find_if(items_.begin(), items_.end(), [&](const TestTask& t) { return t.request.id == taskId; });
            if (it != items_.end()) {
                items_.erase(it);
                removed = true;
            }
        }
        if (removed) {
            publishEvent(EventType::QUEUE_UPDATED, taskId, {{"queue_size", std::to_string(size())}, {"action", "cancelled"}});
        }
        return removed;
    }

    bool contains(const std::string& taskId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::any_of(items_.begin(), items_.end(), [&](const TestTask& t) { return t.request.id == taskId; });
    }

    /**
     * 队列中的位置（0 表示下一个执行）；不在队列返回 -1
     */
    int position(const std::string& taskId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int idx = 0;
        for (const auto& t : items_) {
            if (t.request.id == taskId) return idx;
            ++idx;
        }
        return -1;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    bool empty() const { return size() == 0; }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
    }

    /**
     * 关闭队列：唤醒所有等待者
     */
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    void reopen() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = false;
    }

    std::vector<TestTask> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<TestTask>(items_.begin(), items_.end());
    }

    std::vector<std::string> getTaskIds() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> ids;
        for (const auto& t : items_) ids.push_back(t.request.id);
        return ids;
    }

    /**
     * 生成任务 ID：test-YYYYMMDD-HHMMSS-NNN
     */
    std::string generateTaskId(const TimePoint& tp) {
        std::string stamp = TimeUtil::formatLocal(tp, "%Y%m%d-%H%M%S");
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "%03u", static_cast<unsigned>(++idCounter_ % 1000));
        return "test-" + stamp + "-" + suffix;
    }

private:
    std::deque<TestTask> items_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_ = false;
    unsigned long long sequence_ = 0;
    unsigned idCounter_ = 0;
};

} // namespace testhub
