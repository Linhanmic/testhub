/*
 * TestHub - WebSocket 服务器实现
 */

#include "websocket_server.h"
#include "../model/json_convert.h"
#include "../util/base64.h"
#include "../util/logger.h"
#include "../util/sha1.h"
#include "../util/string_util.h"

#include <cstring>

#ifdef _WIN32
#include <ws2tcpip.h>
using ssize_t = long;
#else
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace testhub {

namespace {

void closeSock(socket_t s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

bool recvExact(socket_t s, std::string& out, size_t n, std::atomic<bool>& open) {
    out.clear();
    out.reserve(n);
    char buf[4096];
    while (out.size() < n) {
        if (!open) return false;
#ifndef _WIN32
        pollfd pfd{};
        pfd.fd = s;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 500);
        if (pr == 0) continue;
        if (pr < 0) return false;
#endif
        size_t want = std::min(sizeof(buf), n - out.size());
#ifdef _WIN32
        int r = recv(s, buf, static_cast<int>(want), 0);
#else
        ssize_t r = recv(s, buf, want, 0);
#endif
        if (r <= 0) return false;
        out.append(buf, static_cast<size_t>(r));
    }
    return true;
}

bool sendAllRaw(socket_t s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
#ifdef _WIN32
        int n = send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
#else
        ssize_t n = send(s, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

WebSocketServer::WebSocketServer() = default;

WebSocketServer::~WebSocketServer() { stop(); }

std::string WebSocketServer::computeAcceptKey(const std::string& clientKey) {
    auto digest = Sha1::digest(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return Base64::encode(digest.data(), digest.size());
}

std::string WebSocketServer::encodeFrame(const std::string& payload, uint8_t opcode) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
    }
    frame += payload;
    return frame;
}

void WebSocketServer::attach(HttpServer& server, const std::string& path) {
    server.addUpgradeHandler(path, [this](socket_t s, const HttpRequest& req) { return handleUpgrade(s, req); });
}

bool WebSocketServer::handleUpgrade(socket_t socket, const HttpRequest& request) {
    std::string key = request.header("sec-websocket-key");
    std::string version = request.header("sec-websocket-version");
    if (key.empty() || version != "13") {
        HttpResponse bad = HttpResponse::error(400, "Invalid WebSocket handshake");
        bad.headers["Sec-WebSocket-Version"] = "13";
        sendAllRaw(socket, HttpServer::serialize(bad, false));
        return false;
    }
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + computeAcceptKey(key) + "\r\n\r\n";
    if (!sendAllRaw(socket, response)) return false;

    auto conn = std::make_shared<Connection>();
    conn->id = nextId_++;
    conn->socket = socket;
    conn->testIdFilter = request.query("test_id");
    if (!request.query("events").empty()) {
        for (auto& p : StringUtil::split(request.query("events"), ",")) {
            std::string t = StringUtil::trim(p);
            if (!t.empty()) conn->eventPatterns.push_back(t);
        }
    }
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_[conn->id] = conn;
    }
    TH_LOG_INFO("ws", "Client connected #" + std::to_string(conn->id) + " from " + request.remoteAddress +
                      " (total " + std::to_string(connectionCount()) + ")");

    Json hello = Json::object();
    hello["type"] = "welcome";
    hello["connection_id"] = conn->id;
    hello["server_time"] = TimeUtil::toIso8601(TimeUtil::now());
    sendRaw(*conn, encodeFrame(hello.dump()));

    reapFinishedReaders();
    {
        // 持锁赋值：若读线程立刻结束并进入 closeConnection，会等到句柄就位后再决定如何回收自己
        std::lock_guard<std::mutex> lock(conn->readerMutex);
        conn->reader = std::thread(&WebSocketServer::readerLoop, this, conn);
    }
    return true;
}

void WebSocketServer::reapFinishedReaders() {
    std::vector<std::thread> done;
    {
        std::lock_guard<std::mutex> lock(finishedMutex_);
        done.swap(finishedReaders_);
    }
    for (auto& t : done) if (t.joinable()) t.join();
}

bool WebSocketServer::matchesPattern(const std::string& pattern, const std::string& type) {
    if (pattern == "*" || pattern == type) return true;
    if (pattern.size() >= 2 && pattern.back() == '*' && pattern[pattern.size() - 2] == '.') {
        return type.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
    }
    return false;
}

bool WebSocketServer::matchesFilter(Connection& conn, const Event& event) {
    std::lock_guard<std::mutex> lock(conn.filterMutex);
    if (!conn.testIdFilter.empty() && !event.testId.empty() && event.testId != conn.testIdFilter) return false;
    if (conn.eventPatterns.empty()) return true;
    for (const auto& p : conn.eventPatterns) if (matchesPattern(p, event.type)) return true;
    return false;
}

void WebSocketServer::readerLoop(std::shared_ptr<Connection> conn) {
    std::string fragmented;
    uint8_t fragmentOpcode = 0;
    while (conn->open && !stopping_) {
        std::string header;
        if (!recvExact(conn->socket, header, 2, conn->open)) break;
        uint8_t b0 = static_cast<uint8_t>(header[0]);
        uint8_t b1 = static_cast<uint8_t>(header[1]);
        bool fin = (b0 & 0x80) != 0;
        uint8_t opcode = b0 & 0x0F;
        bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7F;
        if (len == 126) {
            std::string ext;
            if (!recvExact(conn->socket, ext, 2, conn->open)) break;
            len = (static_cast<uint64_t>(static_cast<uint8_t>(ext[0])) << 8) | static_cast<uint8_t>(ext[1]);
        } else if (len == 127) {
            std::string ext;
            if (!recvExact(conn->socket, ext, 8, conn->open)) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(ext[i]);
        }
        if (len > 4 * 1024 * 1024) break;  // 客户端消息过大
        std::string mask;
        if (masked && !recvExact(conn->socket, mask, 4, conn->open)) break;
        std::string payload;
        if (len > 0 && !recvExact(conn->socket, payload, static_cast<size_t>(len), conn->open)) break;
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }

        switch (opcode) {
            case 0x8: {  // close
                closeConnection(conn, true);
                return;
            }
            case 0x9: {  // ping
                sendRaw(*conn, encodeFrame(payload, 0xA));
                continue;
            }
            case 0xA:  // pong
                continue;
            case 0x0:  // continuation
                fragmented += payload;
                if (!fin) continue;
                payload = fragmented;
                opcode = fragmentOpcode;
                fragmented.clear();
                break;
            case 0x1: case 0x2:
                if (!fin) {
                    fragmented = payload;
                    fragmentOpcode = opcode;
                    continue;
                }
                break;
            default:
                continue;
        }
        if (opcode != 0x1) continue;

        std::string err;
        Json msg = Json::tryParse(payload, &err);
        if (!msg.isObject()) continue;
        std::string action = msg["action"].asString(msg["type"].asString());
        if (action == "subscribe") {
            std::lock_guard<std::mutex> lock(conn->filterMutex);
            conn->eventPatterns.clear();
            for (const auto& e : msg["events"].asArray()) if (e.isString()) conn->eventPatterns.push_back(e.asString());
            if (msg["test_id"].isString()) conn->testIdFilter = msg["test_id"].asString();
            Json ack = Json::object();
            ack["type"] = "subscribed";
            ack["events"] = msg["events"];
            ack["test_id"] = conn->testIdFilter;
            sendRaw(*conn, encodeFrame(ack.dump()));
        } else if (action == "unsubscribe") {
            std::lock_guard<std::mutex> lock(conn->filterMutex);
            conn->eventPatterns.clear();
            conn->testIdFilter.clear();
        } else if (action == "ping") {
            Json pong = Json::object();
            pong["type"] = "pong";
            pong["server_time"] = TimeUtil::toIso8601(TimeUtil::now());
            sendRaw(*conn, encodeFrame(pong.dump()));
        } else if (messageHandler_) {
            messageHandler_(conn->id, msg);
        }
    }
    closeConnection(conn, false);
}

bool WebSocketServer::sendRaw(Connection& conn, const std::string& frame) {
    if (!conn.open) return false;
    std::lock_guard<std::mutex> lock(conn.writeMutex);
    if (!sendAllRaw(conn.socket, frame)) {
        conn.open = false;
        return false;
    }
    ++messagesSent_;
    return true;
}

void WebSocketServer::closeConnection(std::shared_ptr<Connection> conn, bool sendClose) {
    bool wasOpen = conn->open.exchange(false);
    if (wasOpen && sendClose) {
        std::lock_guard<std::mutex> lock(conn->writeMutex);
        std::string payload = "\x03\xE8";  // 1000 normal closure
        sendAllRaw(conn->socket, encodeFrame(payload, 0x8));
    }
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        removed = connections_.erase(conn->id) > 0;
    }
    if (removed) {
#ifdef _WIN32
        shutdown(conn->socket, SD_BOTH);
#else
        shutdown(conn->socket, SHUT_RDWR);
#endif
        closeSock(conn->socket);
        TH_LOG_INFO("ws", "Client disconnected #" + std::to_string(conn->id) + " (total " + std::to_string(connectionCount()) + ")");
        // 读线程自己结束时不能 join 自己：交给 finished 列表，由下次升级或 stop() 回收
        std::lock_guard<std::mutex> readerLock(conn->readerMutex);
        if (conn->reader.joinable()) {
            if (conn->reader.get_id() == std::this_thread::get_id()) {
                std::lock_guard<std::mutex> lock(finishedMutex_);
                finishedReaders_.push_back(std::move(conn->reader));
            } else {
                conn->reader.join();
            }
        }
    }
}

void WebSocketServer::broadcastEvent(const Event& event) {
    std::vector<std::shared_ptr<Connection>> targets;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        if (connections_.empty()) return;
        for (auto& kv : connections_) targets.push_back(kv.second);
    }
    Json json = toJson(event);
    json["type"] = "event";
    std::string frame = encodeFrame(json.dump());
    for (auto& c : targets) {
        if (!matchesFilter(*c, event)) continue;
        if (!sendRaw(*c, frame)) closeConnection(c, false);
    }
}

void WebSocketServer::broadcast(const Json& message) {
    std::vector<std::shared_ptr<Connection>> targets;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto& kv : connections_) targets.push_back(kv.second);
    }
    std::string frame = encodeFrame(message.dump());
    for (auto& c : targets) {
        if (!sendRaw(*c, frame)) closeConnection(c, false);
    }
}

bool WebSocketServer::send(int connectionId, const Json& message) {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = connections_.find(connectionId);
        if (it == connections_.end()) return false;
        conn = it->second;
    }
    return sendRaw(*conn, encodeFrame(message.dump()));
}

size_t WebSocketServer::connectionCount() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return connections_.size();
}

void WebSocketServer::stop() {
    stopping_ = true;
    std::vector<std::shared_ptr<Connection>> conns;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto& kv : connections_) conns.push_back(kv.second);
    }
    for (auto& c : conns) closeConnection(c, true);
    reapFinishedReaders();
}

} // namespace testhub
