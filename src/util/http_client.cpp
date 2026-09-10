/*
 * TestHub - 极简 HTTP 客户端实现
 */

#include "http_client.h"
#include "string_util.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_handle = SOCKET;
#define TH_BAD_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using socket_handle = int;
#define TH_BAD_SOCKET (-1)
#endif

namespace testhub {

namespace {

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WinsockInit() { WSACleanup(); }
};
void ensureWinsock() { static WinsockInit init; }
void closeSock(socket_handle s) { closesocket(s); }
void setNonBlocking(socket_handle s, bool on) { u_long mode = on ? 1 : 0; ioctlsocket(s, FIONBIO, &mode); }
bool wouldBlock() { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
void ensureWinsock() {}
void closeSock(socket_handle s) { ::close(s); }
void setNonBlocking(socket_handle s, bool on) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return;
    fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
bool wouldBlock() { return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN; }
#endif

bool waitFor(socket_handle s, bool write, int timeoutMs) {
    if (timeoutMs < 0) timeoutMs = 0;
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv) > 0;
#else
    pollfd pfd{};
    pfd.fd = s;
    pfd.events = write ? POLLOUT : POLLIN;
    int r = poll(&pfd, 1, timeoutMs);
    return r > 0 && (pfd.revents & (write ? (POLLOUT | POLLERR | POLLHUP) : (POLLIN | POLLHUP | POLLERR)));
#endif
}

using Clock = std::chrono::steady_clock;

int remaining(Clock::time_point deadline) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return ms < 0 ? 0 : static_cast<int>(ms);
}

struct SocketGuard {
    socket_handle s;
    explicit SocketGuard(socket_handle sock) : s(sock) {}
    ~SocketGuard() { if (s != TH_BAD_SOCKET) closeSock(s); }
};

socket_handle connectWithTimeout(const ParsedUrl& url, Clock::time_point deadline, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &res);
    if (rc != 0 || !res) {
        error = "DNS lookup failed for " + url.host;
        return TH_BAD_SOCKET;
    }
    socket_handle result = TH_BAD_SOCKET;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        socket_handle s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == TH_BAD_SOCKET) continue;
        setNonBlocking(s, true);
        int c = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool connected = c == 0;
        if (!connected && wouldBlock()) {
            if (waitFor(s, true, remaining(deadline))) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
                connected = soerr == 0;
                if (!connected) error = "connect failed: " + std::string(std::strerror(soerr));
            } else {
                error = "connect timed out";
            }
        } else if (!connected) {
            error = "connect failed";
        }
        if (connected) {
            result = s;
            break;
        }
        closeSock(s);
    }
    freeaddrinfo(res);
    if (result == TH_BAD_SOCKET && error.empty()) error = "connect failed";
    return result;
}

bool sendAll(socket_handle s, const std::string& data, Clock::time_point deadline, std::string& error) {
    size_t off = 0;
    while (off < data.size()) {
        if (!waitFor(s, true, remaining(deadline))) {
            error = "send timed out";
            return false;
        }
#ifdef _WIN32
        int n = send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
#else
        ssize_t n = send(s, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#endif
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n < 0 && wouldBlock()) {
            continue;
        } else {
            error = "send failed";
            return false;
        }
    }
    return true;
}

/** 读到 deadline、连接关闭或 stop 判定为 true */
template <typename Pred>
bool readUntil(socket_handle s, std::string& buf, Clock::time_point deadline, Pred done, std::string& error) {
    char chunk[8192];
    while (!done(buf)) {
        if (!waitFor(s, false, remaining(deadline))) {
            error = "read timed out";
            return false;
        }
#ifdef _WIN32
        int n = recv(s, chunk, sizeof(chunk), 0);
#else
        ssize_t n = recv(s, chunk, sizeof(chunk), 0);
#endif
        if (n > 0) {
            buf.append(chunk, static_cast<size_t>(n));
        } else if (n == 0) {
            return done(buf);  // 对端关闭：由调用方决定是否已完整
        } else if (wouldBlock()) {
            continue;
        } else {
            error = "read failed";
            return false;
        }
    }
    return true;
}

bool parseChunked(const std::string& in, std::string& out) {
    size_t pos = 0;
    while (true) {
        size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos) return false;
        std::string sizeLine = in.substr(pos, eol - pos);
        size_t semi = sizeLine.find(';');
        if (semi != std::string::npos) sizeLine = sizeLine.substr(0, semi);
        size_t size = static_cast<size_t>(std::strtoul(sizeLine.c_str(), nullptr, 16));
        pos = eol + 2;
        if (size == 0) return true;
        if (pos + size > in.size()) return false;
        out.append(in, pos, size);
        pos += size + 2;
    }
}

} // namespace

bool HttpClient::parseUrl(const std::string& url, ParsedUrl& out, std::string* error) {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        if (error) *error = "URL must start with http://";
        return false;
    }
    out.scheme = StringUtil::toLower(url.substr(0, schemeEnd));
    if (out.scheme != "http") {
        if (error) *error = out.scheme == "https" ? "https is not supported (no TLS support)" : "Unsupported URL scheme '" + out.scheme + "'";
        return false;
    }
    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find_first_of("/?#", hostStart);
    std::string authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    if (authority.empty()) {
        if (error) *error = "URL has no host";
        return false;
    }
    // 忽略 userinfo
    size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    out.port = 80;
    if (!authority.empty() && authority.front() == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) {
            if (error) *error = "Malformed IPv6 host";
            return false;
        }
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') out.port = std::atoi(authority.c_str() + close + 2);
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            out.port = std::atoi(authority.c_str() + colon + 1);
        } else {
            out.host = authority;
        }
    }
    if (out.host.empty() || out.port <= 0 || out.port > 65535) {
        if (error) *error = "Invalid host or port";
        return false;
    }
    out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    size_t hash = out.path.find('#');
    if (hash != std::string::npos) out.path.erase(hash);
    if (out.path.empty() || out.path[0] != '/') out.path = "/" + out.path;
    return true;
}

HttpClientResponse HttpClient::request(const std::string& method, const std::string& url, const std::string& body,
                                       const std::map<std::string, std::string>& headers, int timeoutMs) {
    HttpClientResponse resp;
    ParsedUrl parsed;
    if (!parseUrl(url, parsed, &resp.error)) return resp;
    ensureWinsock();

    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs <= 0 ? 10000 : timeoutMs);
    socket_handle s = connectWithTimeout(parsed, deadline, resp.error);
    if (s == TH_BAD_SOCKET) return resp;
    SocketGuard guard(s);

    std::string req = method + " " + parsed.path + " HTTP/1.1\r\n";
    req += "Host: " + parsed.host + (parsed.port == 80 ? "" : ":" + std::to_string(parsed.port)) + "\r\n";
    req += "User-Agent: TestHub\r\nAccept: */*\r\nConnection: close\r\n";
    bool hasContentType = false;
    for (const auto& kv : headers) {
        if (StringUtil::toLower(kv.first) == "content-type") hasContentType = true;
        req += kv.first + ": " + kv.second + "\r\n";
    }
    if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
        if (!hasContentType && !body.empty()) req += "Content-Type: application/octet-stream\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n" + body;
    if (!sendAll(s, req, deadline, resp.error)) return resp;

    std::string buf;
    if (!readUntil(s, buf, deadline, [](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; }, resp.error)) {
        if (resp.error.empty()) resp.error = "connection closed before headers";
        return resp;
    }
    size_t headEnd = buf.find("\r\n\r\n");
    std::string head = buf.substr(0, headEnd);
    buf.erase(0, headEnd + 4);

    size_t lineEnd = head.find("\r\n");
    std::string statusLine = head.substr(0, lineEnd);
    if (statusLine.size() < 12 || statusLine.compare(0, 5, "HTTP/") != 0) {
        resp.error = "malformed status line";
        return resp;
    }
    resp.status = std::atoi(statusLine.substr(9, 3).c_str());
    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            resp.headers[StringUtil::toLower(StringUtil::trim(line.substr(0, colon)))] = StringUtil::trim(line.substr(colon + 1));
        }
        if (e == std::string::npos) break;
        pos = e + 2;
    }

    bool noBody = method == "HEAD" || resp.status == 204 || resp.status == 304 || (resp.status >= 100 && resp.status < 200);
    if (noBody) {
        resp.ok = true;
        return resp;
    }
    auto te = resp.headers.find("transfer-encoding");
    auto cl = resp.headers.find("content-length");
    if (te != resp.headers.end() && StringUtil::toLower(te->second).find("chunked") != std::string::npos) {
        std::string decoded;
        auto complete = [&decoded](const std::string& b) { decoded.clear(); return parseChunked(b, decoded); };
        if (!readUntil(s, buf, deadline, complete, resp.error)) {
            if (resp.error.empty()) resp.error = "incomplete chunked body";
            return resp;
        }
        resp.body = decoded;
    } else if (cl != resp.headers.end()) {
        size_t len = static_cast<size_t>(std::strtoull(cl->second.c_str(), nullptr, 10));
        if (!readUntil(s, buf, deadline, [len](const std::string& b) { return b.size() >= len; }, resp.error)) {
            if (resp.error.empty()) resp.error = "incomplete body";
            return resp;
        }
        resp.body = buf.substr(0, len);
    } else {
        // 无长度信息：读到对端关闭
        std::string ignored;
        readUntil(s, buf, deadline, [](const std::string&) { return false; }, ignored);
        resp.body = buf;
    }
    resp.ok = true;
    return resp;
}

HttpClientResponse HttpClient::post(const std::string& url, const std::string& body, const std::string& contentType,
                                    int timeoutMs, const std::map<std::string, std::string>& headers) {
    std::map<std::string, std::string> h = headers;
    h["Content-Type"] = contentType;
    return request("POST", url, body, h, timeoutMs);
}

} // namespace testhub
