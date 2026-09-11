#include "test_framework.h"
#include "engine/scheduler.h"
#include "util/cron.h"
#include "util/file_util.h"
#include "util/time_util.h"

#include "compat.h"
#include <chrono>
#include <filesystem>

using namespace testhub;
namespace fs = std::filesystem;

namespace {

TimePoint utc(const char* iso) { return TimeUtil::fromIso8601(iso); }

bool matches(const std::string& expr, const char* iso) {
    CronExpr c;
    std::string err;
    REQUIRE(CronExpr::parse(expr, c, &err));
    return c.matches(utc(iso));
}

std::string tempDir() {
    auto p = fs::temp_directory_path() / ("testhub-sched-" + std::to_string(testhubGetPid()) + "-" +
                                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p.string();
}

} // namespace

TEST_CASE("cron: parse rejects empty, wrong arity and out of range") {
    CronExpr c;
    std::string err;
    CHECK(!CronExpr::parse("", c, &err));
    CHECK(!CronExpr::parse("* * *", c, &err));
    CHECK(!CronExpr::parse("60 * * * *", c, &err));
    CHECK(!CronExpr::parse("* 24 * * *", c, &err));
    CHECK(!CronExpr::parse("* * 32 * *", c, &err));
    CHECK(!CronExpr::parse("* * * 13 *", c, &err));
    CHECK(!CronExpr::parse("* * * * 8", c, &err));
    CHECK(!CronExpr::parse("@never", c, &err));
}

TEST_CASE("cron: wildcards, lists, ranges, steps and names") {
    CHECK(matches("* * * * *", "2026-09-11T12:34:00.000Z"));
    CHECK(matches("0 2 * * *", "2026-09-11T02:00:00.000Z"));
    CHECK(!matches("0 2 * * *", "2026-09-11T02:01:00.000Z"));
    CHECK(!matches("0 2 * * *", "2026-09-11T03:00:00.000Z"));
    CHECK(matches("0,30 * * * *", "2026-09-11T10:30:00.000Z"));
    CHECK(!matches("0,30 * * * *", "2026-09-11T10:15:00.000Z"));
    CHECK(matches("*/15 * * * *", "2026-09-11T10:00:00.000Z"));
    CHECK(matches("*/15 * * * *", "2026-09-11T10:45:00.000Z"));
    CHECK(!matches("*/15 * * * *", "2026-09-11T10:07:00.000Z"));
    CHECK(matches("0 0 1 JAN *", "2026-01-01T00:00:00.000Z"));
    CHECK(!matches("0 0 1 JAN *", "2026-02-01T00:00:00.000Z"));
    CHECK(matches("0 0 * * FRI", "2026-09-11T00:00:00.000Z"));  // 2026-09-11 is Friday
    CHECK(!matches("0 0 * * FRI", "2026-09-12T00:00:00.000Z"));
    CHECK(matches("0 0 * * 5", "2026-09-11T00:00:00.000Z"));
    CHECK(matches("0 0 * * 0", "2026-09-13T00:00:00.000Z"));  // Sunday
    CHECK(matches("0 0 * * 7", "2026-09-13T00:00:00.000Z"));
}

TEST_CASE("cron: aliases and DOM/DOW either-or") {
    CHECK(matches("@hourly", "2026-09-11T13:00:00.000Z"));
    CHECK(!matches("@hourly", "2026-09-11T13:01:00.000Z"));
    CHECK(matches("@daily", "2026-09-11T00:00:00.000Z"));
    CHECK(!matches("@daily", "2026-09-11T00:01:00.000Z"));
    CHECK(matches("@monthly", "2026-03-01T00:00:00.000Z"));
    CHECK(!matches("@monthly", "2026-03-02T00:00:00.000Z"));
    // 日=11 或 周五：9/11 周五且日=11 命中；9/12 周六日=12 不命中；9/4 周五命中
    CHECK(matches("0 0 11 * FRI", "2026-09-11T00:00:00.000Z"));
    CHECK(matches("0 0 11 * FRI", "2026-09-04T00:00:00.000Z"));
    CHECK(!matches("0 0 11 * FRI", "2026-09-12T00:00:00.000Z"));
}

TEST_CASE("cron: nextAfter skips current minute") {
    CronExpr c;
    REQUIRE(CronExpr::parse("0 * * * *", c));
    TimePoint next = c.nextAfter(utc("2026-09-11T10:00:00.000Z"));
    CHECK_EQ(TimeUtil::toIso8601(next), std::string("2026-09-11T11:00:00.000Z"));
    CronExpr every;
    REQUIRE(CronExpr::parse("* * * * *", every));
    TimePoint n2 = every.nextAfter(utc("2026-09-11T10:00:30.000Z"));
    CHECK_EQ(TimeUtil::toIso8601(n2), std::string("2026-09-11T10:01:00.000Z"));
}

TEST_CASE("scheduler: tick fires once per matching minute and persists") {
    std::string dir = tempDir();
    std::vector<std::string> ids;
    Scheduler sch;
    SchedulerConfig cfg;
    cfg.enabled = false;
    cfg.dir = dir;
    sch.configure(cfg);
    sch.setSubmit([&](TestRequest r) {
        CHECK_EQ(r.metadata["schedule_id"].empty(), false);
        ids.push_back(r.name);
        return "test-" + std::to_string(ids.size());
    });
    sch.setIsActive([](const std::string&) { return false; });

    Schedule in;
    in.name = "minutely";
    in.cron = "* * * * *";
    in.request.name = "from-plan";
    in.request.specFiles = {"login.spec"};
    std::string err;
    Schedule created = sch.create(in, err);
    REQUIRE(err.empty());
    REQUIRE(!created.id.empty());

    TimePoint t0 = utc("2026-03-01T00:00:00.000Z");
    CHECK_EQ(sch.tick(t0), 1);
    CHECK_EQ(sch.tick(t0 + std::chrono::seconds(30)), 0);
    CHECK_EQ(sch.tick(t0 + std::chrono::minutes(1)), 1);
    CHECK_EQ(ids.size(), static_cast<size_t>(2));

    Scheduler sch2;
    sch2.configure(cfg);
    sch2.start();  // enabled=false：只加载不启动线程
    auto list = sch2.list();
    REQUIRE_EQ(list.size(), static_cast<size_t>(1));
    CHECK_EQ(list[0].runCount, static_cast<unsigned long long>(2));
    CHECK(!list[0].lastTestId.empty());
    sch2.stop();
    fs::remove_all(dir);
}

TEST_CASE("scheduler: skip_if_running and invalid cron") {
    Scheduler sch;
    SchedulerConfig cfg;
    cfg.enabled = false;
    cfg.dir.clear();
    sch.configure(cfg);
    int submits = 0;
    sch.setSubmit([&](TestRequest) { ++submits; return "test-busy"; });
    sch.setIsActive([&](const std::string& id) { return id == "test-busy"; });

    Schedule in;
    in.cron = "* * * * *";
    in.skipIfRunning = true;
    std::string err;
    Schedule created = sch.create(in, err);
    REQUIRE(err.empty());

    TimePoint t0 = utc("2026-04-01T00:00:00.000Z");
    CHECK_EQ(sch.tick(t0), 1);
    CHECK_EQ(submits, 1);
    CHECK_EQ(sch.tick(t0 + std::chrono::minutes(1)), 1);  // tick 仍计入，但 skip
    CHECK_EQ(submits, 1);
    auto got = sch.get(created.id);
    REQUIRE(got.has_value());
    CHECK_EQ(got->skipCount, static_cast<unsigned long long>(1));
    CHECK_EQ(got->runCount, static_cast<unsigned long long>(1));

    Schedule bad;
    bad.cron = "not a cron";
    std::string e2;
    Schedule fail = sch.create(bad, e2);
    CHECK(fail.id.empty());
    CHECK(!e2.empty());
}

TEST_CASE("scheduler: fireNow and disable") {
    Scheduler sch;
    SchedulerConfig cfg;
    cfg.enabled = false;
    cfg.dir.clear();
    sch.configure(cfg);
    int submits = 0;
    sch.setSubmit([&](TestRequest) { return "test-" + std::to_string(++submits); });
    sch.setIsActive([](const std::string&) { return false; });

    Schedule in;
    in.cron = "0 0 1 1 *";  // 每年一次，tick 不会在任意时刻触发
    in.enabled = false;
    std::string err;
    Schedule created = sch.create(in, err);
    REQUIRE(err.empty());
    CHECK_EQ(sch.tick(utc("2026-06-01T12:00:00.000Z")), 0);

    std::string fireErr;
    std::string id = sch.fireNow(created.id, fireErr);
    CHECK(fireErr.empty());
    CHECK_EQ(id, std::string("test-1"));

    Schedule patch = created;
    patch.enabled = true;
    REQUIRE(sch.update(created.id, patch, false, err));
    CHECK(sch.remove(created.id));
    CHECK(!sch.get(created.id).has_value());
}

TEST_CASE("scheduler: enable-only update does not reset lastFiredMinute") {
    Scheduler sch;
    SchedulerConfig cfg;
    cfg.enabled = false;
    cfg.dir.clear();
    sch.configure(cfg);
    int submits = 0;
    sch.setSubmit([&](TestRequest) { return "test-" + std::to_string(++submits); });
    sch.setIsActive([](const std::string&) { return false; });

    Schedule in;
    in.cron = "* * * * *";
    std::string err;
    Schedule created = sch.create(in, err);
    REQUIRE(err.empty());

    TimePoint t0 = utc("2026-05-01T00:00:00.000Z");
    CHECK_EQ(sch.tick(t0), 1);
    CHECK_EQ(submits, 1);

    Schedule patch = *sch.get(created.id);
    patch.enabled = true;
    REQUIRE(sch.update(created.id, patch, false, err));
    CHECK_EQ(sch.tick(t0), 0);
    CHECK_EQ(submits, 1);

    patch.cron = "*/5 * * * *";
    REQUIRE(sch.update(created.id, patch, false, err));
    CHECK_EQ(sch.tick(t0), 1);
    CHECK_EQ(submits, 2);
}
