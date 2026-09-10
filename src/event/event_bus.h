/*
 * TestHub - 事件总线
 * 发布/订阅模式的事件系统。事件在独立的分发线程中按顺序投递给处理器，
 * 因此发布方可以在持锁状态下安全发布，处理器也不会阻塞执行引擎。
 * 同时保留最近 N 条事件的环形历史，供 UI 与 API 回放。
 */

#pragma once

#include "../model/types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace testhub {

class EventBus {
public:
    static EventBus& getInstance() {
        static EventBus instance;
        return instance;
    }

    /**
     * 订阅事件（eventType 为 "*" 表示所有事件，支持前缀 "test.*" 形式）
     * @return 处理器 ID
     */
    std::string subscribe(const std::string& eventType, EventHandler handler);

    void unsubscribe(const std::string& handlerId);
    void unsubscribe(const std::string& eventType, const std::string& handlerId) { (void)eventType; unsubscribe(handlerId); }

    /**
     * 发布事件（异步投递）
     */
    void publish(const Event& event);

    /**
     * 等待队列中所有事件被投递完成（主要用于测试与优雅停机）
     */
    void waitForIdle(int timeoutMs = 5000);

    /**
     * 最近的事件（最新在末尾）
     */
    std::vector<Event> recentEvents(size_t limit = 100, const std::string& testId = "") const;

    void setHistoryLimit(size_t limit);

    void clear();
    size_t getHandlerCount(const std::string& eventType = "") const;
    unsigned long long publishedCount() const { return published_; }

private:
    EventBus();
    ~EventBus();
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    struct HandlerEntry {
        std::string id;
        std::string pattern;
        EventHandler handler;
    };

    static bool matches(const std::string& pattern, const std::string& type);

    void dispatchLoop();

    mutable std::mutex handlersMutex_;
    std::vector<HandlerEntry> handlers_;
    int handlerCounter_ = 0;

    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::condition_variable idleCv_;
    std::deque<Event> queue_;
    bool dispatching_ = false;
    std::atomic<bool> stop_{false};
    std::thread dispatcher_;

    mutable std::mutex historyMutex_;
    std::deque<Event> history_;
    size_t historyLimit_ = 500;
    std::atomic<unsigned long long> published_{0};
};

/**
 * 便捷的事件发布函数
 */
inline void publishEvent(const std::string& type, const std::string& testId,
                         const std::map<std::string, std::string>& data = {}) {
    Event event;
    event.type = type;
    event.testId = testId;
    event.timestamp = std::chrono::system_clock::now();
    event.data = data;
    EventBus::getInstance().publish(event);
}

} // namespace testhub
