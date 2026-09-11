/*
 * TestHub - REST API 路由
 */

#include "../testhub.h"
#include "../model/json_convert.h"
#include "../report/report_writer.h"
#include "../util/http_client.h"
#include "../util/logger.h"
#include "../util/string_util.h"

#include <stdexcept>

namespace testhub {

namespace {

Json parseBody(const HttpRequest& req) {
    if (StringUtil::trim(req.body).empty()) return Json::object();
    return Json::parse(req.body);  // ParseError 由 HttpServer::dispatch 转为 400
}

size_t parseSize(const std::string& s, size_t def, size_t max) {
    if (s.empty()) return def;
    try {
        long long v = std::stoll(s);
        if (v < 0) return def;
        return std::min<size_t>(static_cast<size_t>(v), max);
    } catch (...) {
        return def;
    }
}

bool parseBool(const std::string& s, bool def = false) {
    if (s.empty()) return def;
    return StringUtil::toBool(s);
}

Json summaryToJson(const spec::SpecSummary& s) {
    Json j = Json::object();
    j["file"] = s.file;
    j["heading"] = s.heading;
    j["tags"] = toJson(s.tags);
    j["scenario_count"] = s.scenarioCount;
    j["is_data_driven"] = s.isDataDriven;
    j["valid"] = s.valid;
    j["size"] = static_cast<double>(s.size);
    j["modified_at"] = static_cast<double>(s.modifiedAt);
    Json errors = Json::array();
    for (const auto& e : s.errors) errors.push(toJson(e));
    j["errors"] = errors;
    return j;
}

} // namespace

void TestHub::registerApiRoutes() {
    HttpServer& http = *httpServer_;

    // ---------------- 健康与状态 ----------------
    http.get("/api/v1/health", [this](const HttpRequest&) {
        Json j = Json::object();
        j["status"] = "ok";
        j["version"] = version();
        j["running"] = running_.load();
        j["uptime_seconds"] = running_ ? std::chrono::duration<double>(TimeUtil::now() - startedAt_).count() : 0.0;
        j["runner"] = runnerBridge_ ? runnerStateToString(runnerBridge_->getStatus().state) : "none";
        j["auth_required"] = auth_.enabled();
        j["auth_protect_reads"] = auth_.enabled() && auth_.config().protectReads;
        return HttpResponse::json(200, j);
    });

    http.get("/api/v1/status", [this](const HttpRequest&) {
        return HttpResponse::json(200, statusJson());
    });

    http.get("/api/v1/config", [this](const HttpRequest&) {
        return HttpResponse::json(200, config_.toJson());
    });

    http.get("/api/v1/projects", [this](const HttpRequest&) {
        return HttpResponse::json(200, projectsJson());
    });
    http.post("/api/v1/projects/{id}/select", [this](const HttpRequest& req) {
        std::string error;
        if (!selectProject(req.param("id"), error)) {
            int code = error.find("not found") != std::string::npos ? 404 : 409;
            return HttpResponse::error(code, error);
        }
        return HttpResponse::json(200, projectsJson());
    });

    // ---------------- 测试 ----------------
    auto submit = [this](const HttpRequest& req) {
        Json body = parseBody(req);
        TestRequest request;
        std::string error;
        if (!testRequestFromJson(body, request, error)) return HttpResponse::error(400, error);
        if (!request.callbackUrl.empty()) {
            ParsedUrl parsed;
            if (!HttpClient::parseUrl(request.callbackUrl, parsed, &error)) {
                return HttpResponse::error(400, "Invalid callback_url: " + error);
            }
        }
        try {
            std::string id = engine_->submit(request);
            Json j = Json::object();
            j["test_id"] = id;
            j["status"] = "queued";
            j["queue_position"] = engine_->queuePosition(id);
            j["message"] = "Test submitted successfully";
            HttpResponse r = HttpResponse::json(202, j);
            r.headers["Location"] = "/api/v1/tests/" + id;
            return r;
        } catch (const std::invalid_argument& e) {
            return HttpResponse::error(400, e.what());
        }
    };
    http.post("/api/v1/tests/run", submit);
    http.post("/api/v1/tests", submit);

    http.get("/api/v1/tests", [this](const HttpRequest& req) {
        size_t limit = parseSize(req.query("limit"), 100, 1000);
        size_t offset = parseSize(req.query("offset"), 0, 1000000);
        std::string state = req.query("state");
        auto tests = engine_->list(state, limit, offset);
        Json arr = Json::array();
        for (const auto& t : tests) arr.push(toJson(t));
        Json j = Json::object();
        j["tests"] = arr;
        j["count"] = static_cast<int>(tests.size());
        j["total"] = static_cast<int>(state.empty() || state == "active" || state == "finished" ? engine_->count() : engine_->count(state));
        j["limit"] = static_cast<int>(limit);
        j["offset"] = static_cast<int>(offset);
        j["queue_size"] = static_cast<int>(engine_->queueSize());
        return HttpResponse::json(200, j);
    });

    http.get("/api/v1/trends", [this](const HttpRequest& req) {
        std::string spec = req.query("spec");
        size_t limit = parseSize(req.query("limit"), 50, 200);
        auto series = groupTrends(engine_->trendRuns(), spec, limit);
        Json arr = Json::array();
        for (const auto& s : series) arr.push(specTrendToJson(s));
        Json j = Json::object();
        j["specs"] = arr;
        j["count"] = static_cast<int>(arr.size());
        return HttpResponse::json(200, j);
    });

    http.get("/api/v1/tests/{id}/compare", [this](const HttpRequest& req) {
        std::string error;
        auto cmp = engine_->compareTests(req.param("id"), req.query("with"), error);
        if (!cmp) {
            int code = (error.find("not found") != std::string::npos || error.find("No previous") != std::string::npos)
                           ? 404
                           : 400;
            return HttpResponse::error(code, error);
        }
        return HttpResponse::json(200, comparisonToJson(*cmp));
    });

    http.del("/api/v1/tests", [this](const HttpRequest&) {
        size_t removed = engine_->clearHistory();
        Json j = Json::object();
        j["removed"] = static_cast<int>(removed);
        return HttpResponse::json(200, j);
    });

    http.get("/api/v1/tests/{id}", [this](const HttpRequest& req) {
        std::string id = req.param("id");
        auto record = engine_->getRecord(id);
        if (!record) return HttpResponse::error(404, "Test not found: " + id);
        Json j = toJson(record->status);
        j["request"] = toJson(record->request);
        j["resolved_specs"] = toJson(record->resolvedSpecs);
        if (record->status.state == TestState::QUEUED) j["queue_position"] = engine_->queuePosition(id);
        if (record->hasResult) {
            j["duration"] = record->result.totalDuration;
            j["has_result"] = true;
        } else {
            j["has_result"] = false;
        }
        return HttpResponse::json(200, j);
    });

    http.get("/api/v1/tests/{id}/result", [this](const HttpRequest& req) {
        std::string id = req.param("id");
        auto record = engine_->getRecord(id);
        if (!record) return HttpResponse::error(404, "Test not found: " + id);
        if (!record->hasResult) {
            Json j = Json::object();
            j["test_id"] = id;
            j["state"] = testStateToString(record->status.state);
            j["message"] = "Test has not finished yet";
            j["progress"] = record->status.progress;
            return HttpResponse::json(202, j);
        }
        Json j = toJson(record->result);
        j["request"] = toJson(record->request);
        return HttpResponse::json(200, j);
    });

    http.get("/api/v1/tests/{id}/report", [this](const HttpRequest& req) {
        std::string id = req.param("id");
        ReportFormat format;
        if (!parseReportFormat(req.query("format"), format)) {
            return HttpResponse::error(400, "Unsupported report format '" + req.query("format") + "' (expected junit or html)");
        }
        auto record = engine_->getRecord(id);
        if (!record) return HttpResponse::error(404, "Test not found: " + id);
        if (!record->hasResult) {
            return HttpResponse::error(409, "Test has not finished yet (state '" + testStateToString(record->status.state) + "')");
        }
        HttpResponse r = HttpResponse::text(200, ReportWriter::render(*record, format), reportContentType(format));
        if (parseBool(req.query("download"))) {
            r.headers["Content-Disposition"] = "attachment; filename=\"" + id + "." + reportFileExtension(format) + "\"";
        }
        return r;
    });

    http.get("/api/v1/tests/{id}/events", [this](const HttpRequest& req) {
        std::string id = req.param("id");
        if (!engine_->exists(id)) return HttpResponse::error(404, "Test not found: " + id);
        size_t limit = parseSize(req.query("limit"), 200, 1000);
        Json arr = Json::array();
        for (const auto& e : EventBus::getInstance().recentEvents(limit, id)) arr.push(toJson(e));
        Json j = Json::object();
        j["test_id"] = id;
        j["events"] = arr;
        return HttpResponse::json(200, j);
    });

    auto cancel = [this](const HttpRequest& req) {
        std::string id = req.param("id");
        auto status = engine_->getStatus(id);
        if (!status) return HttpResponse::error(404, "Test not found: " + id);
        if (isTerminalState(status->state)) {
            return HttpResponse::error(409, "Test already finished with state '" + testStateToString(status->state) + "'");
        }
        if (!engine_->cancel(id)) return HttpResponse::error(409, "Test could not be cancelled");
        Json j = Json::object();
        j["test_id"] = id;
        j["message"] = "Cancellation requested";
        return HttpResponse::json(200, j);
    };
    http.post("/api/v1/tests/{id}/cancel", cancel);

    http.del("/api/v1/tests/{id}", [this, cancel](const HttpRequest& req) {
        std::string id = req.param("id");
        auto status = engine_->getStatus(id);
        if (!status) return HttpResponse::error(404, "Test not found: " + id);
        if (!isTerminalState(status->state)) return cancel(req);
        if (!engine_->remove(id)) return HttpResponse::error(409, "Test could not be removed");
        Json j = Json::object();
        j["test_id"] = id;
        j["message"] = "Test record removed";
        return HttpResponse::json(200, j);
    });

    http.post("/api/v1/tests/{id}/rerun", [this](const HttpRequest& req) {
        std::string id = req.param("id");
        bool failedOnly = parseBool(req.query("failed_only"));
        Json body = parseBody(req);
        if (body["failed_only"].isBool()) failedOnly = body["failed_only"].asBool();
        try {
            std::string newId = engine_->rerun(id, failedOnly);
            if (newId.empty()) return HttpResponse::error(404, "Test not found: " + id);
            Json j = Json::object();
            j["test_id"] = newId;
            j["rerun_of"] = id;
            j["status"] = "queued";
            return HttpResponse::json(202, j);
        } catch (const std::invalid_argument& e) {
            return HttpResponse::error(400, e.what());
        }
    });

    http.get("/api/v1/queue", [this](const HttpRequest&) {
        Json arr = Json::array();
        for (const auto& t : engine_->queue().snapshot()) {
            Json j = Json::object();
            j["test_id"] = t.request.id;
            j["name"] = t.request.name;
            j["priority"] = priorityToString(t.request.priority);
            j["spec_files"] = toJson(t.request.specFiles);
            j["tags"] = toJson(t.request.tags);
            j["submit_time"] = TimeUtil::toIso8601(t.submitTime);
            arr.push(j);
        }
        Json j = Json::object();
        j["queue"] = arr;
        j["size"] = static_cast<int>(arr.size());
        return HttpResponse::json(200, j);
    });

    // ---------------- 规范 ----------------
    http.get("/api/v1/specs", [this](const HttpRequest& req) {
        auto list = specs_.list();
        std::string tag = req.query("tag");
        Json arr = Json::array();
        int scenarios = 0, invalid = 0;
        for (const auto& s : list) {
            if (!tag.empty() && std::find(s.tags.begin(), s.tags.end(), tag) == s.tags.end()) continue;
            arr.push(summaryToJson(s));
            scenarios += s.scenarioCount;
            if (!s.valid) invalid++;
        }
        Json j = Json::object();
        j["specs"] = arr;
        j["count"] = static_cast<int>(arr.size());
        j["total_scenarios"] = scenarios;
        j["invalid"] = invalid;
        j["specs_dir"] = specs_.specsDir();
        j["current_project"] = config_.currentProjectId;
        j["concepts"] = static_cast<int>(specs_.concepts().size());
        return HttpResponse::json(200, j);
    });

    http.post("/api/v1/specs/validate", [this](const HttpRequest& req) {
        Json body = parseBody(req);
        Json results = Json::array();
        bool allValid = true;
        auto addResult = [&](const spec::ParseResult& pr, const std::string& name) {
            Json r = Json::object();
            r["file"] = name;
            r["valid"] = pr.errors.empty();
            Json errs = Json::array();
            for (const auto& e : pr.errors) errs.push(toJson(e));
            Json warns = Json::array();
            for (const auto& w : pr.warnings) warns.push(toJson(w));
            r["errors"] = errs;
            r["warnings"] = warns;
            if (pr.specification) {
                r["heading"] = pr.specification->heading;
                r["scenario_count"] = static_cast<int>(pr.specification->scenarios.size());
                r["tags"] = toJson(pr.specification->tags);
            }
            if (!pr.errors.empty()) allValid = false;
            results.push(r);
        };
        if (body["content"].isString()) {
            addResult(specs_.parseText(body["content"].asString(), body["file"].asString("inline.spec")),
                      body["file"].asString("inline.spec"));
        }
        std::vector<std::string> files;
        for (const auto& f : body["files"].asArray()) if (f.isString()) files.push_back(f.asString());
        for (const auto& f : body["spec_files"].asArray()) if (f.isString()) files.push_back(f.asString());
        if (!files.empty()) {
            std::vector<std::string> missing;
            auto resolved = specs_.resolve(files, &missing);
            for (const auto& m : missing) {
                Json r = Json::object();
                r["file"] = m;
                r["valid"] = false;
                Json errs = Json::array();
                Json e = Json::object();
                e["file"] = m;
                e["line"] = 0;
                e["message"] = "File not found";
                errs.push(e);
                r["errors"] = errs;
                results.push(r);
                allValid = false;
            }
            for (const auto& path : resolved) addResult(specs_.load(path), specs_.toRelative(path));
        }
        if (!body["content"].isString() && files.empty()) {
            // 校验整个目录
            for (const auto& s : specs_.list()) addResult(specs_.load(s.absolutePath), s.file);
        }
        Json j = Json::object();
        j["valid"] = allValid;
        j["results"] = results;
        return HttpResponse::json(200, j);
    });

    http.post("/api/v1/specs/reload", [this](const HttpRequest&) {
        auto errors = specs_.reloadConcepts();
        // 手动重载同时让监控器同步快照并报告期间发生的文件变化
        spec::SpecChangeSet changes = specWatcher_.scan();
        Json errs = Json::array();
        for (const auto& e : errors) errs.push(toJson(e));
        Json j = Json::object();
        j["concepts"] = static_cast<int>(specs_.concepts().size());
        j["errors"] = errs;
        j["specs"] = static_cast<int>(specs_.list().size());
        Json ch = Json::object();
        ch["created"] = toJson(changes.created);
        ch["updated"] = toJson(changes.updated);
        ch["deleted"] = toJson(changes.deleted);
        j["changes"] = ch;
        publishEvent(EventType::SPECS_RELOADED, "", {{"source", "manual"}, {"concepts", std::to_string(specs_.concepts().size())}});
        return HttpResponse::json(200, j);
    });

    http.get("/api/v1/concepts", [this](const HttpRequest&) {
        Json arr = Json::array();
        for (const auto& kv : specs_.concepts().all()) {
            Json c = Json::object();
            c["heading"] = kv.second.heading;
            c["parameterized_text"] = kv.second.parameterizedText;
            c["params"] = toJson(kv.second.params);
            c["file"] = kv.second.fileName;
            c["line_number"] = kv.second.lineNumber;
            Json steps = Json::array();
            for (const auto& s : kv.second.steps) steps.push(toJson(s));
            c["steps"] = steps;
            arr.push(c);
        }
        Json j = Json::object();
        j["concepts"] = arr;
        j["count"] = static_cast<int>(arr.size());
        return HttpResponse::json(200, j);
    });

    http.get("/api/v1/specs/*", [this](const HttpRequest& req) {
        std::string rel = req.param("*");
        std::string content;
        if (!specs_.readRaw(rel, content)) return HttpResponse::error(404, "Spec not found: " + rel);
        if (parseBool(req.query("raw"))) return HttpResponse::text(200, content);
        spec::ParseResult pr = specs_.parseText(content, rel);
        Json j = Json::object();
        j["file"] = rel;
        j["content"] = content;
        j["valid"] = pr.errors.empty();
        Json errs = Json::array();
        for (const auto& e : pr.errors) errs.push(toJson(e));
        j["errors"] = errs;
        Json warns = Json::array();
        for (const auto& w : pr.warnings) warns.push(toJson(w));
        j["warnings"] = warns;
        if (pr.specification) j["spec"] = toJson(*pr.specification);
        return HttpResponse::json(200, j);
    });

    http.put("/api/v1/specs/*", [this](const HttpRequest& req) {
        std::string rel = req.param("*");
        std::string content = req.body;
        std::string ct = StringUtil::toLower(req.header("content-type"));
        if (ct.find("application/json") != std::string::npos) {
            Json body = parseBody(req);
            if (!body["content"].isString()) return HttpResponse::error(400, "JSON body must contain 'content'");
            content = body["content"].asString();
        }
        std::string error;
        bool existed = false;
        {
            std::string tmp;
            existed = specs_.readRaw(rel, tmp);
        }
        if (!specs_.writeRaw(rel, content, &error)) return HttpResponse::error(400, error);
        if (spec::SpecRepository::isConceptFile(rel)) specs_.reloadConcepts();
        specWatcher_.acknowledge();  // 自己写的文件不再由监控器重复报告
        spec::ParseResult pr = specs_.parseText(content, rel);
        Json j = Json::object();
        j["file"] = rel;
        j["created"] = !existed;
        j["valid"] = pr.errors.empty();
        Json errs = Json::array();
        for (const auto& e : pr.errors) errs.push(toJson(e));
        j["errors"] = errs;
        publishEvent(EventType::SPECS_RELOADED, "", {{"source", "api"}, {"file", rel}, {"action", existed ? "updated" : "created"}});
        return HttpResponse::json(existed ? 200 : 201, j);
    });

    http.del("/api/v1/specs/*", [this](const HttpRequest& req) {
        std::string rel = req.param("*");
        std::string error;
        if (!specs_.remove(rel, &error)) return HttpResponse::error(error == "File not found" ? 404 : 400, error);
        if (spec::SpecRepository::isConceptFile(rel)) specs_.reloadConcepts();
        specWatcher_.acknowledge();
        publishEvent(EventType::SPECS_RELOADED, "", {{"source", "api"}, {"file", rel}, {"action", "deleted"}});
        Json j = Json::object();
        j["file"] = rel;
        j["deleted"] = true;
        return HttpResponse::json(200, j);
    });

    // ---------------- Runner ----------------
    http.get("/api/v1/runner/status", [this](const HttpRequest&) {
        return HttpResponse::json(200, toJson(runnerBridge_->getStatus()));
    });

    http.post("/api/v1/runner/restart", [this](const HttpRequest&) {
        bool ok = runnerBridge_->restartRunner();
        Json j = toJson(runnerBridge_->getStatus());
        j["restarted"] = ok;
        j["message"] = ok ? "Runner restarted" : "Failed to restart runner";
        return HttpResponse::json(ok ? 200 : 500, j);
    });

    http.get("/api/v1/runner/steps", [this](const HttpRequest&) {
        auto steps = runnerBridge_->getAllSteps();
        Json arr = Json::array();
        for (const auto& s : steps) {
            Json j = Json::object();
            j["text"] = s.stepText;
            j["parameterized_text"] = s.parameterizedStepText;
            j["params"] = toJson(s.parameters);
            arr.push(j);
        }
        Json j = Json::object();
        j["steps"] = arr;
        j["count"] = static_cast<int>(arr.size());
        j["reported"] = !steps.empty();
        return HttpResponse::json(200, j);
    });

    // ---------------- 测试计划（cron，UTC） ----------------
    auto scheduleFromBody = [](const Json& body, Schedule& s, bool& hasRequest, std::string& error) -> bool {
        if (body["name"].isString()) s.name = body["name"].asString();
        if (body["cron"].isString()) s.cron = body["cron"].asString();
        if (body["enabled"].isBool()) s.enabled = body["enabled"].asBool();
        if (body["skip_if_running"].isBool()) s.skipIfRunning = body["skip_if_running"].asBool();
        hasRequest = body["request"].isObject();
        if (hasRequest) {
            if (!testRequestFromJson(body["request"], s.request, error)) return false;
        } else if (body["spec_files"].isArray() || body["specs"].isArray() || body["tags"].isArray() ||
                   body["scenarios"].isArray()) {
            hasRequest = true;
            if (!testRequestFromJson(body, s.request, error)) return false;
        }
        return true;
    };

    http.get("/api/v1/schedules", [this](const HttpRequest&) {
        Json arr = Json::array();
        for (const auto& s : scheduler_.list()) arr.push(Scheduler::toJson(s));
        Json j = Json::object();
        j["schedules"] = arr;
        j["count"] = static_cast<int>(arr.size());
        SchedulerStats st = scheduler_.stats();
        j["enabled"] = st.enabled;
        return HttpResponse::json(200, j);
    });

    http.post("/api/v1/schedules", [this, scheduleFromBody](const HttpRequest& req) {
        Json body = parseBody(req);
        Schedule s;
        bool hasRequest = false;
        std::string error;
        if (!scheduleFromBody(body, s, hasRequest, error)) return HttpResponse::error(400, error);
        if (s.cron.empty()) return HttpResponse::error(400, "cron is required");
        Schedule created = scheduler_.create(s, error);
        if (!error.empty() || created.id.empty()) {
            return HttpResponse::error(400, error.empty() ? "failed to create schedule" : error);
        }
        return HttpResponse::json(201, Scheduler::toJson(created));
    });

    http.get("/api/v1/schedules/{id}", [this](const HttpRequest& req) {
        auto s = scheduler_.get(req.param("id"));
        if (!s) return HttpResponse::error(404, "Schedule not found: " + req.param("id"));
        return HttpResponse::json(200, Scheduler::toJson(*s));
    });

    http.put("/api/v1/schedules/{id}", [this, scheduleFromBody](const HttpRequest& req) {
        std::string id = req.param("id");
        auto existing = scheduler_.get(id);
        if (!existing) return HttpResponse::error(404, "Schedule not found: " + id);
        Json body = parseBody(req);
        Schedule patch = *existing;
        bool hasRequest = false;
        std::string error;
        if (!scheduleFromBody(body, patch, hasRequest, error)) return HttpResponse::error(400, error);
        if (!scheduler_.update(id, patch, hasRequest, error)) {
            return HttpResponse::error(error.find("not found") != std::string::npos ? 404 : 400, error);
        }
        return HttpResponse::json(200, Scheduler::toJson(*scheduler_.get(id)));
    });

    http.del("/api/v1/schedules/{id}", [this](const HttpRequest& req) {
        if (!scheduler_.remove(req.param("id"))) return HttpResponse::error(404, "Schedule not found: " + req.param("id"));
        Json j = Json::object();
        j["removed"] = true;
        return HttpResponse::json(200, j);
    });

    http.post("/api/v1/schedules/{id}/run", [this](const HttpRequest& req) {
        std::string error;
        std::string testId = scheduler_.fireNow(req.param("id"), error);
        if (!error.empty() && testId.empty()) {
            int code = 400;
            if (error.find("not found") != std::string::npos) code = 404;
            else if (error.find("still running") != std::string::npos) code = 409;
            return HttpResponse::error(code, error);
        }
        Json j = Json::object();
        j["test_id"] = testId;
        j["schedule_id"] = req.param("id");
        j["status"] = "queued";
        HttpResponse r = HttpResponse::json(202, j);
        r.headers["Location"] = "/api/v1/tests/" + testId;
        return r;
    });

    // ---------------- 事件 ----------------
    http.get("/api/v1/events", [](const HttpRequest& req) {
        size_t limit = parseSize(req.query("limit"), 100, 1000);
        std::string testId = req.query("test_id");
        Json arr = Json::array();
        for (const auto& e : EventBus::getInstance().recentEvents(limit, testId)) arr.push(toJson(e));
        Json j = Json::object();
        j["events"] = arr;
        j["count"] = static_cast<int>(arr.size());
        return HttpResponse::json(200, j);
    });
}

} // namespace testhub
