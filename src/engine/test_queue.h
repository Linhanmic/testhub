/*
 * TestHub - 测试队列
 * 优先级队列管理测试任务
 */

#pragma once

#include "../model/types.h"
#include "../event/event_bus.h"

#include <queue>
#include <vector>
#include <set>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <map>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace testhub {

/**
 * 测试任务
 */
struct TestTask {
    TestRequest request;
    std::chrono::system_clock::time_point submitTime;
    
    // 用于优先级队列的比较
    bool operator<(const TestTask& other) const {
        // 优先级高的排在前面
        if (request.priority != other.request.priority) {
            return request.priority < other.request.priority;
        }
        // 同优先级按提交时间排序（先提交的排在前面）
        return submitTime > other.submitTime;
    }
};

/**
 * 测试队列
 * 管理测试任务的优先级队列
 */
class TestQueue {
public:
    TestQueue() = default;
    ~TestQueue() = default;

    /**
     * 添加任务到队列
     * @param request 测试请求
     * @return 任务 ID
     */
    std::string enqueue(const TestRequest& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        TestTask task;
        task.request = request;
        task.submitTime = std::chrono::system_clock::now();
        
        // 如果没有指定 ID，生成一个
        if (task.request.id.empty()) {
            task.request.id = generateTaskId();
        }
        
        queue_.push(task);
        taskIds_.insert(task.request.id);
        
        // 发布队列更新事件
        publishEvent(EventType::QUEUE_UPDATED, "", {
            {"queue_size", std::to_string(queue_.size())},
            {"task_id", task.request.id}
        });
        
        return task.request.id;
    }

    /**
     * 从队列取出下一个任务
     * @return 测试任务（如果队列为空返回 nullopt）
     */
    std::optional<TestTask> dequeue() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (queue_.empty()) {
            return std::nullopt;
        }
        
        TestTask task = queue_.top();
        queue_.pop();
        taskIds_.erase(task.request.id);
        
        return task;
    }

    /**
     * 取消任务
     * @param taskId 任务 ID
     * @return 是否成功取消
     */
    bool cancel(const std::string& taskId) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查任务是否在队列中
        if (taskIds_.find(taskId) == taskIds_.end()) {
            return false;
        }
        
        // 重建队列，排除要取消的任务
        std::priority_queue<TestTask> newQueue;
        while (!queue_.empty()) {
            TestTask task = queue_.top();
            queue_.pop();
            
            if (task.request.id != taskId) {
                newQueue.push(task);
            } else {
                taskIds_.erase(taskId);
                
                // 发布取消事件
                publishEvent(EventType::TEST_CANCELLED, taskId);
            }
        }
        
        queue_ = std::move(newQueue);
        return true;
    }

    /**
     * 检查任务是否在队列中
     * @param taskId 任务 ID
     * @return 是否在队列中
     */
    bool contains(const std::string& taskId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return taskIds_.find(taskId) != taskIds_.end();
    }

    /**
     * 获取队列大小
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * 检查队列是否为空
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /**
     * 清空队列
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            queue_.pop();
        }
        taskIds_.clear();
    }

    /**
     * 获取队列中的所有任务 ID
     */
    std::vector<std::string> getTaskIds() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<std::string>(taskIds_.begin(), taskIds_.end());
    }

private:
    // 优先级队列
    std::priority_queue<TestTask> queue_;
    
    // 任务 ID 集合（用于快速查找）
    std::set<std::string> taskIds_;
    
    // 互斥锁
    mutable std::mutex mutex_;
    
    // 任务 ID 计数器
    int taskCounter_ = 0;

    /**
     * 生成任务 ID
     */
    std::string generateTaskId() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        std::ostringstream oss;
        oss << "test-" << std::put_time(std::localtime(&time), "%Y%m%d-%H%M%S") 
            << "-" << std::setfill('0') << std::setw(3) << (++taskCounter_ % 1000);
        
        return oss.str();
    }
};

} // namespace testhub
