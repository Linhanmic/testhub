/*
 * TestHub - HTTP 服务器实现
 */

#include "http_server.h"
#include "../util/file_util.h"
#include "../util/logger.h"
#include "../util/string_util.h"
#include "../util/time_util.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define TH_INVALID_SOCKET INVALID_SOCKET
using ssize_t = long;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#define TH_INVALID_SOCKET (-1)
#endif

namespace testhub {

namespace {

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WinsockInit() { WSACleanup(); }
};
void ensureWinsock() { static WinsockInit init; }
#else
void ensureWinsock() {}
#endif

bool waitReadable(socket_t s, int timeoutMs) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(0, &set, nullptr, nullptr, &tv) > 0;
#else
    pollfd pfd{};
    pfd.fd = s;
    pfd.events = POLLIN;
    int r = poll(&pfd, 1, timeoutMs);
    return r > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR));
#endif
}

int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

} // namespace

// ============================================================
// HttpResponse
// ============================================================

const char* HttpResponse::reasonPhrase(int code) {
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 418: return "I'm a teapot";
        case 422: return "Unprocessable Entity";
        case 426: return "Upgrade Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
    }
}

// ============================================================
// 构造 / 生命周期
// ============================================================

HttpServer::HttpServer(int port) : listenSocket_(TH_INVALID_SOCKET) {
    config_.port = port;
}

HttpServer::HttpServer(const HttpServerConfig& config) : config_(config), listenSocket_(TH_INVALID_SOCKET) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::closeSocket(socket_t s) {
    if (s == TH_INVALID_SOCKET) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

bool HttpServer::bindAndListen() {
    ensureWinsock();
    listenSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ == TH_INVALID_SOCKET) {
        lastError_ = "socket() failed: " + std::to_string(lastSocketError());
        return false;
    }
    int opt = 1;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));
    if (config_.host.empty() || config_.host == "0.0.0.0" || config_.host == "*") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
        lastError_ = "Invalid host address: " + config_.host;
        closeSocket(listenSocket_);
        listenSocket_ = TH_INVALID_SOCKET;
        return false;
    }

    if (bind(listenSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        lastError_ = "bind() to port " + std::to_string(config_.port) + " failed: " + std::to_string(lastSocketError());
        closeSocket(listenSocket_);
        listenSocket_ = TH_INVALID_SOCKET;
        return false;
    }
    if (listen(listenSocket_, config_.backlog) < 0) {
        lastError_ = "listen() failed: " + std::to_string(lastSocketError());
        closeSocket(listenSocket_);
        listenSocket_ = TH_INVALID_SOCKET;
        return false;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (getsockname(listenSocket_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        boundPort_ = ntohs(bound.sin_port);
    } else {
        boundPort_ = config_.port;
    }
    return true;
}

bool HttpServer::start() {
    if (running_) return true;
    if (!bindAndListen()) {
        TH_LOG_ERROR("http", lastError_);
        return false;
    }
    stopping_ = false;
    running_ = true;
    int workers = std::max(1, config_.workerThreads);
    for (int i = 0; i < workers; ++i) workers_.emplace_back(&HttpServer::workerLoop, this);
    acceptThread_ = std::thread(&HttpServer::acceptLoop, this);
    TH_LOG_INFO("http", "Listening on " + config_.host + ":" + std::to_string(boundPort_));
    return true;
}

void HttpServer::stop() {
    if (!running_) return;
    stopping_ = true;
    running_ = false;
    if (listenSocket_ != TH_INVALID_SOCKET) {
#ifdef _WIN32
        shutdown(listenSocket_, SD_BOTH);
#else
        shutdown(listenSocket_, SHUT_RDWR);
#endif
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    closeSocket(listenSocket_);
    listenSocket_ = TH_INVALID_SOCKET;

    pendingCv_.notify_all();
    for (auto& w : workers_) if (w.joinable()) w.join();
    workers_.clear();
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        for (auto& p : pending_) closeSocket(p.first);
        pending_.clear();
    }
    TH_LOG_INFO("http", "Server stopped");
}

void HttpServer::acceptLoop() {
    while (!stopping_) {
        if (!waitReadable(listenSocket_, 200)) continue;
        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        socket_t client = accept(listenSocket_, reinterpret_cast<sockaddr*>(&clientAddr), &len);
        if (client == TH_INVALID_SOCKET) {
            if (stopping_) break;
            continue;
        }
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
        std::string remote = std::string(ip) + ":" + std::to_string(ntohs(clientAddr.sin_port));
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pending_.emplace_back(client, remote);
        }
        pendingCv_.notify_one();
    }
}

void HttpServer::workerLoop() {
    while (true) {
        std::pair<socket_t, std::string> item;
        {
            std::unique_lock<std::mutex> lock(pendingMutex_);
            pendingCv_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) {
                if (stopping_) return;
                continue;
            }
            item = pending_.front();
            pending_.pop_front();
        }
        ++activeConnections_;
        try {
            handleConnection(item.first, item.second);
        } catch (const std::exception& e) {
            TH_LOG_ERROR("http", std::string("Connection handler crashed: ") + e.what());
            closeSocket(item.first);
        } catch (...) {
            closeSocket(item.first);
        }
        --activeConnections_;
    }
}

bool HttpServer::sendAll(socket_t s, const std::string& data) {
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

// ============================================================
// 请求解析
// ============================================================

std::string HttpServer::urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::map<std::string, std::string> HttpServer::parseQueryString(const std::string& qs) {
    std::map<std::string, std::string> params;
    size_t start = 0;
    while (start <= qs.size()) {
        size_t end = qs.find('&', start);
        if (end == std::string::npos) end = qs.size();
        std::string pair = qs.substr(start, end - start);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) params[urlDecode(pair)] = "";
            else params[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        }
        if (end == qs.size()) break;
        start = end + 1;
    }
    return params;
}

bool HttpServer::parseRequestHead(const std::string& head, HttpRequest& request, std::string& error) {
    std::istringstream stream(head);
    std::string line;
    if (!std::getline(stream, line)) {
        error = "Empty request";
        return false;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream first(line);
    std::string target;
    if (!(first >> request.method >> target)) {
        error = "Malformed request line";
        return false;
    }
    std::string version;
    if (first >> version) request.version = version;
    if (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") {
        error = "Unsupported HTTP version";
        return false;
    }
    request.rawTarget = target;
    size_t q = target.find('?');
    if (q != std::string::npos) {
        request.path = urlDecode(target.substr(0, q));
        request.queryParams = parseQueryString(target.substr(q + 1));
    } else {
        request.path = urlDecode(target);
    }
    if (request.path.empty() || request.path[0] != '/') {
        if (StringUtil::startsWith(request.path, "http://") || StringUtil::startsWith(request.path, "https://")) {
            size_t slash = request.path.find('/', request.path.find("//") + 2);
            request.path = slash == std::string::npos ? "/" : request.path.substr(slash);
        } else if (request.path != "*") {
            error = "Invalid request target";
            return false;
        }
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            error = "Malformed header line";
            return false;
        }
        std::string key = StringUtil::toLower(StringUtil::trim(line.substr(0, colon)));
        std::string value = StringUtil::trim(line.substr(colon + 1));
        auto it = request.headers.find(key);
        if (it != request.headers.end() && key != "content-length") it->second += ", " + value;
        else request.headers[key] = value;
    }
    return true;
}

bool HttpServer::readRequest(socket_t client, HttpRequest& request, std::string& buffer, int& errorCode,
                             std::string& errorMessage, int firstByteTimeoutMs) {
    char chunk[8192];
    size_t headerEnd = buffer.find("\r\n\r\n");
    bool firstByte = buffer.empty();

    while (headerEnd == std::string::npos) {
        int timeout = firstByte ? firstByteTimeoutMs : config_.readTimeoutMs;
        if (!waitReadable(client, timeout)) {
            errorCode = firstByte ? 0 : 408;  // 空闲超时不响应
            errorMessage = "Request timeout";
            return false;
        }
#ifdef _WIN32
        int n = recv(client, chunk, sizeof(chunk), 0);
#else
        ssize_t n = recv(client, chunk, sizeof(chunk), 0);
#endif
        if (n <= 0) {
            errorCode = 0;
            errorMessage = "Connection closed";
            return false;
        }
        firstByte = false;
        buffer.append(chunk, static_cast<size_t>(n));
        headerEnd = buffer.find("\r\n\r\n");
        if (headerEnd == std::string::npos && buffer.size() > config_.maxHeaderBytes) {
            errorCode = 431;
            errorMessage = "Request headers too large";
            return false;
        }
    }

    std::string head = buffer.substr(0, headerEnd + 2);
    std::string rest = buffer.substr(headerEnd + 4);
    buffer.clear();
    std::string parseError;
    if (!parseRequestHead(head, request, parseError)) {
        errorCode = 400;
        errorMessage = parseError;
        return false;
    }

    size_t contentLength = 0;
    std::string cl = request.header("content-length");
    if (!cl.empty()) {
        try {
            long long v = std::stoll(cl);
            if (v < 0) throw std::out_of_range("negative");
            contentLength = static_cast<size_t>(v);
        } catch (...) {
            errorCode = 400;
            errorMessage = "Invalid Content-Length";
            return false;
        }
    } else if (StringUtil::toLower(request.header("transfer-encoding")).find("chunked") != std::string::npos) {
        errorCode = 501;
        errorMessage = "Chunked transfer encoding is not supported";
        return false;
    }
    if (contentLength > config_.maxBodyBytes) {
        errorCode = 413;
        errorMessage = "Request body too large";
        return false;
    }

    if (StringUtil::toLower(request.header("expect")) == "100-continue" && contentLength > rest.size()) {
        sendAll(client, "HTTP/1.1 100 Continue\r\n\r\n");
    }

    request.body = rest;
    while (request.body.size() < contentLength) {
        if (!waitReadable(client, config_.readTimeoutMs)) {
            errorCode = 408;
            errorMessage = "Timed out reading request body";
            return false;
        }
#ifdef _WIN32
        int n = recv(client, chunk, sizeof(chunk), 0);
#else
        ssize_t n = recv(client, chunk, sizeof(chunk), 0);
#endif
        if (n <= 0) {
            errorCode = 400;
            errorMessage = "Connection closed while reading body";
            return false;
        }
        request.body.append(chunk, static_cast<size_t>(n));
    }
    if (request.body.size() > contentLength) {
        // 管线化：多余的字节属于下一个请求
        buffer = request.body.substr(contentLength);
        request.body.resize(contentLength);
    }
    return true;
}

std::string HttpServer::serialize(const HttpResponse& response, bool keepAlive) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << response.statusCode << ' ' << HttpResponse::reasonPhrase(response.statusCode) << "\r\n";
    bool hasDate = false, hasServer = false, hasLength = false, hasConnection = false;
    for (const auto& h : response.headers) {
        std::string lower = StringUtil::toLower(h.first);
        if (lower == "date") hasDate = true;
        if (lower == "server") hasServer = true;
        if (lower == "content-length") hasLength = true;
        if (lower == "connection") hasConnection = true;
        oss << h.first << ": " << h.second << "\r\n";
    }
    if (!hasDate) oss << "Date: " << TimeUtil::httpDate() << "\r\n";
    if (!hasServer) oss << "Server: TestHub\r\n";
    if (!hasLength && response.statusCode != 204 && response.statusCode != 304) {
        oss << "Content-Length: " << response.body.size() << "\r\n";
    }
    if (!hasConnection) oss << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n";
    oss << "\r\n";
    if (response.statusCode != 204 && response.statusCode != 304) oss << response.body;
    return oss.str();
}

// ============================================================
// 连接处理
// ============================================================

void HttpServer::handleConnection(socket_t client, const std::string& remote) {
    int opt = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&opt), sizeof(opt));

    bool first = true;
    std::string leftover;  // 上一个请求之后已读到的字节（HTTP 管线化）
    while (!stopping_) {
        HttpRequest request;
        request.remoteAddress = remote;
        int errorCode = 0;
        std::string errorMessage;
        int firstTimeout = first ? config_.readTimeoutMs : config_.keepAliveTimeoutMs;
        first = false;
        if (!readRequest(client, request, leftover, errorCode, errorMessage, firstTimeout)) {
            if (errorCode != 0) {
                HttpResponse err = HttpResponse::error(errorCode, errorMessage);
                sendAll(client, serialize(err, false));
            }
            break;
        }
        ++requestCount_;

        // WebSocket 升级
        if (StringUtil::toLower(request.header("upgrade")) == "websocket") {
            UpgradeHandler upgrade;
            {
                std::lock_guard<std::mutex> lock(routesMutex_);
                auto it = upgrades_.find(request.path);
                if (it != upgrades_.end()) upgrade = it->second;
            }
            if (upgrade) {
                if (config_.logRequests) TH_LOG_DEBUG("http", remote + " UPGRADE " + request.path);
                if (upgrade(client, request)) return;  // socket 已被接管
                break;
            }
            HttpResponse err = HttpResponse::error(404, "No WebSocket endpoint at " + request.path);
            sendAll(client, serialize(err, false));
            break;
        }

        auto started = std::chrono::steady_clock::now();
        HttpResponse response = dispatch(request);
        if (!response.handled) return;
        bool keepAlive = request.keepAlive() && !stopping_;
        if (!sendAll(client, serialize(response, keepAlive))) break;
        if (config_.logRequests) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            TH_LOG_DEBUG("http", remote + " " + request.method + " " + request.rawTarget + " -> " +
                                 std::to_string(response.statusCode) + " (" + std::to_string(ms) + "ms)");
        }
        if (!keepAlive) break;
    }
    closeSocket(client);
}

// ============================================================
// 路由
// ============================================================

std::vector<std::string> HttpServer::splitPath(const std::string& path) {
    std::vector<std::string> segments;
    std::string current;
    for (char c : path) {
        if (c == '/') {
            if (!current.empty()) segments.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) segments.push_back(current);
    return segments;
}

void HttpServer::addRoute(const std::string& method, const std::string& path, RequestHandler handler) {
    Route route;
    route.method = StringUtil::toUpper(method);
    route.segments = splitPath(path);
    if (!route.segments.empty() && route.segments.back() == "*") {
        route.wildcard = true;
        route.segments.pop_back();
    }
    route.handler = std::move(handler);
    std::lock_guard<std::mutex> lock(routesMutex_);
    routes_.push_back(std::move(route));
}

void HttpServer::addUpgradeHandler(const std::string& path, UpgradeHandler handler) {
    std::lock_guard<std::mutex> lock(routesMutex_);
    upgrades_[path] = std::move(handler);
}

void HttpServer::addStaticAsset(const std::string& path, std::string content, const std::string& contentType) {
    StaticAsset asset;
    asset.etag = "\"" + std::to_string(std::hash<std::string>{}(content)) + "\"";
    asset.content = std::move(content);
    asset.contentType = contentType;
    std::lock_guard<std::mutex> lock(routesMutex_);
    assets_[path] = std::move(asset);
}

void HttpServer::serveDirectory(const std::string& urlPrefix, const std::string& dir) {
    std::string prefix = urlPrefix;
    if (prefix.empty() || prefix.back() != '/') prefix.push_back('/');
    std::lock_guard<std::mutex> lock(routesMutex_);
    directories_.emplace_back(prefix, dir);
}

bool HttpServer::matchRoute(const HttpRequest& request, RequestHandler& handler,
                            std::map<std::string, std::string>& params, bool& methodMismatch) const {
    std::vector<std::string> segments = splitPath(request.path);
    std::lock_guard<std::mutex> lock(routesMutex_);
    methodMismatch = false;
    const Route* best = nullptr;
    int bestScore = -1;
    std::map<std::string, std::string> bestParams;

    for (const auto& route : routes_) {
        if (route.wildcard) {
            if (segments.size() < route.segments.size()) continue;
        } else if (segments.size() != route.segments.size()) {
            continue;
        }
        std::map<std::string, std::string> p;
        int score = 0;
        bool ok = true;
        for (size_t i = 0; i < route.segments.size(); ++i) {
            const std::string& pat = route.segments[i];
            if (pat.size() >= 2 && pat.front() == '{' && pat.back() == '}') {
                p[pat.substr(1, pat.size() - 2)] = segments[i];
            } else if (pat == segments[i]) {
                score += 2;
            } else {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        if (route.wildcard) {
            std::string rest;
            for (size_t i = route.segments.size(); i < segments.size(); ++i) {
                if (!rest.empty()) rest.push_back('/');
                rest += segments[i];
            }
            p["*"] = rest;
        } else {
            score += 1;
        }
        if (route.method != request.method) {
            methodMismatch = true;
            continue;
        }
        if (score > bestScore) {
            best = &route;
            bestScore = score;
            bestParams = p;
        }
    }
    if (!best) return false;
    handler = best->handler;
    params = bestParams;
    methodMismatch = false;
    return true;
}

HttpResponse HttpServer::serveStatic(const HttpRequest& request, bool& found) const {
    found = false;
    std::string path = request.path;
    std::lock_guard<std::mutex> lock(routesMutex_);
    auto it = assets_.find(path);
    if (it == assets_.end() && (path.empty() || path.back() == '/')) it = assets_.find(path + "index.html");
    if (it != assets_.end()) {
        found = true;
        if (request.header("if-none-match") == it->second.etag) {
            HttpResponse r;
            r.statusCode = 304;
            r.headers.erase("Content-Type");
            r.headers["ETag"] = it->second.etag;
            return r;
        }
        HttpResponse r = HttpResponse::text(200, it->second.content, it->second.contentType);
        r.headers["ETag"] = it->second.etag;
        r.headers["Cache-Control"] = "no-cache";
        return r;
    }
    for (const auto& d : directories_) {
        if (!StringUtil::startsWith(path, d.first) && path + "/" != d.first) continue;
        std::string rel = path.size() >= d.first.size() ? path.substr(d.first.size()) : "";
        if (rel.find("..") != std::string::npos) {
            found = true;
            return HttpResponse::error(403, "Forbidden");
        }
        std::string full = FileUtil::joinPath(d.second, rel);
        if (FileUtil::directoryExists(full)) full = FileUtil::joinPath(full, "index.html");
        if (!FileUtil::fileExists(full)) continue;
        found = true;
        HttpResponse r = HttpResponse::text(200, FileUtil::readFile(full), mimeTypeFor(full));
        r.headers["Cache-Control"] = "no-cache";
        return r;
    }
    return HttpResponse();
}

HttpResponse HttpServer::dispatch(const HttpRequest& input) {
    HttpRequest request = input;
    HttpResponse response;
    try {
        if (config_.enableCors && request.method == "OPTIONS") {
            response = HttpResponse::noContent();
            response.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, PATCH, DELETE, OPTIONS";
            response.headers["Access-Control-Allow-Headers"] = request.header("access-control-request-headers", "Content-Type, Authorization");
            response.headers["Access-Control-Max-Age"] = "600";
        } else {
            RequestHandler handler;
            std::map<std::string, std::string> params;
            bool methodMismatch = false;
            bool matched = matchRoute(request, handler, params, methodMismatch);
            if (!matched && request.method == "HEAD") {
                // HEAD 复用 GET 处理器（响应体在末尾被清空）
                HttpRequest asGet = request;
                asGet.method = "GET";
                matched = matchRoute(asGet, handler, params, methodMismatch);
            }
            if (matched) {
                request.pathParams = params;
                response = handler(request);
            } else if (methodMismatch) {
                response = HttpResponse::error(405, "Method Not Allowed");
            } else {
                bool found = false;
                if (request.method == "GET" || request.method == "HEAD") {
                    response = serveStatic(request, found);
                }
                if (!found) {
                    if (fallback_) response = fallback_(request);
                    else response = HttpResponse::error(404, "Not Found", request.method + " " + request.path);
                }
            }
        }
    } catch (const std::invalid_argument& e) {
        response = HttpResponse::error(400, e.what());
    } catch (const Json::ParseError& e) {
        response = HttpResponse::error(400, "Invalid JSON", e.what());
    } catch (const std::exception& e) {
        TH_LOG_ERROR("http", std::string("Handler exception: ") + e.what());
        response = HttpResponse::error(500, "Internal Server Error", e.what());
    }
    if (config_.enableCors && response.handled) {
        response.headers["Access-Control-Allow-Origin"] = "*";
    }
    if (request.method == "HEAD") response.body.clear();
    return response;
}

std::string HttpServer::mimeTypeFor(const std::string& path) {
    std::string ext = StringUtil::toLower(FileUtil::getExtension(path));
    static const std::map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".js", "application/javascript; charset=utf-8"},
        {".mjs", "application/javascript; charset=utf-8"}, {".json", "application/json; charset=utf-8"},
        {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif", "image/gif"}, {".ico", "image/x-icon"}, {".webp", "image/webp"},
        {".woff", "font/woff"}, {".woff2", "font/woff2"}, {".ttf", "font/ttf"},
        {".txt", "text/plain; charset=utf-8"}, {".md", "text/markdown; charset=utf-8"},
        {".spec", "text/plain; charset=utf-8"}, {".cpt", "text/plain; charset=utf-8"},
        {".xml", "application/xml"}, {".pdf", "application/pdf"}, {".map", "application/json"}
    };
    auto it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

} // namespace testhub
