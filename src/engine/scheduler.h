/*
 * TestHub - 测试计划调度器
 * 按 UTC cron 表达式周期性提交测试。计划以 JSON 落盘，重启后回放。
 * 选择轮询而非系统 crontab：零依赖、可注入 now 便于测试、与规范目录监控同一模式。
 */

#pragma once

#include "../model/types.h"
#include "../util/cron.h"
#include "../util/json.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace testhub {

struct Schedule {
    std::string id;
    std::string name;
    std::string cron;
    bool enabled = true;
    bool skipIfRunning = true;
    TestRequest request;
    TimePoint createdAt;
    TimePoint updatedAt;
    TimePoint lastRunAt;
    std::string lastTestId;
    std::string lastError;
    unsigned long long runCount = 0;
    unsigned long long skipCount = 0;
    long long lastFiredMinute = -1;  // 内部：已触发过的 UTC 分钟，避免同一分钟重复提交
};

struct SchedulerConfig {
    bool enabled = true;
    int intervalMs = 1000;
    std::string dir = "data/schedules";  // 空 = 仅内存
};

struct SchedulerStats {
    bool enabled = false;
    int intervalMs = 0;
    size_t count = 0;
    size_t enabledCount = 0;
    unsigned long long ticks = 0;
    unsigned long long fires = 0;
    unsigned long long skips = 0;
    unsigned long long errors = 0;
};

class Scheduler {
public:
    static constexpr int kFormatVersion = 1;

    using SubmitFn = std::function<std::string(TestRequest)>;
    using IsActiveFn = std::function<bool(const std::string&)>;

    Scheduler() = default;
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void configure(const SchedulerConfig& config);
    const SchedulerConfig& config() const { return config_; }

    void setSubmit(SubmitFn fn) { submit_ = std::move(fn); }
    void setIsActive(IsActiveFn fn) { isActive_ = std::move(fn); }

    void start();
    void stop();
    bool running() const { return running_; }

    /** 扫描到期计划并提交；测试可注入 now。返回本次触发次数 */
    int tick(TimePoint now);

    Schedule create(Schedule in, std::string& error);
    bool update(const std::string& id, const Schedule& patch, bool hasRequest, std::string& error);
    bool remove(const std::string& id);
    std::optional<Schedule> get(const std::string& id) const;
    std::vector<Schedule> list() const;

    /** 忽略 cron，立即提交一次 */
    std::string fireNow(const std::string& id, std::string& error);

    SchedulerStats stats() const;

    static Json toJson(const Schedule& s, bool includeNext = true);
    static bool fromJson(const Json& json, Schedule& s, std::string& error);

private:
    struct Loaded {
        Schedule schedule;
        CronExpr expr;
    };

    bool loadAll();
    bool saveOne(const Loaded& item);
    bool deleteFile(const std::string& id);
    std::string generateId(TimePoint tp);
    std::string fireLocked(Loaded& item, TimePoint now, bool force, std::string& error);

    SchedulerConfig config_;
    SubmitFn submit_;
    IsActiveFn isActive_;

    mutable std::mutex mutex_;
    std::map<std::string, Loaded> items_;
    std::vector<std::string> order_;
    unsigned idCounter_ = 0;

    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    bool stopRequested_ = false;

    unsigned long long ticks_ = 0;
    unsigned long long fires_ = 0;
    unsigned long long skips_ = 0;
    unsigned long long errors_ = 0;
};

} // namespace testhub
