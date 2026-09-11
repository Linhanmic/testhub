#include "test_framework.h"
#include "engine/trends.h"

#include <vector>

using namespace testhub;

namespace {

TrendRun run(const std::string& id, const std::string& spec, int passed, int total, double duration, int failed = 0) {
    TrendRun r;
    r.testId = id;
    r.name = id;
    r.state = failed ? TestState::FAILED : TestState::PASSED;
    r.duration = duration;
    r.totalScenarios = total;
    r.passedScenarios = passed;
    r.failedScenarios = failed;
    r.skippedScenarios = total - passed - failed;
    if (!spec.empty()) r.specFiles = {spec};
    return r;
}

ScenarioResult sc(const std::string& name, TestState state, int row = -1, double duration = 0.1) {
    ScenarioResult s;
    s.scenarioName = name;
    s.state = state;
    s.dataRowIndex = row;
    s.duration = duration;
    return s;
}

TestResult resultOf(const std::string& spec, std::vector<ScenarioResult> scenarios) {
    SpecResult sp;
    sp.specFile = spec;
    sp.scenarioResults = std::move(scenarios);
    TestResult r;
    r.specResults.push_back(std::move(sp));
    return r;
}

} // namespace

TEST_CASE("trends: groupTrends filters, trims and emits (all)") {
    CHECK(groupTrends({}, "", 10).empty());

    std::vector<TrendRun> runs = {
        run("a", "login.spec", 3, 3, 1.0),
        run("b", "calc.spec", 2, 2, 2.0),
        run("c", "login.spec", 2, 3, 1.5, 1),
        run("d", "login.spec", 3, 3, 0.8),
        run("e", "login.spec", 3, 3, 0.9),
    };
    auto all = groupTrends(runs, "", 50);
    REQUIRE_EQ(all.size(), static_cast<size_t>(3));
    CHECK_EQ(all[0].specFile, std::string("(all)"));
    CHECK_EQ(all[0].points.size(), static_cast<size_t>(5));
    CHECK_EQ(all[1].specFile, std::string("calc.spec"));
    CHECK_EQ(all[2].specFile, std::string("login.spec"));
    CHECK_EQ(all[2].points.size(), static_cast<size_t>(4));

    auto login = groupTrends(runs, "login.spec", 3);
    REQUIRE_EQ(login.size(), static_cast<size_t>(1));
    CHECK_EQ(login[0].specFile, std::string("login.spec"));
    REQUIRE_EQ(login[0].points.size(), static_cast<size_t>(3));
    CHECK_EQ(login[0].points[0].testId, std::string("c"));
    CHECK_EQ(login[0].points[2].testId, std::string("e"));
    Json j = specTrendToJson(login[0]);
    CHECK_EQ(j["runs"].asInt(), 3);
    CHECK(j["latest_pass_rate"].asNumber() > 0.99);
    CHECK(j["avg_duration"].asNumber() > 0.0);
    CHECK_EQ(j["points"].size(), static_cast<size_t>(3));

    auto missing = groupTrends(runs, "nope.spec", 10);
    CHECK(missing.empty());

    TrendRun dup = run("x", "login.spec", 1, 1, 1.0);
    dup.specFiles = {"login.spec", "login.spec"};
    auto dedup = groupTrends({dup}, "", 10);
    REQUIRE_EQ(dedup.size(), static_cast<size_t>(2));  // (all) + login.spec
    CHECK_EQ(dedup[1].points.size(), static_cast<size_t>(1));
}

TEST_CASE("trends: compareScenarios regression, improvement and still_failed") {
    TestResult base = resultOf("a.spec", {
        sc("ok", TestState::PASSED),
        sc("flake", TestState::FAILED),
        sc("always", TestState::FAILED),
    });
    TestResult cur = resultOf("a.spec", {
        sc("ok", TestState::PASSED),
        sc("flake", TestState::PASSED),
        sc("always", TestState::TEST_ERROR),
    });
    auto diffs = compareScenarios(base, cur);
    auto sum = summarizeDiffs(diffs);
    CHECK_EQ(sum.unchanged, 1);
    CHECK_EQ(sum.improved, 1);
    CHECK_EQ(sum.stillFailed, 1);
    CHECK_EQ(sum.regressed, 0);

    TestResult worse = resultOf("a.spec", {
        sc("ok", TestState::FAILED),
        sc("flake", TestState::FAILED),
        sc("always", TestState::FAILED),
    });
    auto down = summarizeDiffs(compareScenarios(base, worse));
    CHECK_EQ(down.regressed, 1);
    CHECK_EQ(down.stillFailed, 2);
}

TEST_CASE("trends: compareScenarios data rows, added and removed") {
    TestResult base = resultOf("t.spec", {
        sc("row", TestState::PASSED, 0),
        sc("row", TestState::FAILED, 1),
        sc("gone", TestState::PASSED),
    });
    TestResult cur = resultOf("t.spec", {
        sc("row", TestState::PASSED, 0),
        sc("row", TestState::PASSED, 1),
        sc("new", TestState::PASSED),
    });
    auto diffs = compareScenarios(base, cur);
    auto sum = summarizeDiffs(diffs);
    CHECK_EQ(sum.improved, 1);
    CHECK_EQ(sum.unchanged, 1);
    CHECK_EQ(sum.added, 1);
    CHECK_EQ(sum.removed, 1);
    bool sawRow1 = false;
    for (const auto& d : diffs) {
        if (d.scenarioName == "row" && d.dataRowIndex == 1) {
            CHECK_EQ(d.change, std::string("improved"));
            sawRow1 = true;
        }
    }
    CHECK(sawRow1);

    TestComparison c;
    c.currentId = "cur";
    c.baselineId = "base";
    c.currentName = "now";
    c.baselineName = "then";
    c.autoBaseline = true;
    c.diffs = diffs;
    Json j = comparisonToJson(c);
    CHECK(j["baseline_auto"].asBool());
    CHECK_EQ(j["summary"]["added"].asInt(), 1);
    CHECK_EQ(j["summary"]["removed"].asInt(), 1);
    CHECK_EQ(j["count"].asInt(), 4);
    CHECK_EQ(j["scenarios"][0]["spec"].asString(), std::string("t.spec"));
}
