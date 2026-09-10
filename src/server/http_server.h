/*
 * TestHub - HTTP 服务器
 * 提供 RESTful API 接口
 */

#pragma once

#include "../model/types.h"
#include "../engine/test_queue.h"
#include "../runner/runner_bridge.h"

#include <string>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

namespace testhub {

/**
 * HTTP 请求
 */
struct HttpRequest {
    std::string method;      // GET, POST, PUT, DELETE
    std::string path;        // 请求路径
    std::string body;        // 请求体
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> queryParams;
};

/**
 * HTTP 响应
 */
struct HttpResponse {
    int statusCode = 200;
    std::string body;
    std::map<std::string, std::string> headers;
    
    HttpResponse() {
        headers["Content-Type"] = "application/json";
        headers["Access-Control-Allow-Origin"] = "*";
    }
    
    static HttpResponse json(int code, const std::string& jsonBody) {
        HttpResponse response;
        response.statusCode = code;
        response.body = jsonBody;
        return response;
    }
    
    static HttpResponse error(int code, const std::string& message) {
        HttpResponse response;
        response.statusCode = code;
        response.body = "{\"error\":\"" + message + "\"}";
        return response;
    }
};

/**
 * 请求处理器类型
 */
using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

/**
 * 路由定义
 */
struct Route {
    std::string method;
    std::string pathPattern;
    RequestHandler handler;
};

/**
 * HTTP 服务器
 * 提供 RESTful API 接口
 */
class HttpServer {
public:
    HttpServer(int port = 8080);
    ~HttpServer();

    /**
     * 启动服务器
     */
    bool start();

    /**
     * 停止服务器
     */
    void stop();

    /**
     * 检查服务器是否运行中
     */
    bool isRunning() const { return running_; }

    /**
     * 注册路由
     */
    void addRoute(const std::string& method, const std::string& path, RequestHandler handler);

    /**
     * 注册 GET 路由
     */
    void get(const std::string& path, RequestHandler handler) {
        addRoute("GET", path, handler);
    }

    /**
     * 注册 POST 路由
     */
    void post(const std::string& path, RequestHandler handler) {
        addRoute("POST", path, handler);
    }

    /**
     * 注册 PUT 路由
     */
    void put(const std::string& path, RequestHandler handler) {
        addRoute("PUT", path, handler);
    }

    /**
     * 注册 DELETE 路由
     */
    void del(const std::string& path, RequestHandler handler) {
        addRoute("DELETE", path, handler);
    }

    /**
     * 设置测试队列
     */
    void setTestQueue(TestQueue* queue) { testQueue_ = queue; }

    /**
     * 设置 Runner 桥接
     */
    void setRunnerBridge(RunnerBridge* bridge) { runnerBridge_ = bridge; }

private:
    // 端口
    int port_;
    
    // 是否运行中
    std::atomic<bool> running_{false};
    
    // 监听线程
    std::thread listenThread_;
    
    // 路由列表
    std::vector<Route> routes_;
    
    // 互斥锁
    std::mutex mutex_;
    
    // 测试队列
    TestQueue* testQueue_ = nullptr;
    
    // Runner 桥接
    RunnerBridge* runnerBridge_ = nullptr;

    /**
     * 监听循环
     */
    void listenLoop();

    /**
     * 处理连接
     */
    void handleConnection(int clientSocket);

    /**
     * 处理请求
     */
    HttpResponse handleRequest(const HttpRequest& request);

    /**
     * 匹配路由
     */
    bool matchRoute(const std::string& method, const std::string& path, 
                    RequestHandler& handler, std::map<std::string, std::string>& params);

    /**
     * 解析 HTTP 请求
     */
    HttpRequest parseRequest(const std::string& rawRequest);

    /**
     * 序列化 HTTP 响应
     */
    std::string serializeResponse(const HttpResponse& response);

    /**
     * 解析查询参数
     */
    std::map<std::string, std::string> parseQueryParams(const std::string& queryString);

    /**
     * 注册默认路由
     */
    void registerDefaultRoutes();

    // ============================================================
    // API 处理器
    // ============================================================

    /**
     * POST /api/v1/tests/run
     * 提交测试运行请求
     */
    HttpResponse handleRunTest(const HttpRequest& request);

    /**
     * GET /api/v1/tests/{id}
     * 查询测试状态
     */
    HttpResponse handleGetTestStatus(const HttpRequest& request);

    /**
     * GET /api/v1/tests
     * 列出所有测试
     */
    HttpResponse handleListTests(const HttpRequest& request);

    /**
     * DELETE /api/v1/tests/{id}
     * 取消测试
     */
    HttpResponse handleCancelTest(const HttpRequest& request);

    /**
     * GET /api/v1/tests/{id}/result
     * 获取测试结果
     */
    HttpResponse handleGetTestResult(const HttpRequest& request);

    /**
     * GET /api/v1/specs
     * 列出规范文件
     */
    HttpResponse handleListSpecs(const HttpRequest& request);

    /**
     * POST /api/v1/specs/validate
     * 验证规范文件
     */
    HttpResponse handleValidateSpecs(const HttpRequest& request);

    /**
     * GET /api/v1/runner/status
     * 获取 Runner 状态
     */
    HttpResponse handleGetRunnerStatus(const HttpRequest& request);

    /**
     * POST /api/v1/runner/restart
     * 重启 Runner
     */
    HttpResponse handleRestartRunner(const HttpRequest& request);

    /**
     * GET /api/v1/health
     * 健康检查
     */
    HttpResponse handleHealthCheck(const HttpRequest& request);
};

} // namespace testhub
