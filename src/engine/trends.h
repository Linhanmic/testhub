/*
 * TestHub - 结果趋势与对比（纯函数）
 * 按规范聚合历次通过率 / 耗时；对比两次结果的场景级回归与改善。
 */

#pragma once

#include "../model/types.h"
#include "../util/json.h"
#include "../util/time_util.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace testhub {

inline double passRateOf(int passed, int total) {
    return total > 0 ? static_cast<double>(passed) / static_cast<double>(total) : 0.0;
}

struct TrendRun {
    std::string testId;
    std::string name;
    TestState state = TestState::PASSED;
    TimePoint endTime;
    double duration = 0.0;
    int totalScenarios = 0;
    int passedScenarios = 0;
    int failedScenarios = 0;
    int skippedScenarios = 0;
    std::vector<std::string> specFiles;
};

struct SpecTrendSeries {
    std::string specFile;
    std::vector<TrendRun> points;  // 时间升序，已按 maxPoints 截取最近若干次
};

inline Json trendRunToJson(const TrendRun& r) {
    Json j = Json::object();
    j["test_id"] = r.testId;
    j["name"] = r.name;
    j["state"] = testStateToString(r.state);
    j["end_time"] = TimeUtil::toIso8601(r.endTime);
    j["duration"] = r.duration;
    j["total_scenarios"] = r.totalScenarios;
    j["passed_scenarios"] = r.passedScenarios;
    j["failed_scenarios"] = r.failedScenarios;
    j["skipped_scenarios"] = r.skippedScenarios;
    j["pass_rate"] = passRateOf(r.passedScenarios, r.totalScenarios);
    return j;
}

inline Json specTrendToJson(const SpecTrendSeries& s) {
    Json pts = Json::array();
    double durSum = 0;
    for (const auto& p : s.points) {
        pts.push(trendRunToJson(p));
        durSum += p.duration;
    }
    Json j = Json::object();
    j["spec"] = s.specFile;
    j["runs"] = static_cast<int>(s.points.size());
    j["latest_pass_rate"] = s.points.empty() ? 0.0 : passRateOf(s.points.back().passedScenarios, s.points.back().totalScenarios);
    j["avg_duration"] = s.points.empty() ? 0.0 : durSum / static_cast<double>(s.points.size());
    j["points"] = pts;
    return j;
}

/** 按规范分组；filter 为空则全部。overall 的 specFile 为 "(all)"。points 为时间升序的最近 maxPoints 次。 */
inline std::vector<SpecTrendSeries> groupTrends(const std::vector<TrendRun>& chronological,
                                                const std::string& specFilter, size_t maxPoints) {
    if (maxPoints < 1) maxPoints = 1;
    std::map<std::string, std::vector<TrendRun>> bySpec;
    std::vector<TrendRun> overall;
    overall.reserve(chronological.size());
    for (const auto& run : chronological) {
        overall.push_back(run);
        std::set<std::string> seen;
        for (const auto& spec : run.specFiles) {
            if (!seen.insert(spec).second) continue;
            if (specFilter.empty() || spec == specFilter) bySpec[spec].push_back(run);
        }
    }
    auto trim = [&](std::vector<TrendRun>& v) {
        if (v.size() > maxPoints) v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() - maxPoints));
    };
    std::vector<SpecTrendSeries> out;
    if (specFilter.empty() && !overall.empty()) {
        trim(overall);
        SpecTrendSeries all;
        all.specFile = "(all)";
        all.points = std::move(overall);
        out.push_back(std::move(all));
    }
    for (auto& kv : bySpec) {
        trim(kv.second);
        SpecTrendSeries s;
        s.specFile = kv.first;
        s.points = std::move(kv.second);
        out.push_back(std::move(s));
    }
    return out;
}

inline std::string scenarioKey(const std::string& spec, const std::string& name, int dataRowIndex) {
    return spec + "\n" + name + "\n" + std::to_string(dataRowIndex);
}

struct ScenarioSnap {
    std::string specFile;
    std::string scenarioName;
    int dataRowIndex = -1;
    TestState state = TestState::PASSED;
    double duration = 0.0;
};

inline std::vector<ScenarioSnap> flattenScenarios(const TestResult& r) {
    std::vector<ScenarioSnap> out;
    for (const auto& spec : r.specResults) {
        for (const auto& sc : spec.scenarioResults) {
            ScenarioSnap s;
            s.specFile = spec.specFile;
            s.scenarioName = sc.scenarioName;
            s.dataRowIndex = sc.dataRowIndex;
            s.state = sc.state;
            s.duration = sc.duration;
            out.push_back(std::move(s));
        }
    }
    return out;
}

inline bool scenarioFailed(TestState s) {
    return s == TestState::FAILED || s == TestState::TEST_ERROR;
}

struct ScenarioDiff {
    std::string specFile;
    std::string scenarioName;
    int dataRowIndex = -1;
    std::string change;  // regressed | improved | still_failed | unchanged | added | removed
    TestState baseline = TestState::SKIPPED;
    TestState current = TestState::SKIPPED;
    double durationBaseline = 0.0;
    double durationCurrent = 0.0;
};

inline std::vector<ScenarioDiff> compareScenarios(const TestResult& baseline, const TestResult& current) {
    std::map<std::string, ScenarioSnap> left, right;
    for (const auto& s : flattenScenarios(baseline)) left[scenarioKey(s.specFile, s.scenarioName, s.dataRowIndex)] = s;
    for (const auto& s : flattenScenarios(current)) right[scenarioKey(s.specFile, s.scenarioName, s.dataRowIndex)] = s;
    std::vector<std::string> keys;
    keys.reserve(left.size() + right.size());
    for (const auto& kv : left) keys.push_back(kv.first);
    for (const auto& kv : right) {
        if (!left.count(kv.first)) keys.push_back(kv.first);
    }
    std::vector<ScenarioDiff> out;
    out.reserve(keys.size());
    for (const auto& k : keys) {
        ScenarioDiff d;
        auto lit = left.find(k);
        auto rit = right.find(k);
        if (lit != left.end()) {
            d.specFile = lit->second.specFile;
            d.scenarioName = lit->second.scenarioName;
            d.dataRowIndex = lit->second.dataRowIndex;
            d.baseline = lit->second.state;
            d.durationBaseline = lit->second.duration;
        }
        if (rit != right.end()) {
            d.specFile = rit->second.specFile;
            d.scenarioName = rit->second.scenarioName;
            d.dataRowIndex = rit->second.dataRowIndex;
            d.current = rit->second.state;
            d.durationCurrent = rit->second.duration;
        }
        if (lit == left.end()) d.change = "added";
        else if (rit == right.end()) d.change = "removed";
        else if (scenarioFailed(d.baseline) && !scenarioFailed(d.current)) d.change = "improved";
        else if (!scenarioFailed(d.baseline) && scenarioFailed(d.current)) d.change = "regressed";
        else if (scenarioFailed(d.baseline) && scenarioFailed(d.current)) d.change = "still_failed";
        else d.change = "unchanged";
        out.push_back(std::move(d));
    }
    return out;
}

inline Json scenarioDiffToJson(const ScenarioDiff& d) {
    Json j = Json::object();
    j["spec"] = d.specFile;
    j["scenario"] = d.scenarioName;
    j["data_row_index"] = d.dataRowIndex;
    j["change"] = d.change;
    j["baseline_state"] = testStateToString(d.baseline);
    j["current_state"] = testStateToString(d.current);
    j["baseline_duration"] = d.durationBaseline;
    j["current_duration"] = d.durationCurrent;
    j["duration_delta"] = d.durationCurrent - d.durationBaseline;
    return j;
}

struct ComparisonSummary {
    int regressed = 0;
    int improved = 0;
    int stillFailed = 0;
    int unchanged = 0;
    int added = 0;
    int removed = 0;
};

inline ComparisonSummary summarizeDiffs(const std::vector<ScenarioDiff>& diffs) {
    ComparisonSummary s;
    for (const auto& d : diffs) {
        if (d.change == "regressed") ++s.regressed;
        else if (d.change == "improved") ++s.improved;
        else if (d.change == "still_failed") ++s.stillFailed;
        else if (d.change == "added") ++s.added;
        else if (d.change == "removed") ++s.removed;
        else ++s.unchanged;
    }
    return s;
}

inline Json summaryToJson(const ComparisonSummary& s) {
    Json j = Json::object();
    j["regressed"] = s.regressed;
    j["improved"] = s.improved;
    j["still_failed"] = s.stillFailed;
    j["unchanged"] = s.unchanged;
    j["added"] = s.added;
    j["removed"] = s.removed;
    return j;
}

struct TestComparison {
    std::string currentId;
    std::string baselineId;
    std::string currentName;
    std::string baselineName;
    TestState currentState = TestState::PASSED;
    TestState baselineState = TestState::PASSED;
    double currentDuration = 0.0;
    double baselineDuration = 0.0;
    TimePoint currentEnd;
    TimePoint baselineEnd;
    bool autoBaseline = false;
    std::vector<ScenarioDiff> diffs;
};

inline Json comparisonToJson(const TestComparison& c) {
    Json j = Json::object();
    auto side = [](const std::string& id, const std::string& name, TestState state, double duration, TimePoint end) {
        Json s = Json::object();
        s["test_id"] = id;
        s["name"] = name;
        s["state"] = testStateToString(state);
        s["duration"] = duration;
        s["end_time"] = TimeUtil::toIso8601(end);
        return s;
    };
    j["current"] = side(c.currentId, c.currentName, c.currentState, c.currentDuration, c.currentEnd);
    j["baseline"] = side(c.baselineId, c.baselineName, c.baselineState, c.baselineDuration, c.baselineEnd);
    j["baseline_auto"] = c.autoBaseline;
    j["summary"] = summaryToJson(summarizeDiffs(c.diffs));
    Json arr = Json::array();
    for (const auto& d : c.diffs) arr.push(scenarioDiffToJson(d));
    j["scenarios"] = arr;
    j["count"] = static_cast<int>(c.diffs.size());
    return j;
}

} // namespace testhub
