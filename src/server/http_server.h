/*
 * TestHub - HTTP 服务器
 * 零依赖的 HTTP/1.1 服务器：路由（含路径参数）、静态资源、keep-alive、
 * 请求体按 Content-Length 完整读取、连接线程池、WebSocket 升级钩子。
 */

#pragma once

#include "../util/json.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
using socket_t = SOCKET;
#else
using socket_t = int;
#endif

namespace testhub {

/**
 * HTTP 请求
 */
struct HttpRequest {
    std::string method;
    std::string path;          // 已解码的路径（不含查询串）
    std::string rawTarget;     // 原始请求目标
    std::string version = "HTTP/1.1";
    std::string body;
    std::map<std::string, std::string> headers;      // 键小写
    std::map<std::string, std::string> queryParams;  // 已解码
    std::map<std::string, std::string> pathParams;   // 路由参数
    std::string remoteAddress;

    std::string header(const std::string& name, const std::string& def = "") const {
        std::string key;
        for (char c : name) key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        auto it = headers.find(key);
        return it == headers.end() ? def : it->second;
    }
    std::string query(const std::string& name, const std::string& def = "") const {
        auto it = queryParams.find(name);
        return it == queryParams.end() ? def : it->second;
    }
    std::string param(const std::string& name, const std::string& def = "") const {
        auto it = pathParams.find(name);
        return it == pathParams.end() ? def : it->second;
    }
    bool keepAlive() const {
        std::string c = header("connection");
        for (auto& ch : c) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (c.find("close") != std::string::npos) return false;
        if (version == "HTTP/1.0") return c.find("keep-alive") != std::string::npos;
        return true;
    }
};

/**
 * HTTP 响应
 */
struct HttpResponse {
    int statusCode = 200;
    std::string body;
    std::map<std::string, std::string> headers;
    bool handled = true;  // false 表示由处理器接管了连接（如 WebSocket）

    HttpResponse() {
        headers["Content-Type"] = "application/json; charset=utf-8";
    }

    static HttpResponse json(int code, const Json& value) {
        HttpResponse r;
        r.statusCode = code;
        r.body = value.dump();
        return r;
    }
    static HttpResponse json(int code, const std::string& rawJson) {
        HttpResponse r;
        r.statusCode = code;
        r.body = rawJson;
        return r;
    }
    static HttpResponse text(int code, const std::string& text, const std::string& contentType = "text/plain; charset=utf-8") {
        HttpResponse r;
        r.statusCode = code;
        r.body = text;
        r.headers["Content-Type"] = contentType;
        return r;
    }
    static HttpResponse html(const std::string& html) { return text(200, html, "text/html; charset=utf-8"); }
    static HttpResponse error(int code, const std::string& message, const std::string& detail = "") {
        Json j = Json::object();
        j["error"] = message;
        j["status"] = code;
        if (!detail.empty()) j["detail"] = detail;
        return json(code, j);
    }
    static HttpResponse noContent() {
        HttpResponse r;
        r.statusCode = 204;
        r.headers.erase("Content-Type");
        return r;
    }
    static HttpResponse redirect(const std::string& location, int code = 302) {
        HttpResponse r;
        r.statusCode = code;
        r.headers["Location"] = location;
        r.headers.erase("Content-Type");
        return r;
    }
    static HttpResponse hijacked() {
        HttpResponse r;
        r.handled = false;
        return r;
    }

    static const char* reasonPhrase(int code);
};

using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

/**
 * WebSocket/原始连接接管处理器：返回 true 表示已接管 socket（服务器不再关闭）
 */
using UpgradeHandler = std::function<bool(socket_t socket, const HttpRequest& request)>;

/**
 * 服务器配置
 */
struct HttpServerConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
    int workerThreads = 8;
    int backlog = 64;
    int readTimeoutMs = 15000;
    int keepAliveTimeoutMs = 10000;
    size_t maxHeaderBytes = 64 * 1024;
    size_t maxBodyBytes = 16 * 1024 * 1024;
    bool enableCors = true;
    bool logRequests = true;
};

class HttpServer {
public:
    explicit HttpServer(int port = 8080);
    explicit HttpServer(const HttpServerConfig& config);
    ~HttpServer();

    bool start();
    void stop();
    bool isRunning() const { return running_; }
    int port() const { return boundPort_; }
    const std::string& lastError() const { return lastError_; }

    // 路由注册。路径支持 {param} 与末尾通配 * ；例如 /api/v1/tests/{id}/result、/static/*
    void addRoute(const std::string& method, const std::string& path, RequestHandler handler);
    void get(const std::string& path, RequestHandler handler) { addRoute("GET", path, std::move(handler)); }
    void post(const std::string& path, RequestHandler handler) { addRoute("POST", path, std::move(handler)); }
    void put(const std::string& path, RequestHandler handler) { addRoute("PUT", path, std::move(handler)); }
    void del(const std::string& path, RequestHandler handler) { addRoute("DELETE", path, std::move(handler)); }
    void patch(const std::string& path, RequestHandler handler) { addRoute("PATCH", path, std::move(handler)); }

    /**
     * 注册升级处理器（WebSocket）
     */
    void addUpgradeHandler(const std::string& path, UpgradeHandler handler);

    /**
     * 注册内存静态资源
     */
    void addStaticAsset(const std::string& path, std::string content, const std::string& contentType);

    /**
     * 注册磁盘静态目录：urlPrefix 下的请求映射到 dir；index.html 作为目录默认页
     */
    void serveDirectory(const std::string& urlPrefix, const std::string& dir);

    /**
     * 未匹配路由时的回退处理（例如 SPA 的 index.html）
     */
    void setFallback(RequestHandler handler) { fallback_ = std::move(handler); }

    /**
     * 直接处理一个请求（不经网络；用于单元测试）
     */
    HttpResponse dispatch(const HttpRequest& request);

    static std::string urlDecode(const std::string& s);
    static std::string mimeTypeFor(const std::string& path);
    static std::map<std::string, std::string> parseQueryString(const std::string& qs);
    static bool parseRequestHead(const std::string& head, HttpRequest& request, std::string& error);
    static std::string serialize(const HttpResponse& response, bool keepAlive);

    size_t activeConnections() const { return activeConnections_; }
    unsigned long long requestCount() const { return requestCount_; }

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;
        bool wildcard = false;
        RequestHandler handler;
    };
    struct StaticAsset {
        std::string content;
        std::string contentType;
        std::string etag;
    };

    HttpServerConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    socket_t listenSocket_;
    int boundPort_ = 0;
    std::string lastError_;

    std::thread acceptThread_;
    std::vector<std::thread> workers_;
    std::deque<std::pair<socket_t, std::string>> pending_;
    std::mutex pendingMutex_;
    std::condition_variable pendingCv_;

    mutable std::mutex routesMutex_;
    std::vector<Route> routes_;
    std::map<std::string, UpgradeHandler> upgrades_;
    std::map<std::string, StaticAsset> assets_;
    std::vector<std::pair<std::string, std::string>> directories_;
    RequestHandler fallback_;

    std::atomic<size_t> activeConnections_{0};
    std::atomic<unsigned long long> requestCount_{0};

    bool bindAndListen();
    void acceptLoop();
    void workerLoop();
    void handleConnection(socket_t client, const std::string& remote);
    bool readRequest(socket_t client, HttpRequest& request, int& errorCode, std::string& errorMessage, int firstByteTimeoutMs);
    bool matchRoute(const HttpRequest& request, RequestHandler& handler, std::map<std::string, std::string>& params, bool& methodMismatch) const;
    HttpResponse serveStatic(const HttpRequest& request, bool& found) const;
    static bool sendAll(socket_t s, const std::string& data);
    static void closeSocket(socket_t s);
    static std::vector<std::string> splitPath(const std::string& path);
};

} // namespace testhub
