/*
 * TestHub - 事件总线
 * 发布/订阅模式的事件系统
 */

#pragma once

#include "../model/types.h"

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <functional>
#include <algorithm>

namespace testhub {

/**
 * 事件总线
 * 支持发布/订阅模式的事件系统
 */
class EventBus {
public:
    /**
     * 获取单例实例
     */
    static EventBus& getInstance() {
        static EventBus instance;
        return instance;
    }

    /**
     * 订阅事件
     * @param eventType 事件类型
     * @param handler 事件处理器
     * @return 处理器 ID（用于取消订阅）
     */
    std::string subscribe(const std::string& eventType, EventHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string handlerId = generateHandlerId();
        handlers_[eventType].push_back({handlerId, handler});
        
        return handlerId;
    }

    /**
     * 取消订阅
     * @param eventType 事件类型
     * @param handlerId 处理器 ID
     */
    void unsubscribe(const std::string& eventType, const std::string& handlerId) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = handlers_.find(eventType);
        if (it != handlers_.end()) {
            auto& handlers = it->second;
            handlers.erase(
                std::remove_if(handlers.begin(), handlers.end(),
                    [&handlerId](const HandlerEntry& entry) {
                        return entry.id == handlerId;
                    }),
                handlers.end()
            );
        }
    }

    /**
     * 发布事件
     * @param event 事件对象
     */
    void publish(const Event& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 通知特定类型的处理器
        auto it = handlers_.find(event.type);
        if (it != handlers_.end()) {
            for (const auto& entry : it->second) {
                try {
                    entry.handler(event);
                } catch (...) {
                    // 忽略处理器异常
                }
            }
        }
        
        // 通知通配符处理器
        auto wildcardIt = handlers_.find("*");
        if (wildcardIt != handlers_.end()) {
            for (const auto& entry : wildcardIt->second) {
                try {
                    entry.handler(event);
                } catch (...) {
                    // 忽略处理器异常
                }
            }
        }
    }

    /**
     * 清除所有处理器
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_.clear();
    }

    /**
     * 获取处理器数量
     */
    size_t getHandlerCount(const std::string& eventType = "") const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (eventType.empty()) {
            size_t count = 0;
            for (const auto& pair : handlers_) {
                count += pair.second.size();
            }
            return count;
        }
        
        auto it = handlers_.find(eventType);
        if (it != handlers_.end()) {
            return it->second.size();
        }
        return 0;
    }

private:
    EventBus() = default;
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    struct HandlerEntry {
        std::string id;
        EventHandler handler;
    };

    // 处理器映射
    std::map<std::string, std::vector<HandlerEntry>> handlers_;
    
    // 互斥锁
    mutable std::mutex mutex_;
    
    // 处理器 ID 计数器
    int handlerCounter_ = 0;

    /**
     * 生成处理器 ID
     */
    std::string generateHandlerId() {
        return "handler_" + std::to_string(++handlerCounter_);
    }
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
