/*
 * TestHub - 主服务器实现
 */

#include "testhub.h"
#include "model/json_convert.h"
#include "util/file_util.h"
#include "util/logger.h"

namespace testhub {

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

    const Json& runner = json["runner"];
    if (runner["language"].isString()) runnerLanguage = runner["language"].asString();
    if (runner["command"].isString()) runnerCommand = runner["command"].asString();
    if (runner["project_path"].isString()) projectPath = runner["project_path"].asString();
    if (runner["connection_timeout"].isNumber()) runnerConnectionTimeout = runner["connection_timeout"].asInt();
    if (runner["request_timeout"].isNumber()) runnerRequestTimeout = runner["request_timeout"].asInt();
    if (runner["auto_restart"].isBool()) autoRestartRunner = runner["auto_restart"].asBool();
    if (runner["max_restarts"].isNumber()) runnerMaxRestarts = runner["max_restarts"].asInt();
    if (runner["mock_delay_ms"].isNumber()) mockDelayMs = runner["mock_delay_ms"].asInt();

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

    const Json& logging = json["logging"];
    if (logging["level"].isString()) logLevel = logging["level"].asString();
    if (logging["file"].isString()) logFile = logging["file"].asString();
    if (logging["requests"].isBool()) logRequests = logging["requests"].asBool();
}

Json TestHubConfig::toJson() const {
    Json j = Json::object();
    Json server = Json::object();
    server["host"] = host;
    server["port"] = port;
    server["worker_threads"] = httpWorkerThreads;
    server["web_ui"] = enableWebUi;
    if (!webDir.empty()) server["web_dir"] = webDir;
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
    j["runner"] = runner;

    Json execution = Json::object();
    execution["max_concurrent_tests"] = maxConcurrentTests;
    execution["default_timeout"] = defaultTimeout;
    execution["step_timeout"] = stepTimeout;
    execution["history_limit"] = static_cast<int>(historyLimit);
    execution["results_dir"] = resultsDir;
    execution["environment"] = testhub::toJson(environment);
    j["execution"] = execution;

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

    Logger::getInstance().setLevel(logLevelFromString(config_.logLevel));
    if (!config_.logFile.empty() && !Logger::getInstance().setFile(config_.logFile)) {
        TH_LOG_WARN("testhub", "Cannot open log file: " + config_.logFile);
    }

    specs_.configure(config_.specsDir, config_.conceptsDir);
    auto conceptErrors = specs_.reloadConcepts();
    for (const auto& e : conceptErrors) {
        TH_LOG_WARN("specs", e.fileName + ":" + std::to_string(e.lineNumber) + ": " + e.message);
    }

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

    notifier_ = std::make_unique<CallbackNotifier>(*engine_);
    CallbackConfig callbackConfig;
    callbackConfig.enabled = config_.callbacksEnabled;
    callbackConfig.timeoutMs = config_.callbackTimeoutMs;
    callbackConfig.maxAttempts = config_.callbackMaxAttempts;
    callbackConfig.retryBackoffMs = config_.callbackRetryBackoffMs;
    callbackConfig.publicBaseUrl = config_.publicBaseUrl;
    notifier_->configure(callbackConfig);

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

bool TestHub::start() {
    if (!initialized_) {
        TH_LOG_ERROR("testhub", "TestHub not initialized");
        return false;
    }
    if (running_) return true;

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
    if (!runnerBridge_->start(runnerConfig)) {
        TH_LOG_WARN("testhub", "Runner failed to start; tests will error until the runner is available");
    }

    if (!httpServer_->start()) {
        TH_LOG_ERROR("testhub", "Failed to start HTTP server: " + httpServer_->lastError());
        return false;
    }
    notifier_->start();
    engine_->start();
    startedAt_ = TimeUtil::now();
    running_ = true;

    TH_LOG_INFO("testhub", std::string("TestHub ") + version() + " started on http://" +
                           (config_.host == "0.0.0.0" ? "localhost" : config_.host) + ":" + std::to_string(boundPort()));
    TH_LOG_INFO("testhub", "Specs directory: " + specs_.specsDir());
    TH_LOG_INFO("testhub", "Runner: " + config_.runnerLanguage + (config_.runnerCommand.empty() ? "" : " (" + config_.runnerCommand + ")"));
    publishEvent(EventType::SERVER_STARTED, "", {{"version", version()}, {"port", std::to_string(boundPort())}});
    return true;
}

void TestHub::stop() {
    if (!running_) return;
    running_ = false;
    publishEvent(EventType::SERVER_STOPPING, "", {});
    EventBus::getInstance().waitForIdle(1000);

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
    j["started_at"] = TimeUtil::toIso8601(startedAt_);
    j["uptime_seconds"] = running_ ? std::chrono::duration<double>(TimeUtil::now() - startedAt_).count() : 0.0;
    j["specs_dir"] = specs_.specsDir();
    j["concepts"] = static_cast<int>(specs_.concepts().size());
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
    return j;
}

} // namespace testhub
