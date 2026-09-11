/*
 * TestHub - 事件总线实现
 */

#include "event_bus.h"

#include <algorithm>

namespace testhub {

EventBus::EventBus() {
    dispatcher_ = std::thread(&EventBus::dispatchLoop, this);
}

EventBus::~EventBus() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queueCv_.notify_all();
    }
    if (dispatcher_.joinable()) dispatcher_.join();
}

bool EventBus::matches(const std::string& pattern, const std::string& type) {
    if (pattern == "*" || pattern == type) return true;
    if (pattern.size() >= 2 && pattern.back() == '*' && pattern[pattern.size() - 2] == '.') {
        return type.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
    }
    return false;
}

std::string EventBus::subscribe(const std::string& eventType, EventHandler handler) {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    std::string id = "handler_" + std::to_string(++handlerCounter_);
    handlers_.push_back({id, eventType, std::move(handler)});
    return id;
}

void EventBus::unsubscribe(const std::string& handlerId) {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    handlers_.erase(std::remove_if(handlers_.begin(), handlers_.end(),
                                   [&](const HandlerEntry& e) { return e.id == handlerId; }),
                    handlers_.end());
}

void EventBus::publish(const Event& event) {
    ++published_;
    {
        std::lock_guard<std::mutex> lock(historyMutex_);
        history_.push_back(event);
        while (history_.size() > historyLimit_) history_.pop_front();
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.push_back(event);
    }
    queueCv_.notify_one();
}

void EventBus::dispatchLoop() {
    while (true) {
        Event event;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_) return;
                continue;
            }
            event = queue_.front();
            queue_.pop_front();
            dispatching_ = true;
        }
        std::vector<EventHandler> targets;
        {
            std::lock_guard<std::mutex> lock(handlersMutex_);
            for (const auto& h : handlers_) {
                if (matches(h.pattern, event.type)) targets.push_back(h.handler);
            }
        }
        for (auto& h : targets) {
            try {
                h(event);
            } catch (...) {
                // 处理器异常不得影响其他订阅者
            }
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            dispatching_ = false;
            if (queue_.empty()) idleCv_.notify_all();
        }
    }
}

void EventBus::waitForIdle(int timeoutMs) {
    std::unique_lock<std::mutex> lock(queueMutex_);
    idleCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                     [&] { return queue_.empty() && !dispatching_; });
}

std::vector<Event> EventBus::recentEvents(size_t limit, const std::string& testId) const {
    std::lock_guard<std::mutex> lock(historyMutex_);
    std::vector<Event> out;
    for (auto it = history_.rbegin(); it != history_.rend() && out.size() < limit; ++it) {
        if (!testId.empty() && it->testId != testId) continue;
        out.push_back(*it);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void EventBus::setHistoryLimit(size_t limit) {
    std::lock_guard<std::mutex> lock(historyMutex_);
    historyLimit_ = std::max<size_t>(1, limit);
    while (history_.size() > historyLimit_) history_.pop_front();
}

void EventBus::clear() {
    waitForIdle(1000);
    {
        std::lock_guard<std::mutex> lock(handlersMutex_);
        handlers_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(historyMutex_);
        history_.clear();
    }
}

size_t EventBus::getHandlerCount(const std::string& eventType) const {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    if (eventType.empty()) return handlers_.size();
    size_t n = 0;
    for (const auto& h : handlers_) if (h.pattern == eventType) ++n;
    return n;
}

} // namespace testhub
