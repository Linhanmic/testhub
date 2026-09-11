/*
 * TestHub - 规范目录监控实现
 */

#include "spec_watcher.h"

#include "../event/event_bus.h"
#include "../model/types.h"
#include "../util/logger.h"
#include "../util/string_util.h"
#include "../util/time_util.h"
#include "spec_repository.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace testhub {
namespace spec {

namespace {

long long nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

long long fileTimeToEpochMs(fs::file_time_type t) {
    // C++17 没有 file_clock → system_clock 的可移植转换，用两个 now() 的差值近似（与 SpecRepository::list 一致）
    auto sys = std::chrono::time_point_cast<std::chrono::milliseconds>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return sys.time_since_epoch().count();
}

std::string joinLimited(const std::vector<std::string>& items, size_t limit) {
    if (items.size() <= limit) return StringUtil::join(items, ",");
    std::vector<std::string> head(items.begin(), items.begin() + static_cast<long>(limit));
    return StringUtil::join(head, ",") + ",…(+" + std::to_string(items.size() - limit) + ")";
}

} // namespace

SpecWatcher::SpecWatcher(SpecRepository& specs) : specs_(specs) {}

SpecWatcher::~SpecWatcher() { stop(); }

void SpecWatcher::configure(const SpecWatcherConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    if (config_.intervalMs <= 0) config_.enabled = false;
    if (config_.settleMs < 0) config_.settleMs = 0;
}

SpecWatcher::Snapshot SpecWatcher::takeSnapshot(long long nowMs, std::vector<std::string>* unsettled) const {
    Snapshot snap;
    std::vector<std::string> roots = {specs_.specsDir()};
    const std::string& concepts = specs_.conceptsDir();
    std::string prefix = specs_.specsDir();
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
    if (!concepts.empty() && concepts != specs_.specsDir() && !StringUtil::startsWith(concepts, prefix)) {
        roots.push_back(concepts);
    }
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (!ec && it != end) {
            const fs::directory_entry& entry = *it;
            std::string path = entry.path().lexically_normal().generic_string();
            if (entry.is_regular_file(ec) && (SpecRepository::isSpecFile(path) || SpecRepository::isConceptFile(path))) {
                Signature sig;
                std::error_code fec;
                auto mtime = fs::last_write_time(entry.path(), fec);
                if (!fec) sig.mtimeMs = fileTimeToEpochMs(mtime);
                auto size = fs::file_size(entry.path(), fec);
                if (!fec) sig.size = static_cast<long long>(size);
                if (unsettled && config_.settleMs > 0 && nowMs - sig.mtimeMs < config_.settleMs) {
                    unsettled->push_back(path);
                }
                snap[path] = sig;
            }
            it.increment(ec);
        }
    }
    return snap;
}

SpecChangeSet SpecWatcher::diff(const Snapshot& before, const Snapshot& after) const {
    SpecChangeSet changes;
    for (const auto& kv : after) {
        auto it = before.find(kv.first);
        if (it == before.end()) changes.created.push_back(specs_.toRelative(kv.first));
        else if (it->second != kv.second) changes.updated.push_back(specs_.toRelative(kv.first));
        else continue;
        if (SpecRepository::isConceptFile(kv.first)) changes.conceptsChanged = true;
    }
    for (const auto& kv : before) {
        if (after.find(kv.first) == after.end()) {
            changes.deleted.push_back(specs_.toRelative(kv.first));
            if (SpecRepository::isConceptFile(kv.first)) changes.conceptsChanged = true;
        }
    }
    std::sort(changes.created.begin(), changes.created.end());
    std::sort(changes.updated.begin(), changes.updated.end());
    std::sort(changes.deleted.begin(), changes.deleted.end());
    return changes;
}

SpecChangeSet SpecWatcher::scan() {
    SpecChangeSet changes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        long long nowMs = nowEpochMs();
        std::vector<std::string> unsettled;
        Snapshot after = takeSnapshot(nowMs, &unsettled);
        // 仍在写入的文件：沿用旧签名（已存在）或暂不纳入（新建），等下一轮稳定后再报告
        for (const auto& path : unsettled) {
            auto old = snapshot_.find(path);
            if (old != snapshot_.end()) after[path] = old->second;
            else after.erase(path);
        }
        changes = diff(snapshot_, after);
        snapshot_ = std::move(after);
        scans_++;
        if (!changes.empty()) {
            changes_ += changes.size();
            reloads_++;
            lastChangeAt_ = TimeUtil::toIso8601(TimeUtil::now());
        }
    }
    if (changes.empty()) return changes;

    if (changes.conceptsChanged) {
        for (const auto& e : specs_.reloadConcepts()) {
            TH_LOG_WARN("specs", e.fileName + ":" + std::to_string(e.lineNumber) + ": " + e.message);
        }
    }
    std::string summary;
    if (!changes.created.empty()) summary += " created: " + joinLimited(changes.created, 5);
    if (!changes.updated.empty()) summary += " updated: " + joinLimited(changes.updated, 5);
    if (!changes.deleted.empty()) summary += " deleted: " + joinLimited(changes.deleted, 5);
    TH_LOG_INFO("specs", "Detected " + std::to_string(changes.size()) + " change(s) in specs directory:" + summary);

    std::map<std::string, std::string> data;
    data["source"] = "watcher";
    data["created"] = std::to_string(changes.created.size());
    data["updated"] = std::to_string(changes.updated.size());
    data["deleted"] = std::to_string(changes.deleted.size());
    std::vector<std::string> files;
    files.insert(files.end(), changes.created.begin(), changes.created.end());
    files.insert(files.end(), changes.updated.begin(), changes.updated.end());
    files.insert(files.end(), changes.deleted.begin(), changes.deleted.end());
    data["files"] = joinLimited(files, 20);
    data["concepts"] = std::to_string(specs_.concepts().size());
    if (changes.conceptsChanged) data["concepts_reloaded"] = "true";
    publishEvent(EventType::SPECS_RELOADED, "", data);
    return changes;
}

void SpecWatcher::acknowledge() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = takeSnapshot(nowEpochMs(), nullptr);
}

void SpecWatcher::start() {
    if (running_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = takeSnapshot(nowEpochMs(), nullptr);
        stopRequested_ = false;
        if (!config_.enabled) {
            TH_LOG_INFO("specs", "Spec directory watcher disabled");
            return;
        }
    }
    thread_ = std::thread(&SpecWatcher::loop, this);
    TH_LOG_INFO("specs", "Watching " + specs_.specsDir() + " for changes every " + std::to_string(config_.intervalMs) + " ms");
}

void SpecWatcher::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
}

void SpecWatcher::loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(config_.intervalMs), [&] { return stopRequested_; });
            if (stopRequested_) return;
        }
        scan();
    }
}

SpecWatcherStats SpecWatcher::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SpecWatcherStats s;
    s.enabled = config_.enabled && running_;
    s.intervalMs = config_.intervalMs;
    s.trackedFiles = snapshot_.size();
    s.scans = scans_;
    s.changes = changes_;
    s.reloads = reloads_;
    s.lastChangeAt = lastChangeAt_;
    return s;
}

} // namespace spec
} // namespace testhub
