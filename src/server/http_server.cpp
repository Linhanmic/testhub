/*
 * TestHub - HTTP 服务器实现
 */

#include "http_server.h"
#include "../util/string_util.h"

#include <iostream>
#include <sstream>
#include <regex>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

namespace testhub {

HttpServer::HttpServer(int port) : port_(port) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    if (running_) {
        return true;
    }

#ifdef _WIN32
    // 初始化 Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
#endif

    // 注册默认路由
    registerDefaultRoutes();

    // 启动监听线程
    running_ = true;
    listenThread_ = std::thread(&HttpServer::listenLoop, this);

    return true;
}

void HttpServer::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (listenThread_.joinable()) {
        listenThread_.join();
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

void HttpServer::addRoute(const std::string& method, const std::string& path, RequestHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    routes_.push_back({method, path, handler});
}

void HttpServer::listenLoop() {
    // 创建 socket
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        running_ = false;
        return;
    }

    // 设置 socket 选项
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    // 绑定地址
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(serverSocket, (struct sockaddr*)&address, sizeof(address)) < 0) {
#ifdef _WIN32
        closesocket(serverSocket);
#else
        close(serverSocket);
#endif
        running_ = false;
        return;
    }

    // 开始监听
    if (listen(serverSocket, 10) < 0) {
#ifdef _WIN32
        closesocket(serverSocket);
#else
        close(serverSocket);
#endif
        running_ = false;
        return;
    }

    std::cout << "TestHub server listening on port " << port_ << std::endl;

    // 接受连接
    while (running_) {
        struct sockaddr_in clientAddress;
        socklen_t clientAddressLength = sizeof(clientAddress);

        int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddress, &clientAddressLength);
        if (clientSocket < 0) {
            if (running_) {
                std::cerr << "Failed to accept connection" << std::endl;
            }
            continue;
        }

        // 处理连接（在新线程中）
        std::thread(&HttpServer::handleConnection, this, clientSocket).detach();
    }

#ifdef _WIN32
    closesocket(serverSocket);
#else
    close(serverSocket);
#endif
}

void HttpServer::handleConnection(int clientSocket) {
    // 读取请求
    char buffer[4096];
    std::string rawRequest;

    // 设置接收超时
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    while (true) {
        int bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytesRead <= 0) {
            // 超时或连接关闭
            break;
        }

        buffer[bytesRead] = '\0';
        rawRequest += buffer;

        // 检查是否读取完成（简单检查，实际应该更精确）
        if (rawRequest.find("\r\n\r\n") != std::string::npos) {
            break;
        }
        
        // 如果已经读取了一些数据，可能已经完成
        if (rawRequest.length() > 0 && bytesRead < sizeof(buffer) - 1) {
            break;
        }
    }

    if (rawRequest.empty()) {
#ifdef _WIN32
        closesocket(clientSocket);
#else
        close(clientSocket);
#endif
        return;
    }

    // 解析请求
    HttpRequest request = parseRequest(rawRequest);

    // 处理请求
    HttpResponse response = handleRequest(request);

    // 发送响应
    std::string serialized = serializeResponse(response);
    send(clientSocket, serialized.c_str(), serialized.length(), 0);

#ifdef _WIN32
    closesocket(clientSocket);
#else
    close(clientSocket);
#endif
}

HttpResponse HttpServer::handleRequest(const HttpRequest& request) {
    RequestHandler handler;
    std::map<std::string, std::string> params;

    // Debug output
    std::cout << "[DEBUG] Request: " << request.method << " " << request.path << std::endl;

    if (matchRoute(request.method, request.path, handler, params)) {
        HttpRequest req = request;
        req.queryParams.insert(params.begin(), params.end());
        return handler(req);
    }

    std::cout << "[DEBUG] No route found for: " << request.method << " " << request.path << std::endl;
    return HttpResponse::error(404, "Not Found");
}

bool HttpServer::matchRoute(const std::string& method, const std::string& path,
                           RequestHandler& handler, std::map<std::string, std::string>& params) {
    std::lock_guard<std::mutex> lock(mutex_);

    // First try exact match
    for (const auto& route : routes_) {
        if (route.method == method && route.pathPattern == path) {
            handler = route.handler;
            return true;
        }
    }

    // Then try pattern match with parameters
    for (const auto& route : routes_) {
        if (route.method != method) continue;

        std::string pattern = route.pathPattern;
        
        // Check if pattern has parameters
        if (pattern.find('{') == std::string::npos) continue;
        
        // Simple prefix matching for patterns like /api/v1/tests/{id}
        size_t paramStart = pattern.find('{');
        std::string prefix = pattern.substr(0, paramStart);
        
        if (path.find(prefix) == 0) {
            // Extract parameter value
            std::string paramValue = path.substr(prefix.length());
            
            // Find parameter name
            size_t paramEnd = pattern.find('}', paramStart);
            if (paramEnd != std::string::npos) {
                std::string paramName = pattern.substr(paramStart + 1, paramEnd - paramStart - 1);
                
                // Check if there's more after the parameter
                std::string remaining = pattern.substr(paramEnd + 1);
                if (remaining.empty() || path.find(remaining) != std::string::npos) {
                    params[paramName] = paramValue;
                    handler = route.handler;
                    return true;
                }
            }
        }
    }

    return false;
}

HttpRequest HttpServer::parseRequest(const std::string& rawRequest) {
    HttpRequest request;

    std::istringstream stream(rawRequest);
    std::string line;

    // 解析请求行
    if (std::getline(stream, line)) {
        std::istringstream lineStream(line);
        std::string pathWithQuery;
        lineStream >> request.method >> pathWithQuery;

        // 分离路径和查询参数
        size_t queryPos = pathWithQuery.find('?');
        if (queryPos != std::string::npos) {
            request.path = pathWithQuery.substr(0, queryPos);
            std::string queryString = pathWithQuery.substr(queryPos + 1);
            request.queryParams = parseQueryParams(queryString);
        } else {
            request.path = pathWithQuery;
        }
    }

    // 解析头部
    while (std::getline(stream, line) && line != "\r" && !line.empty()) {
        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);

            // 去除首尾空格
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t\r") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r") + 1);

            request.headers[key] = value;
        }
    }

    // 读取请求体
    std::string body;
    while (std::getline(stream, line)) {
        body += line + "\n";
    }
    if (!body.empty()) {
        body.pop_back(); // 移除最后一个换行符
    }
    request.body = body;

    return request;
}

std::string HttpServer::serializeResponse(const HttpResponse& response) {
    std::ostringstream oss;

    // 状态行
    oss << "HTTP/1.1 " << response.statusCode << " ";
    switch (response.statusCode) {
        case 200: oss << "OK"; break;
        case 201: oss << "Created"; break;
        case 204: oss << "No Content"; break;
        case 400: oss << "Bad Request"; break;
        case 404: oss << "Not Found"; break;
        case 500: oss << "Internal Server Error"; break;
        default: oss << "Unknown"; break;
    }
    oss << "\r\n";

    // 头部
    for (const auto& header : response.headers) {
        oss << header.first << ": " << header.second << "\r\n";
    }
    oss << "Content-Length: " << response.body.length() << "\r\n";
    oss << "\r\n";

    // 请求体
    oss << response.body;

    return oss.str();
}

std::map<std::string, std::string> HttpServer::parseQueryParams(const std::string& queryString) {
    std::map<std::string, std::string> params;

    std::istringstream stream(queryString);
    std::string pair;

    while (std::getline(stream, pair, '&')) {
        size_t eqPos = pair.find('=');
        if (eqPos != std::string::npos) {
            std::string key = pair.substr(0, eqPos);
            std::string value = pair.substr(eqPos + 1);
            params[key] = value;
        }
    }

    return params;
}

void HttpServer::registerDefaultRoutes() {
    // 健康检查
    get("/api/v1/health", [this](const HttpRequest& req) { return handleHealthCheck(req); });

    // 测试相关
    post("/api/v1/tests/run", [this](const HttpRequest& req) { return handleRunTest(req); });
    get("/api/v1/tests", [this](const HttpRequest& req) { return handleListTests(req); });
    get("/api/v1/tests/{id}", [this](const HttpRequest& req) { return handleGetTestStatus(req); });
    del("/api/v1/tests/{id}", [this](const HttpRequest& req) { return handleCancelTest(req); });
    get("/api/v1/tests/{id}/result", [this](const HttpRequest& req) { return handleGetTestResult(req); });

    // 规范相关
    get("/api/v1/specs", [this](const HttpRequest& req) { return handleListSpecs(req); });
    post("/api/v1/specs/validate", [this](const HttpRequest& req) { return handleValidateSpecs(req); });

    // Runner 相关
    get("/api/v1/runner/status", [this](const HttpRequest& req) { return handleGetRunnerStatus(req); });
    post("/api/v1/runner/restart", [this](const HttpRequest& req) { return handleRestartRunner(req); });
}

// ============================================================
// API 处理器实现
// ============================================================

HttpResponse HttpServer::handleRunTest(const HttpRequest& request) {
    if (!testQueue_) {
        return HttpResponse::error(500, "Test queue not initialized");
    }

    // TODO: 解析 JSON 请求体
    // 这里需要解析 JSON 获取测试参数

    TestRequest testRequest;
    testRequest.id = ""; // 自动生成
    testRequest.specFiles = {"specs/"}; // 默认值
    testRequest.priority = Priority::NORMAL;

    std::string testId = testQueue_->enqueue(testRequest);

    return HttpResponse::json(200, "{\"test_id\":\"" + testId + "\",\"status\":\"queued\",\"message\":\"Test submitted successfully\"}");
}

HttpResponse HttpServer::handleGetTestStatus(const HttpRequest& request) {
    // Extract test ID from path: /api/v1/tests/{id}
    std::string testId;
    std::string prefix = "/api/v1/tests/";
    if (request.path.find(prefix) == 0) {
        testId = request.path.substr(prefix.length());
    }
    
    if (testId.empty()) {
        return HttpResponse::error(400, "Missing test ID");
    }

    // TODO: Get status from test engine

    return HttpResponse::json(200, "{\"test_id\":\"" + testId + "\",\"state\":\"unknown\"}");
}

HttpResponse HttpServer::handleListTests(const HttpRequest& request) {
    if (!testQueue_) {
        return HttpResponse::error(500, "Test queue not initialized");
    }

    auto taskIds = testQueue_->getTaskIds();

    std::ostringstream oss;
    oss << "{\"tests\":[";
    for (size_t i = 0; i < taskIds.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << taskIds[i] << "\"";
    }
    oss << "]}";

    return HttpResponse::json(200, oss.str());
}

HttpResponse HttpServer::handleCancelTest(const HttpRequest& request) {
    if (!testQueue_) {
        return HttpResponse::error(500, "Test queue not initialized");
    }

    // Extract test ID from path
    std::string testId;
    std::string prefix = "/api/v1/tests/";
    if (request.path.find(prefix) == 0) {
        testId = request.path.substr(prefix.length());
    }
    
    if (testId.empty()) {
        return HttpResponse::error(400, "Missing test ID");
    }

    if (testQueue_->cancel(testId)) {
        return HttpResponse::json(200, "{\"message\":\"Test cancelled\"}");
    }

    return HttpResponse::error(404, "Test not found");
}

HttpResponse HttpServer::handleGetTestResult(const HttpRequest& request) {
    // Extract test ID from path: /api/v1/tests/{id}/result
    std::string testId;
    std::string prefix = "/api/v1/tests/";
    std::string suffix = "/result";
    
    if (request.path.find(prefix) == 0 && request.path.find(suffix) != std::string::npos) {
        size_t start = prefix.length();
        size_t end = request.path.find(suffix);
        testId = request.path.substr(start, end - start);
    }
    
    if (testId.empty()) {
        return HttpResponse::error(400, "Missing test ID");
    }

    // TODO: Get result from test engine

    return HttpResponse::json(200, "{\"test_id\":\"" + testId + "\",\"result\":null}");
}

HttpResponse HttpServer::handleListSpecs(const HttpRequest& request) {
    // TODO: 列出规范文件

    return HttpResponse::json(200, "{\"specs\":[]}");
}

HttpResponse HttpServer::handleValidateSpecs(const HttpRequest& request) {
    // TODO: 验证规范文件

    return HttpResponse::json(200, "{\"valid\":true,\"errors\":[]}");
}

HttpResponse HttpServer::handleGetRunnerStatus(const HttpRequest& request) {
    if (!runnerBridge_) {
        return HttpResponse::error(500, "Runner bridge not initialized");
    }

    RunnerStatus status = runnerBridge_->getStatus();

    std::ostringstream oss;
    oss << "{\"state\":\"";
    switch (status.state) {
        case RunnerState::DISCONNECTED: oss << "disconnected"; break;
        case RunnerState::CONNECTING: oss << "connecting"; break;
        case RunnerState::CONNECTED: oss << "connected"; break;
        case RunnerState::BUSY: oss << "busy"; break;
        case RunnerState::RUNNER_ERROR: oss << "error"; break;
    }
    oss << "\",\"language\":\"" << status.language << "\"";
    oss << ",\"pid\":" << status.pid;
    oss << "}";

    return HttpResponse::json(200, oss.str());
}

HttpResponse HttpServer::handleRestartRunner(const HttpRequest& request) {
    if (!runnerBridge_) {
        return HttpResponse::error(500, "Runner bridge not initialized");
    }

    if (runnerBridge_->restartRunner()) {
        return HttpResponse::json(200, "{\"message\":\"Runner restarted\"}");
    }

    return HttpResponse::error(500, "Failed to restart runner");
}

HttpResponse HttpServer::handleHealthCheck(const HttpRequest& request) {
    return HttpResponse::json(200, "{\"status\":\"ok\",\"version\":\"1.0.0\"}");
}

} // namespace testhub
