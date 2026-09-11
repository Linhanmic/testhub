/*
 * TestHub - 持久化自动化测试系统
 * 主服务器类：装配 HTTP/WebSocket 服务器、规范仓库、Runner 桥接与执行引擎
 */

#pragma once

#include "engine/execution_engine.h"
#include "event/event_bus.h"
#include "model/types.h"
#include "notify/callback_notifier.h"
#include "runner/runner_bridge.h"
#include "server/auth.h"
#include "server/http_server.h"
#include "server/websocket_server.h"
#include "spec/spec_repository.h"
#include "spec/spec_watcher.h"
#include "util/json.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace testhub {

/**
 * TestHub 配置
 */
struct TestHubConfig {
    // 服务器
    std::string host = "0.0.0.0";
    int port = 8080;
    int httpWorkerThreads = 8;
    bool enableWebUi = true;
    std::string webDir;            // 若设置则从磁盘提供 Web UI（开发模式），否则使用内嵌资源
    std::string authToken;         // 非空时启用 Bearer Token 鉴权（写操作）
    bool authProtectReads = false; // 是否同时保护 GET 与 WebSocket

    // Runner
    std::string runnerLanguage = "mock";
    std::string runnerCommand;
    std::string projectPath;
    int runnerConnectionTimeout = 15000;
    int runnerRequestTimeout = 60000;
    bool autoRestartRunner = true;
    int runnerMaxRestarts = 5;
    int mockDelayMs = 0;
    int runnerPoolSize = 0;        // Runner 进程数；0 表示跟随 maxConcurrentTests

    /**
     * 实际生效的 Runner 池大小
     */
    int effectiveRunnerPoolSize() const {
        int n = runnerPoolSize > 0 ? runnerPoolSize : maxConcurrentTests;
        return n < 1 ? 1 : n;
    }

    // 执行
    int maxConcurrentTests = 1;
    int defaultTimeout = 300000;
    int stepTimeout = 60000;
    size_t historyLimit = 200;
    std::string resultsDir = "data/results";   // 结果持久化目录；空表示禁用
    std::map<std::string, std::string> environment;

    // 回调通知（callback_url）
    bool callbacksEnabled = true;
    int callbackTimeoutMs = 10000;
    int callbackMaxAttempts = 3;
    int callbackRetryBackoffMs = 1000;
    std::string publicBaseUrl;     // 回调载荷中 links 的前缀，如 http://ci.example.com:8080

    // 规范
    std::string specsDir = "specs";
    std::string conceptsDir;
    bool specsWatch = true;            // 轮询监控规范目录，自动重载概念并推送 specs.reloaded
    int specsWatchIntervalMs = 2000;

    // 日志
    std::string logLevel = "info";
    std::string logFile;
    bool logRequests = true;

    /**
     * 从 JSON 配置文件合并（存在的键覆盖当前值）
     */
    void applyJson(const Json& json);
    /** maskSecrets 为 true 时 auth_token 以 "***" 输出（用于 --print-config 与 GET /config） */
    Json toJson(bool maskSecrets = true) const;
};

/**
 * TestHub 服务器
 */
class TestHub {
public:
    TestHub();
    ~TestHub();

    TestHub(const TestHub&) = delete;
    TestHub& operator=(const TestHub&) = delete;

    bool initialize(const TestHubConfig& config);
    bool start();
    void stop();
    bool isRunning() const { return running_; }

    const TestHubConfig& getConfig() const { return config_; }
    HttpServer& getHttpServer() { return *httpServer_; }
    WebSocketServer& getWebSocketServer() { return *wsServer_; }
    ExecutionEngine& getEngine() { return *engine_; }
    RunnerBridge& getRunnerBridge() { return *runnerBridge_; }
    CallbackNotifier& getNotifier() { return *notifier_; }
    const AuthPolicy& getAuth() const { return auth_; }
    spec::SpecRepository& getSpecs() { return specs_; }
    spec::SpecWatcher& getSpecWatcher() { return specWatcher_; }
    EventBus& getEventBus() { return EventBus::getInstance(); }

    /**
     * 实际监听端口（port=0 时由系统分配）
     */
    int boundPort() const { return httpServer_ ? httpServer_->port() : 0; }

    /**
     * Runner / 回调使用的本机 API 根 URL（public_base_url，否则 http://127.0.0.1:<boundPort>）
     */
    std::string runtimeBaseUrl() const;

    /**
     * 服务器状态摘要（供 /api/v1/status 与 Dashboard 使用）
     */
    Json statusJson() const;

    static const char* version() { return "1.1.0"; }

private:
    TestHubConfig config_;
    spec::SpecRepository specs_;
    spec::SpecWatcher specWatcher_{specs_};
    std::unique_ptr<HttpServer> httpServer_;
    std::unique_ptr<WebSocketServer> wsServer_;
    std::unique_ptr<RunnerBridge> runnerBridge_;
    std::unique_ptr<ExecutionEngine> engine_;
    std::unique_ptr<CallbackNotifier> notifier_;
    AuthPolicy auth_;

    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::string eventHandlerId_;
    TimePoint startedAt_;

    void registerApiRoutes();
    void registerWebUi();
};

} // namespace testhub
