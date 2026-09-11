/*
 * TestHub - 主服务器实现
 */

#include "testhub.h"
#include "model/json_convert.h"
#include "util/file_util.h"
#include "util/logger.h"

#include <algorithm>
#include <cctype>

namespace testhub {

namespace {

std::string slugId(const std::string& raw, int fallbackIndex) {
    std::string out;
    for (unsigned char ch : raw) {
        if (std::isalnum(ch)) out.push_back(static_cast<char>(std::tolower(ch)));
        else if (ch == '-' || ch == '_' || ch == '.' || ch == ' ') {
            if (!out.empty() && out.back() != '-') out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "project-" + std::to_string(fallbackIndex);
    return out;
}

std::string uniquifyId(const std::string& id, const std::vector<std::string>& used) {
    std::string cand = id.empty() ? "project" : id;
    if (std::find(used.begin(), used.end(), cand) == used.end()) return cand;
    for (int k = 2; k < 10000; ++k) {
        std::string next = cand + "-" + std::to_string(k);
        if (std::find(used.begin(), used.end(), next) == used.end()) return next;
    }
    return cand + "-x";
}

} // namespace

const SpecProject* TestHubConfig::findProject(const std::string& id) const {
    for (const auto& p : projects) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

SpecProject* TestHubConfig::findProject(const std::string& id) {
    for (auto& p : projects) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

void TestHubConfig::applyCurrentProject() {
    const SpecProject* p = findProject(currentProjectId);
    if (!p && !projects.empty()) p = &projects.front();
    if (!p) return;
    currentProjectId = p->id;
    specsDir = p->dir;
    conceptsDir = p->conceptsDir;
}

void TestHubConfig::finalizeProjects() {
    if (projects.empty()) {
        SpecProject p;
        p.id = "default";
        p.name = "默认";
        p.dir = specsDir.empty() ? "specs" : specsDir;
        p.conceptsDir = conceptsDir;
        projects.push_back(std::move(p));
    }
    std::vector<std::string> used;
    int index = 0;
    for (auto& p : projects) {
        ++index;
        if (p.dir.empty()) p.dir = specsDir.empty() ? "specs" : specsDir;
        if (p.id.empty()) p.id = slugId(p.name, index);
        p.id = uniquifyId(p.id, used);
        used.push_back(p.id);
        if (p.name.empty()) p.name = p.id;
    }
    if (!currentProjectId.empty() && findProject(currentProjectId)) {
        applyCurrentProject();
        return;
    }
    for (const auto& p : projects) {
        if (p.dir == specsDir) {
            currentProjectId = p.id;
            applyCurrentProject();
            return;
        }
    }
    currentProjectId = projects.front().id;
    applyCurrentProject();
}

void TestHubConfig::applySpecsDirOverride(const std::string& dir) {
    if (dir.empty()) return;
    finalizeProjects();
    for (const auto& p : projects) {
        if (p.dir == dir) {
            currentProjectId = p.id;
            applyCurrentProject();
            return;
        }
    }
    if (SpecProject* cur = findProject(currentProjectId)) {
        cur->dir = dir;
    } else if (!projects.empty()) {
        projects.front().dir = dir;
        currentProjectId = projects.front().id;
    }
    applyCurrentProject();
}

// ============================================================
// TestHubConfig
// ============================================================

void TestHubConfig::applyJson(const Json& json) {
    if (!json.isObject()) return;
    const Json& server = json["server"];
    if (server["host"].isString()) host = server["host"].asString();
    if (server["port"].isNumber()) port = server["port"].asInt();
    if (server["worker_threads"].isNumber()) httpWorkerThreads = server["worker_threads"].asInt();
    if (server["web_ui"].isBool()) enableWebUi = server["web_ui"].asBool();
    if (server["web_dir"].isString()) webDir = server["web_dir"].asString();
    if (server["auth_token"].isString()) authToken = server["auth_token"].asString();
    if (server["auth_protect_reads"].isBool()) authProtectReads = server["auth_protect_reads"].asBool();

    const Json& runner = json["runner"];
    if (runner["language"].isString()) runnerLanguage = runner["language"].asString();
    if (runner["command"].isString()) runnerCommand = runner["command"].asString();
    if (runner["project_path"].isString()) projectPath = runner["project_path"].asString();
    if (runner["connection_timeout"].isNumber()) runnerConnectionTimeout = runner["connection_timeout"].asInt();
    if (runner["request_timeout"].isNumber()) runnerRequestTimeout = runner["request_timeout"].asInt();
    if (runner["auto_restart"].isBool()) autoRestartRunner = runner["auto_restart"].asBool();
    if (runner["max_restarts"].isNumber()) runnerMaxRestarts = runner["max_restarts"].asInt();
    if (runner["mock_delay_ms"].isNumber()) mockDelayMs = runner["mock_delay_ms"].asInt();
    if (runner["pool_size"].isNumber()) runnerPoolSize = runner["pool_size"].asInt();

    const Json& execution = json["execution"];
    if (execution["max_concurrent_tests"].isNumber()) maxConcurrentTests = execution["max_concurrent_tests"].asInt();
    if (execution["default_timeout"].isNumber()) defaultTimeout = execution["default_timeout"].asInt();
    if (execution["step_timeout"].isNumber()) stepTimeout = execution["step_timeout"].asInt();
    if (execution["history_limit"].isNumber()) historyLimit = static_cast<size_t>(execution["history_limit"].asInt());
    if (execution["results_dir"].isString()) resultsDir = execution["results_dir"].asString();
    if (execution["environment"].isObject()) {
        for (const auto& kv : execution["environment"].asObject()) {
            environment[kv.first] = kv.second.isString() ? kv.second.asString() : kv.second.dump();
        }
    }

    const Json& scheduler = json["scheduler"];
    if (scheduler["enabled"].isBool()) schedulerEnabled = scheduler["enabled"].asBool();
    if (scheduler["interval_ms"].isNumber()) schedulerIntervalMs = scheduler["interval_ms"].asInt();
    if (scheduler["dir"].isString()) schedulesDir = scheduler["dir"].asString();

    const Json& callbacks = json["callbacks"];
    if (callbacks["enabled"].isBool()) callbacksEnabled = callbacks["enabled"].asBool();
    if (callbacks["timeout_ms"].isNumber()) callbackTimeoutMs = callbacks["timeout_ms"].asInt();
    if (callbacks["max_attempts"].isNumber()) callbackMaxAttempts = callbacks["max_attempts"].asInt();
    if (callbacks["retry_backoff_ms"].isNumber()) callbackRetryBackoffMs = callbacks["retry_backoff_ms"].asInt();
    if (callbacks["public_base_url"].isString()) publicBaseUrl = callbacks["public_base_url"].asString();

    const Json& specs = json["specs"];
    if (specs["default_dir"].isString()) specsDir = specs["default_dir"].asString();
    if (specs["dir"].isString()) specsDir = specs["dir"].asString();
    if (specs["concepts_dir"].isString()) conceptsDir = specs["concepts_dir"].asString();
    if (specs["current"].isString()) currentProjectId = specs["current"].asString();
    if (specs["projects"].isArray()) {
        projects.clear();
        for (const auto& item : specs["projects"].asArray()) {
            if (!item.isObject()) continue;
            SpecProject p;
            p.id = item["id"].asString("");
            p.name = item["name"].asString("");
            p.dir = item["dir"].asString("");
            p.conceptsDir = item["concepts_dir"].asString("");
            if (p.dir.empty() && p.id.empty() && p.name.empty()) continue;
            projects.push_back(std::move(p));
        }
    }
    if (specs["watch"].isBool()) specsWatch = specs["watch"].asBool();
    if (specs["watch_interval_ms"].isNumber()) specsWatchIntervalMs = specs["watch_interval_ms"].asInt();

    const Json& logging = json["logging"];
    if (logging["level"].isString()) logLevel = logging["level"].asString();
    if (logging["file"].isString()) logFile = logging["file"].asString();
    if (logging["requests"].isBool()) logRequests = logging["requests"].asBool();
}

Json TestHubConfig::toJson(bool maskSecrets) const {
    Json j = Json::object();
    Json server = Json::object();
    server["host"] = host;
    server["port"] = port;
    server["worker_threads"] = httpWorkerThreads;
    server["web_ui"] = enableWebUi;
    if (!webDir.empty()) server["web_dir"] = webDir;
    server["auth_token"] = authToken.empty() ? "" : (maskSecrets ? "***" : authToken);
    server["auth_protect_reads"] = authProtectReads;
    j["server"] = server;

    Json runner = Json::object();
    runner["language"] = runnerLanguage;
    runner["command"] = runnerCommand;
    runner["project_path"] = projectPath;
    runner["connection_timeout"] = runnerConnectionTimeout;
    runner["request_timeout"] = runnerRequestTimeout;
    runner["auto_restart"] = autoRestartRunner;
    runner["max_restarts"] = runnerMaxRestarts;
    runner["mock_delay_ms"] = mockDelayMs;
    runner["pool_size"] = runnerPoolSize;
    j["runner"] = runner;

    Json execution = Json::object();
    execution["max_concurrent_tests"] = maxConcurrentTests;
    execution["default_timeout"] = defaultTimeout;
    execution["step_timeout"] = stepTimeout;
    execution["history_limit"] = static_cast<int>(historyLimit);
    execution["results_dir"] = resultsDir;
    execution["environment"] = testhub::toJson(environment);
    j["execution"] = execution;

    Json scheduler = Json::object();
    scheduler["enabled"] = schedulerEnabled;
    scheduler["interval_ms"] = schedulerIntervalMs;
    scheduler["dir"] = schedulesDir;
    j["scheduler"] = scheduler;

    Json callbacks = Json::object();
    callbacks["enabled"] = callbacksEnabled;
    callbacks["timeout_ms"] = callbackTimeoutMs;
    callbacks["max_attempts"] = callbackMaxAttempts;
    callbacks["retry_backoff_ms"] = callbackRetryBackoffMs;
    callbacks["public_base_url"] = publicBaseUrl;
    j["callbacks"] = callbacks;

    Json specs = Json::object();
    specs["dir"] = specsDir;
    specs["concepts_dir"] = conceptsDir;
    specs["current"] = currentProjectId;
    Json arr = Json::array();
    for (const auto& p : projects) {
        Json o = Json::object();
        o["id"] = p.id;
        o["name"] = p.name;
        o["dir"] = p.dir;
        o["concepts_dir"] = p.conceptsDir;
        arr.push(o);
    }
    specs["projects"] = arr;
    specs["watch"] = specsWatch;
    specs["watch_interval_ms"] = specsWatchIntervalMs;
    j["specs"] = specs;

    Json logging = Json::object();
    logging["level"] = logLevel;
    logging["file"] = logFile;
    logging["requests"] = logRequests;
    j["logging"] = logging;
    return j;
}

// ============================================================
// TestHub
// ============================================================

TestHub::TestHub() = default;

TestHub::~TestHub() { stop(); }

bool TestHub::initialize(const TestHubConfig& config) {
    if (initialized_) return true;
    config_ = config;
    config_.finalizeProjects();

    Logger::getInstance().setLevel(logLevelFromString(config_.logLevel));
    if (!config_.logFile.empty() && !Logger::getInstance().setFile(config_.logFile)) {
        TH_LOG_WARN("testhub", "Cannot open log file: " + config_.logFile);
    }

    specs_.configure(config_.specsDir, config_.conceptsDir);
    auto conceptErrors = specs_.reloadConcepts();
    for (const auto& e : conceptErrors) {
        TH_LOG_WARN("specs", e.fileName + ":" + std::to_string(e.lineNumber) + ": " + e.message);
    }
    spec::SpecWatcherConfig watchConfig;
    watchConfig.enabled = config_.specsWatch;
    watchConfig.intervalMs = config_.specsWatchIntervalMs;
    specWatcher_.configure(watchConfig);

    HttpServerConfig httpConfig;
    httpConfig.host = config_.host;
    httpConfig.port = config_.port;
    httpConfig.workerThreads = config_.httpWorkerThreads;
    httpConfig.logRequests = config_.logRequests;
    httpServer_ = std::make_unique<HttpServer>(httpConfig);
    wsServer_ = std::make_unique<WebSocketServer>();
    runnerBridge_ = std::make_unique<RunnerBridge>();
    engine_ = std::make_unique<ExecutionEngine>(specs_, *runnerBridge_);

    EngineConfig engineConfig;
    engineConfig.workerThreads = config_.maxConcurrentTests;
    engineConfig.defaultTimeoutMs = config_.defaultTimeout;
    engineConfig.stepTimeoutMs = config_.stepTimeout;
    engineConfig.historyLimit = config_.historyLimit;
    engineConfig.resultsDir = config_.resultsDir;
    engineConfig.environment = config_.environment;
    engine_->configure(engineConfig);

    SchedulerConfig schedConfig;
    schedConfig.enabled = config_.schedulerEnabled;
    schedConfig.intervalMs = config_.schedulerIntervalMs;
    schedConfig.dir = config_.schedulesDir;
    scheduler_.configure(schedConfig);
    scheduler_.setSubmit([this](TestRequest req) { return engine_->submit(req); });
    scheduler_.setIsActive([this](const std::string& id) {
        auto rec = engine_->getRecord(id);
        if (!rec) return false;
        return rec->status.state == TestState::QUEUED || rec->status.state == TestState::RUNNING;
    });

    notifier_ = std::make_unique<CallbackNotifier>(*engine_);
    CallbackConfig callbackConfig;
    callbackConfig.enabled = config_.callbacksEnabled;
    callbackConfig.timeoutMs = config_.callbackTimeoutMs;
    callbackConfig.maxAttempts = config_.callbackMaxAttempts;
    callbackConfig.retryBackoffMs = config_.callbackRetryBackoffMs;
    callbackConfig.publicBaseUrl = config_.publicBaseUrl;
    notifier_->configure(callbackConfig);

    AuthConfig authConfig;
    authConfig.token = config_.authToken;
    authConfig.protectReads = config_.authProtectReads;
    auth_.configure(authConfig);
    if (auth_.enabled()) {
        httpServer_->setRequestFilter([this](const HttpRequest& req, HttpResponse& denied) { return auth_.authorize(req, denied); });
    }

    registerApiRoutes();
    if (config_.enableWebUi) registerWebUi();
    wsServer_->attach(*httpServer_, "/ws/v1/events");
    wsServer_->attach(*httpServer_, "/ws");

    eventHandlerId_ = EventBus::getInstance().subscribe("*", [this](const Event& event) {
        if (wsServer_) wsServer_->broadcastEvent(event);
    });

    initialized_ = true;
    return true;
}

std::string TestHub::runtimeBaseUrl() const {
    if (!config_.publicBaseUrl.empty()) {
        std::string url = config_.publicBaseUrl;
        while (!url.empty() && (url.back() == '/' || url.back() == '\\')) url.pop_back();
        return url;
    }
    int port = boundPort();
    if (port <= 0) return "";
    return "http://127.0.0.1:" + std::to_string(port);
}

bool TestHub::start() {
    if (!initialized_) {
        TH_LOG_ERROR("testhub", "TestHub not initialized");
        return false;
    }
    if (running_) return true;

    // 先绑定端口，再启动 Runner：子进程和步骤上下文都能读到 TESTHUB_URL，从而用 .spec 测本进程
    if (!httpServer_->start()) {
        TH_LOG_ERROR("testhub", "Failed to start HTTP server: " + httpServer_->lastError());
        return false;
    }
    std::string selfUrl = runtimeBaseUrl();
    if (!selfUrl.empty()) config_.environment["TESTHUB_URL"] = selfUrl;
    if (!config_.authToken.empty()) config_.environment["TESTHUB_TOKEN"] = config_.authToken;
    {
        EngineConfig engineConfig = engine_->config();
        engineConfig.environment = config_.environment;
        engine_->configure(engineConfig);
    }

    RunnerConfig runnerConfig;
    runnerConfig.language = config_.runnerLanguage;
    runnerConfig.command = config_.runnerCommand;
    runnerConfig.workingDir = config_.projectPath;
    runnerConfig.env = config_.environment;
    runnerConfig.connectionTimeoutMs = config_.runnerConnectionTimeout;
    runnerConfig.requestTimeoutMs = config_.runnerRequestTimeout;
    runnerConfig.autoRestart = config_.autoRestartRunner;
    runnerConfig.maxRestarts = config_.runnerMaxRestarts;
    runnerConfig.mockDelayMs = config_.mockDelayMs;
    runnerConfig.poolSize = config_.effectiveRunnerPoolSize();
    if (!runnerBridge_->start(runnerConfig)) {
        TH_LOG_WARN("testhub", "Runner failed to start; tests will error until the runner is available");
    }

    notifier_->start();
    engine_->start();
    specWatcher_.start();
    scheduler_.start();
    startedAt_ = TimeUtil::now();
    running_ = true;

    TH_LOG_INFO("testhub", std::string("TestHub ") + version() + " started on http://" +
                           (config_.host == "0.0.0.0" ? "localhost" : config_.host) + ":" + std::to_string(boundPort()));
    TH_LOG_INFO("testhub", "Specs directory: " + specs_.specsDir() +
                           (config_.currentProjectId.empty() ? "" : " (project " + config_.currentProjectId + ")"));
    TH_LOG_INFO("testhub", "Runner: " + config_.runnerLanguage + (config_.runnerCommand.empty() ? "" : " (" + config_.runnerCommand + ")") +
                           (runnerBridge_->poolSize() > 1 ? " x" + std::to_string(runnerBridge_->poolSize()) + " (pool)" : ""));
    if (auth_.enabled()) {
        TH_LOG_INFO("testhub", std::string("Auth: Bearer token required for ") + (config_.authProtectReads ? "all API requests and WebSocket" : "write operations"));
    } else {
        TH_LOG_WARN("testhub", "Auth: disabled (set server.auth_token / --auth-token / TESTHUB_AUTH_TOKEN to protect the API)");
    }
    publishEvent(EventType::SERVER_STARTED, "", {{"version", version()}, {"port", std::to_string(boundPort())},
                                                 {"url", runtimeBaseUrl()}});
    return true;
}

void TestHub::stop() {
    if (!running_) return;
    running_ = false;
    publishEvent(EventType::SERVER_STOPPING, "", {});
    EventBus::getInstance().waitForIdle(1000);

    specWatcher_.stop();
    scheduler_.stop();
    if (engine_) engine_->stop();
    if (notifier_) notifier_->stop();
    if (wsServer_) wsServer_->stop();
    if (httpServer_) httpServer_->stop();
    if (runnerBridge_) runnerBridge_->stopRunner();
    if (!eventHandlerId_.empty()) {
        EventBus::getInstance().unsubscribe(eventHandlerId_);
        eventHandlerId_.clear();
    }
    TH_LOG_INFO("testhub", "TestHub stopped");
}

Json TestHub::statusJson() const {
    Json j = Json::object();
    j["name"] = "TestHub";
    j["version"] = version();
    j["running"] = running_.load();
    j["port"] = boundPort();
    j["url"] = runtimeBaseUrl();
    j["started_at"] = TimeUtil::toIso8601(startedAt_);
    j["uptime_seconds"] = running_ ? std::chrono::duration<double>(TimeUtil::now() - startedAt_).count() : 0.0;
    j["specs_dir"] = specs_.specsDir();
    {
        std::lock_guard<std::mutex> lock(projectMutex_);
        j["current_project"] = config_.currentProjectId;
        const SpecProject* p = config_.findProject(config_.currentProjectId);
        j["current_project_name"] = p ? p->name : config_.currentProjectId;
        j["project_count"] = static_cast<int>(config_.projects.size());
    }
    j["concepts"] = static_cast<int>(specs_.concepts().size());
    {
        spec::SpecWatcherStats w = specWatcher_.stats();
        Json watch = Json::object();
        watch["enabled"] = w.enabled;
        watch["interval_ms"] = w.intervalMs;
        watch["tracked_files"] = static_cast<int>(w.trackedFiles);
        watch["scans"] = static_cast<double>(w.scans);
        watch["changes"] = static_cast<double>(w.changes);
        watch["reloads"] = static_cast<double>(w.reloads);
        watch["last_change_at"] = w.lastChangeAt;
        j["spec_watcher"] = watch;
    }
    {
        SchedulerStats sch = scheduler_.stats();
        Json sj = Json::object();
        sj["enabled"] = sch.enabled;
        sj["interval_ms"] = sch.intervalMs;
        sj["count"] = static_cast<int>(sch.count);
        sj["enabled_count"] = static_cast<int>(sch.enabledCount);
        sj["ticks"] = static_cast<double>(sch.ticks);
        sj["fires"] = static_cast<double>(sch.fires);
        sj["skips"] = static_cast<double>(sch.skips);
        sj["errors"] = static_cast<double>(sch.errors);
        j["scheduler"] = sj;
    }
    if (runnerBridge_) j["runner"] = toJson(runnerBridge_->getStatus());
    if (engine_) {
        EngineStats s = engine_->stats();
        Json stats = Json::object();
        stats["queued"] = static_cast<int>(s.queued);
        stats["running"] = static_cast<int>(s.running);
        stats["completed"] = static_cast<int>(s.completed);
        stats["passed"] = static_cast<int>(s.passed);
        stats["failed"] = static_cast<int>(s.failed);
        stats["cancelled"] = static_cast<int>(s.cancelled);
        stats["errored"] = static_cast<int>(s.errored);
        stats["total_scenarios"] = s.totalScenarios;
        stats["passed_scenarios"] = s.passedScenarios;
        stats["failed_scenarios"] = s.failedScenarios;
        stats["skipped_scenarios"] = s.skippedScenarios;
        stats["total_duration"] = s.totalDuration;
        stats["history_size"] = static_cast<int>(engine_->count());
        j["stats"] = stats;
    }
    if (notifier_) {
        CallbackStats c = notifier_->stats();
        Json cb = Json::object();
        cb["enabled"] = notifier_->config().enabled;
        cb["pending"] = static_cast<int>(c.pending);
        cb["delivered"] = static_cast<int>(c.delivered);
        cb["failed"] = static_cast<int>(c.failed);
        cb["attempts"] = static_cast<int>(c.attempts);
        j["callbacks"] = cb;
    }
    if (httpServer_) {
        Json http = Json::object();
        http["active_connections"] = static_cast<int>(httpServer_->activeConnections());
        http["requests"] = static_cast<double>(httpServer_->requestCount());
        j["http"] = http;
    }
    if (wsServer_) {
        Json ws = Json::object();
        ws["connections"] = static_cast<int>(wsServer_->connectionCount());
        ws["messages_sent"] = static_cast<double>(wsServer_->messagesSent());
        j["websocket"] = ws;
    }
    j["events_published"] = static_cast<double>(EventBus::getInstance().publishedCount());
    Json auth = Json::object();
    auth["enabled"] = auth_.enabled();
    auth["protect_reads"] = auth_.config().protectReads;
    j["auth"] = auth;
    return j;
}

Json TestHub::projectsJson() const {
    std::lock_guard<std::mutex> lock(projectMutex_);
    Json arr = Json::array();
    for (const auto& p : config_.projects) {
        Json o = Json::object();
        o["id"] = p.id;
        o["name"] = p.name;
        o["dir"] = p.dir;
        o["concepts_dir"] = p.conceptsDir;
        o["current"] = p.id == config_.currentProjectId;
        arr.push(o);
    }
    Json j = Json::object();
    j["current"] = config_.currentProjectId;
    j["count"] = static_cast<int>(config_.projects.size());
    j["specs_dir"] = specs_.specsDir();
    j["projects"] = arr;
    return j;
}

bool TestHub::selectProject(const std::string& id, std::string& error) {
    std::lock_guard<std::mutex> lock(projectMutex_);
    const SpecProject* p = config_.findProject(id);
    if (!p) {
        error = "Project not found: " + id;
        return false;
    }
    if (p->id == config_.currentProjectId) return true;
    if (engine_) {
        EngineStats st = engine_->stats();
        if (st.queued + st.running > 0) {
            error = "Cannot switch project while tests are queued or running";
            return false;
        }
    }
    config_.currentProjectId = p->id;
    config_.applyCurrentProject();
    specs_.configure(config_.specsDir, config_.conceptsDir);
    auto conceptErrors = specs_.reloadConcepts();
    for (const auto& e : conceptErrors) {
        TH_LOG_WARN("specs", e.fileName + ":" + std::to_string(e.lineNumber) + ": " + e.message);
    }
    specWatcher_.acknowledge();
    TH_LOG_INFO("testhub", "Switched project to " + config_.currentProjectId + " (" + specs_.specsDir() + ")");
    publishEvent(EventType::PROJECT_CHANGED, "", {
        {"project_id", config_.currentProjectId},
        {"name", p->name},
        {"specs_dir", specs_.specsDir()},
        {"concepts", std::to_string(specs_.concepts().size())},
    });
    publishEvent(EventType::SPECS_RELOADED, "", {
        {"source", "project"},
        {"project_id", config_.currentProjectId},
        {"concepts", std::to_string(specs_.concepts().size())},
    });
    return true;
}

} // namespace testhub
