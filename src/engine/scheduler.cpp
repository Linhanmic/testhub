#include "scheduler.h"
#include "../event/event_bus.h"
#include "../model/json_convert.h"
#include "../util/file_util.h"
#include "../util/logger.h"
#include "../util/time_util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <optional>

namespace testhub {

namespace fs = std::filesystem;

namespace {

bool safeId(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    }
    return id.find("..") == std::string::npos;
}

} // namespace

Scheduler::~Scheduler() { stop(); }

void Scheduler::configure(const SchedulerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    if (config_.intervalMs < 50) config_.intervalMs = 50;
}

void Scheduler::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    if (!config_.dir.empty()) {
        std::error_code ec;
        fs::create_directories(config_.dir, ec);
        if (ec || !fs::is_directory(config_.dir)) {
            TH_LOG_ERROR("schedule", "Cannot create schedules directory '" + config_.dir + "': " + ec.message() +
                                         "; persistence disabled");
            config_.dir.clear();
        }
    }
    loadAll();
    if (!config_.enabled) {
        TH_LOG_INFO("schedule", "Scheduler disabled (" + std::to_string(items_.size()) + " plan(s) loaded)");
        return;
    }
    stopRequested_ = false;
    running_ = true;
    thread_ = std::thread([this] {
        while (true) {
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait_for(lk, std::chrono::milliseconds(config_.intervalMs), [&] { return stopRequested_; });
                if (stopRequested_) return;
            }
            tick(TimeUtil::now());
        }
    });
    TH_LOG_INFO("schedule", "Scheduler watching " + std::to_string(items_.size()) + " plan(s) every " +
                                std::to_string(config_.intervalMs) + " ms" +
                                (config_.dir.empty() ? " (memory only)" : " in " + config_.dir));
}

void Scheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !thread_.joinable()) return;
        stopRequested_ = true;
        running_ = false;
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
}

int Scheduler::tick(TimePoint now) {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++ticks_;
        ids = order_;
    }
    int fired = 0;
    long long minute = CronExpr::epochMinute(now);
    for (const auto& id : ids) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(id);
        if (it == items_.end()) continue;
        Loaded& item = it->second;
        if (!item.schedule.enabled) continue;
        if (item.schedule.lastFiredMinute == minute) continue;
        if (!item.expr.matches(now)) continue;
        item.schedule.lastFiredMinute = minute;
        std::string error;
        fireLocked(item, now, false, error);
        ++fired;
        saveOne(item);
    }
    return fired;
}

Schedule Scheduler::create(Schedule in, std::string& error) {
    CronExpr expr;
    if (!CronExpr::parse(in.cron, expr, &error)) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    TimePoint now = TimeUtil::now();
    if (in.id.empty()) in.id = generateId(now);
    else if (!safeId(in.id) || items_.count(in.id)) {
        error = items_.count(in.id) ? "Schedule id already exists" : "Invalid schedule id";
        return {};
    }
    if (in.name.empty()) in.name = in.id;
    in.createdAt = now;
    in.updatedAt = now;
    in.cron = expr.source();
    Loaded item;
    item.schedule = std::move(in);
    item.expr = std::move(expr);
    std::string id = item.schedule.id;
    items_[id] = item;
    order_.push_back(id);
    saveOne(items_[id]);
    publishEvent(EventType::SCHEDULE_CREATED, "", {{"schedule_id", id}, {"name", items_[id].schedule.name},
                                                   {"cron", items_[id].schedule.cron}});
    return items_[id].schedule;
}

bool Scheduler::update(const std::string& id, const Schedule& patch, bool hasRequest, std::string& error) {
    std::optional<CronExpr> parsed;
    if (!patch.cron.empty()) {
        CronExpr expr;
        if (!CronExpr::parse(patch.cron, expr, &error)) return false;
        parsed = std::move(expr);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = items_.find(id);
    if (it == items_.end()) { error = "Schedule not found: " + id; return false; }
    Loaded& item = it->second;
    if (!patch.name.empty()) item.schedule.name = patch.name;
    if (parsed && patch.cron != item.schedule.cron) {
        item.schedule.cron = parsed->source();
        item.expr = std::move(*parsed);
        item.schedule.lastFiredMinute = -1;
    }
    item.schedule.enabled = patch.enabled;
    item.schedule.skipIfRunning = patch.skipIfRunning;
    if (hasRequest) item.schedule.request = patch.request;
    item.schedule.updatedAt = TimeUtil::now();
    saveOne(item);
    publishEvent(EventType::SCHEDULE_UPDATED, "", {{"schedule_id", id}, {"name", item.schedule.name},
                                                   {"cron", item.schedule.cron},
                                                   {"enabled", item.schedule.enabled ? "true" : "false"}});
    return true;
}

bool Scheduler::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = items_.find(id);
    if (it == items_.end()) return false;
    std::string name = it->second.schedule.name;
    items_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    deleteFile(id);
    publishEvent(EventType::SCHEDULE_DELETED, "", {{"schedule_id", id}, {"name", name}});
    return true;
}

std::optional<Schedule> Scheduler::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = items_.find(id);
    if (it == items_.end()) return std::nullopt;
    return it->second.schedule;
}

std::vector<Schedule> Scheduler::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Schedule> out;
    out.reserve(order_.size());
    for (const auto& id : order_) {
        auto it = items_.find(id);
        if (it != items_.end()) out.push_back(it->second.schedule);
    }
    return out;
}

std::string Scheduler::fireNow(const std::string& id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = items_.find(id);
    if (it == items_.end()) { error = "Schedule not found: " + id; return ""; }
    TimePoint now = TimeUtil::now();
    it->second.schedule.lastFiredMinute = CronExpr::epochMinute(now);
    std::string testId = fireLocked(it->second, now, true, error);
    saveOne(it->second);
    return testId;
}

SchedulerStats Scheduler::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SchedulerStats s;
    s.enabled = config_.enabled && running_;
    s.intervalMs = config_.intervalMs;
    s.count = items_.size();
    for (const auto& kv : items_) if (kv.second.schedule.enabled) ++s.enabledCount;
    s.ticks = ticks_;
    s.fires = fires_;
    s.skips = skips_;
    s.errors = errors_;
    return s;
}

Json Scheduler::toJson(const Schedule& s, bool includeNext) {
    Json j = Json::object();
    j["id"] = s.id;
    j["name"] = s.name;
    j["cron"] = s.cron;
    j["enabled"] = s.enabled;
    j["skip_if_running"] = s.skipIfRunning;
    j["request"] = testhub::toJson(s.request);
    j["created_at"] = TimeUtil::toIso8601(s.createdAt);
    j["updated_at"] = TimeUtil::toIso8601(s.updatedAt);
    if (s.lastRunAt.time_since_epoch().count() != 0) j["last_run_at"] = TimeUtil::toIso8601(s.lastRunAt);
    if (!s.lastTestId.empty()) j["last_test_id"] = s.lastTestId;
    if (!s.lastError.empty()) j["last_error"] = s.lastError;
    j["run_count"] = static_cast<double>(s.runCount);
    j["skip_count"] = static_cast<double>(s.skipCount);
    if (includeNext && s.enabled) {
        CronExpr expr;
        if (CronExpr::parse(s.cron, expr)) {
            TimePoint next = expr.nextAfter(TimeUtil::now());
            if (next.time_since_epoch().count() != 0) j["next_run_at"] = TimeUtil::toIso8601(next);
        }
    }
    return j;
}

bool Scheduler::fromJson(const Json& json, Schedule& s, std::string& error) {
    if (!json.isObject()) { error = "schedule is not an object"; return false; }
    s.id = json["id"].asString("");
    if (!safeId(s.id)) { error = "missing or invalid id"; return false; }
    s.name = json["name"].asString(s.id);
    s.cron = json["cron"].asString("");
    CronExpr expr;
    if (!CronExpr::parse(s.cron, expr, &error)) return false;
    s.enabled = json["enabled"].asBool(true);
    s.skipIfRunning = json["skip_if_running"].asBool(true);
    if (json["request"].isObject()) {
        std::string reqError;
        if (!testRequestFromJson(json["request"], s.request, reqError)) {
            error = "invalid request: " + reqError;
            return false;
        }
    }
    s.createdAt = TimeUtil::fromIso8601(json["created_at"].asString(""));
    s.updatedAt = TimeUtil::fromIso8601(json["updated_at"].asString(""));
    s.lastRunAt = TimeUtil::fromIso8601(json["last_run_at"].asString(""));
    s.lastTestId = json["last_test_id"].asString("");
    s.lastError = json["last_error"].asString("");
    s.runCount = static_cast<unsigned long long>(json["run_count"].asNumber(0));
    s.skipCount = static_cast<unsigned long long>(json["skip_count"].asNumber(0));
    s.lastFiredMinute = static_cast<long long>(json["last_fired_minute"].asNumber(-1));
    return true;
}

bool Scheduler::loadAll() {
    items_.clear();
    order_.clear();
    if (config_.dir.empty() || !FileUtil::directoryExists(config_.dir)) return true;
    size_t skipped = 0;
    for (const auto& f : FileUtil::listFiles(config_.dir)) {
        if (FileUtil::getExtension(f) != ".json") continue;
        try {
            Json j = Json::parse(FileUtil::readFile(f));
            if (j["format"].asInt(0) > kFormatVersion) { ++skipped; continue; }
            Schedule s;
            std::string error;
            if (!fromJson(j, s, error)) {
                TH_LOG_WARN("schedule", "Skipping " + f + ": " + error);
                ++skipped;
                continue;
            }
            CronExpr expr;
            CronExpr::parse(s.cron, expr);
            Loaded item;
            item.schedule = std::move(s);
            item.expr = std::move(expr);
            std::string id = item.schedule.id;
            items_[id] = std::move(item);
            order_.push_back(id);
        } catch (const std::exception& e) {
            TH_LOG_WARN("schedule", "Skipping " + f + ": " + e.what());
            ++skipped;
        }
    }
    std::sort(order_.begin(), order_.end(), [&](const std::string& a, const std::string& b) {
        return items_[a].schedule.createdAt < items_[b].schedule.createdAt;
    });
    if (skipped) TH_LOG_WARN("schedule", "Skipped " + std::to_string(skipped) + " unreadable schedule file(s)");
    return true;
}

bool Scheduler::saveOne(const Loaded& item) {
    if (config_.dir.empty() || !safeId(item.schedule.id)) return false;
    Json j = toJson(item.schedule, false);
    j["format"] = kFormatVersion;
    j["last_fired_minute"] = static_cast<double>(item.schedule.lastFiredMinute);
    std::string finalPath = FileUtil::joinPath(config_.dir, item.schedule.id + ".json");
    std::string tmpPath = finalPath + ".tmp";
    if (!FileUtil::writeFile(tmpPath, j.dump(2))) return false;
    std::error_code ec;
    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        fs::remove(tmpPath, ec);
        return false;
    }
    return true;
}

bool Scheduler::deleteFile(const std::string& id) {
    if (config_.dir.empty() || !safeId(id)) return false;
    return FileUtil::deleteFile(FileUtil::joinPath(config_.dir, id + ".json"));
}

std::string Scheduler::generateId(TimePoint tp) {
    std::string stamp = TimeUtil::formatLocal(tp, "%Y%m%d-%H%M%S");
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "%03u", static_cast<unsigned>(++idCounter_ % 1000));
    std::string id = std::string("sched-") + stamp + "-" + suffix;
    while (items_.count(id)) {
        std::snprintf(suffix, sizeof(suffix), "%03u", static_cast<unsigned>(++idCounter_ % 1000));
        id = std::string("sched-") + stamp + "-" + suffix;
    }
    return id;
}

std::string Scheduler::fireLocked(Loaded& item, TimePoint now, bool force, std::string& error) {
    (void)force;
    Schedule& s = item.schedule;
    if (s.skipIfRunning && !s.lastTestId.empty() && isActive_ && isActive_(s.lastTestId)) {
        ++s.skipCount;
        ++skips_;
        s.lastError.clear();
        error = "previous test still running";
        publishEvent(EventType::SCHEDULE_SKIPPED, s.lastTestId,
                     {{"schedule_id", s.id}, {"name", s.name}, {"reason", "previous_still_running"}});
        return "";
    }
    if (!submit_) {
        error = "scheduler has no submit callback";
        s.lastError = error;
        ++errors_;
        return "";
    }
    TestRequest req = s.request;
    req.id.clear();
    req.submittedBy = "schedule:" + s.id;
    req.metadata["schedule_id"] = s.id;
    if (req.name.empty()) req.name = s.name;
    try {
        std::string testId = submit_(req);
        s.lastTestId = testId;
        s.lastRunAt = now;
        s.lastError.clear();
        ++s.runCount;
        ++fires_;
        publishEvent(EventType::SCHEDULE_TRIGGERED, testId,
                     {{"schedule_id", s.id}, {"name", s.name}, {"cron", s.cron}});
        return testId;
    } catch (const std::exception& e) {
        error = e.what();
        s.lastError = error;
        s.lastRunAt = now;
        ++errors_;
        publishEvent(EventType::SCHEDULE_ERROR, "",
                     {{"schedule_id", s.id}, {"name", s.name}, {"error", error}});
        return "";
    }
}

} // namespace testhub
