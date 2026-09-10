/*
 * TestHub - 数据模型 <-> JSON 转换
 */

#pragma once

#include "types.h"
#include "../util/json.h"
#include "../util/time_util.h"
#include "../spec/spec.h"

namespace testhub {

inline Json toJson(const std::vector<std::string>& v) {
    Json arr = Json::array();
    for (const auto& s : v) arr.push(s);
    return arr;
}

inline Json toJson(const std::map<std::string, std::string>& m) {
    Json obj = Json::object();
    for (const auto& kv : m) obj[kv.first] = kv.second;
    return obj;
}

inline Json toJson(const spec::Table& table) {
    Json obj = Json::object();
    obj["headers"] = toJson(table.headers);
    Json rows = Json::array();
    for (const auto& r : table.rows) rows.push(toJson(r));
    obj["rows"] = rows;
    return obj;
}

inline Json toJson(const spec::StepArg& arg) {
    Json obj = Json::object();
    obj["type"] = spec::argTypeToString(arg.type);
    if (arg.type == spec::ArgType::Table) obj["table"] = toJson(arg.table);
    else obj["value"] = arg.value;
    return obj;
}

inline Json toJson(const spec::Step& step) {
    Json obj = Json::object();
    obj["text"] = step.text;
    obj["parameterized_text"] = step.parameterizedText;
    obj["line_number"] = step.lineNumber;
    Json args = Json::array();
    for (const auto& a : step.args) args.push(toJson(a));
    obj["args"] = args;
    if (step.isConcept) {
        obj["is_concept"] = true;
        Json inner = Json::array();
        for (const auto& s : step.conceptSteps) inner.push(toJson(s));
        obj["concept_steps"] = inner;
    }
    return obj;
}

inline Json toJson(const spec::Scenario& scenario) {
    Json obj = Json::object();
    obj["name"] = scenario.name;
    obj["tags"] = toJson(scenario.tags);
    obj["line_number"] = scenario.lineNumber;
    obj["span"] = scenario.span;
    Json steps = Json::array();
    for (const auto& s : scenario.steps) steps.push(toJson(s));
    obj["steps"] = steps;
    return obj;
}

inline Json toJson(const spec::Specification& s) {
    Json obj = Json::object();
    obj["file"] = s.fileName;
    obj["heading"] = s.heading;
    obj["tags"] = toJson(s.tags);
    obj["heading_line"] = s.headingLine;
    obj["is_data_driven"] = s.isDataDriven();
    if (!s.dataTable.empty()) obj["data_table"] = toJson(s.dataTable);
    Json ctx = Json::array();
    for (const auto& st : s.contexts) ctx.push(toJson(st));
    obj["contexts"] = ctx;
    Json td = Json::array();
    for (const auto& st : s.teardowns) td.push(toJson(st));
    obj["teardowns"] = td;
    Json scenarios = Json::array();
    for (const auto& sc : s.scenarios) scenarios.push(toJson(sc));
    obj["scenarios"] = scenarios;
    obj["scenario_count"] = static_cast<int>(s.scenarios.size());
    return obj;
}

inline Json toJson(const spec::ParseError& e) {
    Json obj = Json::object();
    obj["file"] = e.fileName;
    obj["line"] = e.lineNumber;
    obj["message"] = e.message;
    if (!e.lineText.empty()) obj["text"] = e.lineText;
    return obj;
}

inline Json toJson(const StepResult& r) {
    Json obj = Json::object();
    obj["step"] = r.stepText;
    obj["parameterized_text"] = r.parameterizedText;
    obj["state"] = testStateToString(r.state);
    obj["duration"] = r.duration;
    if (!r.errorMessage.empty()) obj["error"] = r.errorMessage;
    if (!r.stackTrace.empty()) obj["stack_trace"] = r.stackTrace;
    if (!r.messages.empty()) obj["messages"] = toJson(r.messages);
    if (r.isConcept) {
        obj["is_concept"] = true;
        Json inner = Json::array();
        for (const auto& s : r.conceptSteps) inner.push(toJson(s));
        obj["concept_steps"] = inner;
    }
    return obj;
}

inline Json toJson(const ScenarioResult& r) {
    Json obj = Json::object();
    obj["name"] = r.scenarioName;
    obj["tags"] = toJson(r.tags);
    obj["state"] = testStateToString(r.state);
    obj["duration"] = r.duration;
    obj["line_number"] = r.lineNumber;
    if (!r.errorMessage.empty()) obj["error"] = r.errorMessage;
    if (r.dataRowIndex >= 0) {
        obj["data_row_index"] = r.dataRowIndex;
        obj["data_row"] = toJson(r.dataRow);
    }
    Json ctx = Json::array();
    for (const auto& s : r.contextSteps) ctx.push(toJson(s));
    obj["context_steps"] = ctx;
    Json steps = Json::array();
    for (const auto& s : r.stepResults) steps.push(toJson(s));
    obj["steps"] = steps;
    Json td = Json::array();
    for (const auto& s : r.teardownSteps) td.push(toJson(s));
    obj["teardown_steps"] = td;
    return obj;
}

inline Json toJson(const SpecResult& r) {
    Json obj = Json::object();
    obj["file"] = r.specFile;
    obj["name"] = r.specName;
    obj["tags"] = toJson(r.tags);
    obj["state"] = testStateToString(r.state);
    obj["duration"] = r.duration;
    if (!r.errorMessage.empty()) obj["error"] = r.errorMessage;
    obj["total_scenarios"] = r.totalScenarios;
    obj["passed_scenarios"] = r.passedScenarios;
    obj["failed_scenarios"] = r.failedScenarios;
    obj["skipped_scenarios"] = r.skippedScenarios;
    Json scenarios = Json::array();
    for (const auto& s : r.scenarioResults) scenarios.push(toJson(s));
    obj["scenarios"] = scenarios;
    return obj;
}

inline Json toJson(const TestRequest& r) {
    Json obj = Json::object();
    obj["id"] = r.id;
    obj["name"] = r.name;
    obj["spec_files"] = toJson(r.specFiles);
    obj["tags"] = toJson(r.tags);
    obj["scenarios"] = toJson(r.scenarios);
    obj["environment"] = r.environment;
    obj["parallel_streams"] = r.parallelStreams;
    obj["priority"] = priorityToString(r.priority);
    obj["timeout_ms"] = r.timeoutMs;
    obj["fail_fast"] = r.failFast;
    obj["metadata"] = toJson(r.metadata);
    if (!r.callbackUrl.empty()) obj["callback_url"] = r.callbackUrl;
    if (!r.submittedBy.empty()) obj["submitted_by"] = r.submittedBy;
    return obj;
}

inline Json toJson(const TestStatus& s) {
    Json obj = Json::object();
    obj["test_id"] = s.testId;
    obj["state"] = testStateToString(s.state);
    obj["progress"] = s.progress;
    obj["total_specs"] = s.totalSpecs;
    obj["executed_specs"] = s.executedSpecs;
    obj["passed_specs"] = s.passedSpecs;
    obj["failed_specs"] = s.failedSpecs;
    obj["total_scenarios"] = s.totalScenarios;
    obj["executed_scenarios"] = s.executedScenarios;
    obj["passed_scenarios"] = s.passedScenarios;
    obj["failed_scenarios"] = s.failedScenarios;
    obj["skipped_scenarios"] = s.skippedScenarios;
    obj["current_spec"] = s.currentSpec;
    obj["current_scenario"] = s.currentScenario;
    obj["current_step"] = s.currentStep;
    obj["submit_time"] = TimeUtil::toIso8601(s.submitTime);
    obj["start_time"] = TimeUtil::toIso8601(s.startTime);
    obj["end_time"] = TimeUtil::toIso8601(s.endTime);
    if (!s.errors.empty()) obj["errors"] = toJson(s.errors);
    if (!s.warnings.empty()) obj["warnings"] = toJson(s.warnings);
    return obj;
}

inline Json toJson(const TestResult& r) {
    Json obj = Json::object();
    obj["test_id"] = r.testId;
    obj["state"] = testStateToString(r.finalState);
    obj["duration"] = r.totalDuration;
    obj["start_time"] = TimeUtil::toIso8601(r.startTime);
    obj["end_time"] = TimeUtil::toIso8601(r.endTime);
    obj["total_scenarios"] = r.totalScenarios;
    obj["passed_scenarios"] = r.passedScenarios;
    obj["failed_scenarios"] = r.failedScenarios;
    obj["skipped_scenarios"] = r.skippedScenarios;
    obj["errors"] = toJson(r.errors);
    obj["warnings"] = toJson(r.warnings);
    Json specs = Json::array();
    for (const auto& s : r.specResults) specs.push(toJson(s));
    obj["specs"] = specs;
    return obj;
}

inline Json toJson(const TestInfo& i) {
    Json obj = Json::object();
    obj["test_id"] = i.testId;
    obj["name"] = i.name;
    obj["state"] = testStateToString(i.state);
    obj["spec_files"] = toJson(i.specFiles);
    obj["tags"] = toJson(i.tags);
    obj["priority"] = priorityToString(i.priority);
    obj["progress"] = i.progress;
    obj["submit_time"] = TimeUtil::toIso8601(i.submitTime);
    obj["start_time"] = TimeUtil::toIso8601(i.startTime);
    obj["end_time"] = TimeUtil::toIso8601(i.endTime);
    obj["total_scenarios"] = i.totalScenarios;
    obj["passed_scenarios"] = i.passedScenarios;
    obj["failed_scenarios"] = i.failedScenarios;
    obj["skipped_scenarios"] = i.skippedScenarios;
    obj["duration"] = i.duration;
    return obj;
}

inline Json toJson(const RunnerStatus& s) {
    Json obj = Json::object();
    obj["state"] = runnerStateToString(s.state);
    obj["language"] = s.language;
    obj["command"] = s.command;
    obj["pid"] = s.pid;
    obj["version"] = s.version;
    obj["implemented_steps"] = toJson(s.implementedSteps);
    obj["step_count"] = static_cast<int>(s.implementedSteps.size());
    obj["last_heartbeat"] = TimeUtil::toIso8601(s.lastHeartbeat);
    obj["started_at"] = TimeUtil::toIso8601(s.startedAt);
    obj["restart_count"] = s.restartCount;
    if (!s.lastError.empty()) obj["last_error"] = s.lastError;
    obj["pool_size"] = s.poolSize;
    obj["alive"] = s.aliveCount;
    obj["busy"] = s.busyCount;
    Json slots = Json::array();
    for (const auto& slot : s.slots) {
        Json o = Json::object();
        o["index"] = slot.index;
        o["state"] = runnerStateToString(slot.state);
        o["pid"] = slot.pid;
        o["version"] = slot.version;
        o["restart_count"] = slot.restartCount;
        o["steps_executed"] = static_cast<double>(slot.stepsExecuted);
        o["busy"] = slot.busy;
        if (!slot.lastError.empty()) o["last_error"] = slot.lastError;
        o["started_at"] = TimeUtil::toIso8601(slot.startedAt);
        o["last_heartbeat"] = TimeUtil::toIso8601(slot.lastHeartbeat);
        slots.push(o);
    }
    obj["runners"] = slots;
    return obj;
}

inline Json toJson(const Event& e) {
    Json obj = Json::object();
    obj["event"] = e.type;
    obj["test_id"] = e.testId;
    obj["timestamp"] = TimeUtil::toIso8601(e.timestamp);
    obj["data"] = toJson(e.data);
    return obj;
}

// ------------------------------------------------------------
// 反序列化（用于结果持久化的回放）；字段缺失时保留默认值
// ------------------------------------------------------------

inline std::vector<std::string> stringsFromJson(const Json& json) {
    std::vector<std::string> out;
    if (!json.isArray()) return out;
    for (const auto& v : json.asArray()) if (v.isString()) out.push_back(v.asString());
    return out;
}

inline std::map<std::string, std::string> stringMapFromJson(const Json& json) {
    std::map<std::string, std::string> out;
    if (!json.isObject()) return out;
    for (const auto& kv : json.asObject()) out[kv.first] = kv.second.isString() ? kv.second.asString() : kv.second.dump();
    return out;
}

inline StepResult stepResultFromJson(const Json& json) {
    StepResult r;
    r.stepText = json["step"].asString("");
    r.parameterizedText = json["parameterized_text"].asString("");
    r.state = stringToTestState(json["state"].asString("passed"));
    r.duration = json["duration"].asNumber(0.0);
    r.errorMessage = json["error"].asString("");
    r.stackTrace = json["stack_trace"].asString("");
    r.messages = stringsFromJson(json["messages"]);
    r.isConcept = json["is_concept"].asBool(false);
    if (json["concept_steps"].isArray()) {
        for (const auto& s : json["concept_steps"].asArray()) r.conceptSteps.push_back(stepResultFromJson(s));
    }
    return r;
}

inline ScenarioResult scenarioResultFromJson(const Json& json) {
    ScenarioResult r;
    r.scenarioName = json["name"].asString("");
    r.tags = stringsFromJson(json["tags"]);
    r.state = stringToTestState(json["state"].asString("passed"));
    r.duration = json["duration"].asNumber(0.0);
    r.lineNumber = json["line_number"].asInt(0);
    r.errorMessage = json["error"].asString("");
    r.dataRowIndex = json["data_row_index"].asInt(-1);
    r.dataRow = stringMapFromJson(json["data_row"]);
    auto readSteps = [&](const char* key, std::vector<StepResult>& out) {
        if (!json[key].isArray()) return;
        for (const auto& s : json[key].asArray()) out.push_back(stepResultFromJson(s));
    };
    readSteps("context_steps", r.contextSteps);
    readSteps("steps", r.stepResults);
    readSteps("teardown_steps", r.teardownSteps);
    return r;
}

inline SpecResult specResultFromJson(const Json& json) {
    SpecResult r;
    r.specFile = json["file"].asString("");
    r.specName = json["name"].asString("");
    r.tags = stringsFromJson(json["tags"]);
    r.state = stringToTestState(json["state"].asString("passed"));
    r.duration = json["duration"].asNumber(0.0);
    r.errorMessage = json["error"].asString("");
    r.totalScenarios = json["total_scenarios"].asInt(0);
    r.passedScenarios = json["passed_scenarios"].asInt(0);
    r.failedScenarios = json["failed_scenarios"].asInt(0);
    r.skippedScenarios = json["skipped_scenarios"].asInt(0);
    if (json["scenarios"].isArray()) {
        for (const auto& s : json["scenarios"].asArray()) r.scenarioResults.push_back(scenarioResultFromJson(s));
    }
    return r;
}

inline TestResult testResultFromJson(const Json& json) {
    TestResult r;
    r.testId = json["test_id"].asString("");
    r.finalState = stringToTestState(json["state"].asString("passed"));
    r.totalDuration = json["duration"].asNumber(0.0);
    r.startTime = TimeUtil::fromIso8601(json["start_time"].asString(""));
    r.endTime = TimeUtil::fromIso8601(json["end_time"].asString(""));
    r.totalScenarios = json["total_scenarios"].asInt(0);
    r.passedScenarios = json["passed_scenarios"].asInt(0);
    r.failedScenarios = json["failed_scenarios"].asInt(0);
    r.skippedScenarios = json["skipped_scenarios"].asInt(0);
    r.errors = stringsFromJson(json["errors"]);
    r.warnings = stringsFromJson(json["warnings"]);
    if (json["specs"].isArray()) {
        for (const auto& s : json["specs"].asArray()) r.specResults.push_back(specResultFromJson(s));
    }
    return r;
}

inline TestStatus testStatusFromJson(const Json& json) {
    TestStatus s;
    s.testId = json["test_id"].asString("");
    s.state = stringToTestState(json["state"].asString("queued"));
    s.progress = json["progress"].asNumber(0.0);
    s.totalSpecs = json["total_specs"].asInt(0);
    s.executedSpecs = json["executed_specs"].asInt(0);
    s.passedSpecs = json["passed_specs"].asInt(0);
    s.failedSpecs = json["failed_specs"].asInt(0);
    s.totalScenarios = json["total_scenarios"].asInt(0);
    s.executedScenarios = json["executed_scenarios"].asInt(0);
    s.passedScenarios = json["passed_scenarios"].asInt(0);
    s.failedScenarios = json["failed_scenarios"].asInt(0);
    s.skippedScenarios = json["skipped_scenarios"].asInt(0);
    s.currentSpec = json["current_spec"].asString("");
    s.currentScenario = json["current_scenario"].asString("");
    s.currentStep = json["current_step"].asString("");
    s.submitTime = TimeUtil::fromIso8601(json["submit_time"].asString(""));
    s.startTime = TimeUtil::fromIso8601(json["start_time"].asString(""));
    s.endTime = TimeUtil::fromIso8601(json["end_time"].asString(""));
    s.errors = stringsFromJson(json["errors"]);
    s.warnings = stringsFromJson(json["warnings"]);
    return s;
}

/**
 * 从 JSON 解析测试请求；失败时返回 false 并填充 error
 */
inline bool testRequestFromJson(const Json& json, TestRequest& request, std::string& error) {
    if (!json.isObject()) {
        error = "Request body must be a JSON object";
        return false;
    }
    auto readStrings = [&](const char* key, std::vector<std::string>& out) -> bool {
        const Json& v = json[key];
        if (v.isNull()) return true;
        if (v.isString()) {
            if (!v.asString().empty()) out.push_back(v.asString());
            return true;
        }
        if (!v.isArray()) {
            error = std::string(key) + " must be an array of strings";
            return false;
        }
        for (const auto& item : v.asArray()) {
            if (!item.isString()) {
                error = std::string(key) + " must contain only strings";
                return false;
            }
            if (!item.asString().empty()) out.push_back(item.asString());
        }
        return true;
    };

    if (!readStrings("spec_files", request.specFiles)) return false;
    if (!readStrings("specs", request.specFiles)) return false;
    if (!readStrings("tags", request.tags)) return false;
    if (!readStrings("scenarios", request.scenarios)) return false;

    if (json["name"].isString()) request.name = json["name"].asString();
    if (json["environment"].isString()) request.environment = json["environment"].asString();
    if (json["priority"].isString()) request.priority = stringToPriority(json["priority"].asString());
    if (json["parallel_streams"].isNumber()) request.parallelStreams = std::max(1, json["parallel_streams"].asInt());
    if (json["timeout_ms"].isNumber()) request.timeoutMs = std::max(0, json["timeout_ms"].asInt());
    if (json["fail_fast"].isBool()) request.failFast = json["fail_fast"].asBool();
    if (json["callback_url"].isString()) request.callbackUrl = json["callback_url"].asString();
    if (json["submitted_by"].isString()) request.submittedBy = json["submitted_by"].asString();
    if (json["metadata"].isObject()) {
        for (const auto& kv : json["metadata"].asObject()) {
            request.metadata[kv.first] = kv.second.isString() ? kv.second.asString() : kv.second.dump();
        }
    }
    return true;
}

} // namespace testhub
