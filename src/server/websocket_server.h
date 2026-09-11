/*
 * TestHub - WebSocket 服务器（RFC 6455 服务端子集）
 * 挂载在 HttpServer 的升级钩子上，向所有客户端广播事件；
 * 支持客户端订阅过滤：{"action":"subscribe","events":["test.*"],"test_id":"..."}
 */

#pragma once

#include "http_server.h"
#include "../model/types.h"
#include "../util/json.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace testhub {

class WebSocketServer {
public:
    using MessageHandler = std::function<void(int connectionId, const Json& message)>;

    WebSocketServer();
    ~WebSocketServer();

    /**
     * 挂载到 HTTP 服务器的指定路径
     */
    void attach(HttpServer& server, const std::string& path);

    /**
     * 广播事件（按客户端订阅过滤）
     */
    void broadcastEvent(const Event& event);

    /**
     * 广播任意 JSON
     */
    void broadcast(const Json& message);

    /**
     * 向单个连接发送
     */
    bool send(int connectionId, const Json& message);

    void setMessageHandler(MessageHandler handler) { messageHandler_ = std::move(handler); }

    size_t connectionCount() const;
    unsigned long long messagesSent() const { return messagesSent_; }

    void stop();

    // ---- 帮助函数（公开以便测试）----
    static std::string computeAcceptKey(const std::string& clientKey);
    static std::string encodeFrame(const std::string& payload, uint8_t opcode = 0x1);

private:
    struct Connection {
        int id;
        socket_t socket;
        std::thread reader;
        std::mutex readerMutex;  // 保护 reader 句柄：读线程可能在 handleUpgrade 完成赋值前就已退出
        std::mutex writeMutex;
        std::atomic<bool> open{true};
        std::vector<std::string> eventPatterns;  // 为空表示全部
        std::string testIdFilter;
        std::mutex filterMutex;
    };

    bool handleUpgrade(socket_t socket, const HttpRequest& request);
    void readerLoop(std::shared_ptr<Connection> conn);
    bool sendRaw(Connection& conn, const std::string& frame);
    void closeConnection(std::shared_ptr<Connection> conn, bool sendClose);
    void reapFinishedReaders();
    bool matchesFilter(Connection& conn, const Event& event);
    static bool matchesPattern(const std::string& pattern, const std::string& type);

    mutable std::mutex connectionsMutex_;
    std::map<int, std::shared_ptr<Connection>> connections_;
    std::atomic<int> nextId_{1};
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned long long> messagesSent_{0};
    MessageHandler messageHandler_;
    std::vector<std::thread> finishedReaders_;
    std::mutex finishedMutex_;
};

} // namespace testhub
