/*
 * TestHub - 规范目录监控
 * 以轮询方式发现规范目录中 .spec/.md/.cpt 文件的新增、修改与删除：
 * 概念文件变化时重新加载概念字典，并发布 specs.reloaded 事件通知 UI 与订阅者。
 *
 * 选择轮询而非 inotify/FSEvents/ReadDirectoryChangesW：零依赖、跨平台、实现简单；
 * 规范目录通常只有几十到几百个文件，秒级轮询的开销可以忽略。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace testhub {
namespace spec {

class SpecRepository;

struct SpecWatcherConfig {
    bool enabled = true;
    int intervalMs = 2000;      // 轮询间隔；<= 0 视为禁用
    int settleMs = 200;         // 修改时间距现在不足该值的文件视为"仍在写入"，推迟到下一轮
};

struct SpecChangeSet {
    std::vector<std::string> created;   // 相对规范目录的路径
    std::vector<std::string> updated;
    std::vector<std::string> deleted;
    bool conceptsChanged = false;
    bool empty() const { return created.empty() && updated.empty() && deleted.empty(); }
    size_t size() const { return created.size() + updated.size() + deleted.size(); }
};

struct SpecWatcherStats {
    bool enabled = false;
    int intervalMs = 0;
    size_t trackedFiles = 0;
    unsigned long long scans = 0;
    unsigned long long changes = 0;        // 累计变更文件数
    unsigned long long reloads = 0;        // 发布 specs.reloaded 的次数
    std::string lastChangeAt;              // ISO-8601；从未变更为空
};

class SpecWatcher {
public:
    explicit SpecWatcher(SpecRepository& specs);
    ~SpecWatcher();

    SpecWatcher(const SpecWatcher&) = delete;
    SpecWatcher& operator=(const SpecWatcher&) = delete;

    void configure(const SpecWatcherConfig& config);
    const SpecWatcherConfig& config() const { return config_; }

    /**
     * 记录当前目录快照并启动后台轮询线程；禁用时只记录快照
     */
    void start();
    void stop();
    bool running() const { return running_; }

    /**
     * 立即执行一次扫描（后台线程也调用它）。发现变化时重新加载概念（若有 .cpt 变化）
     * 并发布 specs.reloaded；返回本次变更集。测试可在不启动线程的情况下直接调用。
     */
    SpecChangeSet scan();

    /**
     * 重新记录快照但不发布事件：API 自身写入/删除文件后调用，避免同一变更被报告两次
     */
    void acknowledge();

    SpecWatcherStats stats() const;

private:
    struct Signature {
        long long mtimeMs = 0;
        long long size = 0;
        bool operator==(const Signature& o) const { return mtimeMs == o.mtimeMs && size == o.size; }
        bool operator!=(const Signature& o) const { return !(*this == o); }
    };
    using Snapshot = std::map<std::string, Signature>;  // 绝对路径 → 签名

    Snapshot takeSnapshot(long long nowMs, std::vector<std::string>* unsettled) const;
    SpecChangeSet diff(const Snapshot& before, const Snapshot& after) const;
    void loop();

    SpecRepository& specs_;
    SpecWatcherConfig config_;
    Snapshot snapshot_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    bool stopRequested_ = false;  // 受 mutex_ 保护，供 cv_ 谓词使用

    unsigned long long scans_ = 0;
    unsigned long long changes_ = 0;
    unsigned long long reloads_ = 0;
    std::string lastChangeAt_;
};

} // namespace spec
} // namespace testhub
