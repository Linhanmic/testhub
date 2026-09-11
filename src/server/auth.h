/*
 * TestHub - Bearer Token 鉴权策略
 *
 * 规则（token 为空时全部放行）：
 *   - 非 /api/ 与非 /ws 路径（静态 UI）始终放行；/api/v1/health 始终放行
 *   - 写操作（POST/PUT/PATCH/DELETE）必须携带 token
 *   - 读操作（GET/HEAD）与 WebSocket 仅在 protectReads 时要求 token
 *   - 凭据来源：Authorization: Bearer <token>、X-Auth-Token: <token>；
 *     GET/HEAD/WebSocket 还接受查询参数 access_token（浏览器下载链接与 WebSocket 无法自定义头）
 */

#pragma once

#include "http_server.h"
#include "../util/string_util.h"

#include <string>

namespace testhub {

struct AuthConfig {
    std::string token;          // 空 = 关闭鉴权
    bool protectReads = false;  // 是否连 GET/WS 也要求 token
};

class AuthPolicy {
public:
    AuthPolicy() = default;
    explicit AuthPolicy(AuthConfig config) : config_(std::move(config)) {}

    void configure(AuthConfig config) { config_ = std::move(config); }
    const AuthConfig& config() const { return config_; }
    bool enabled() const { return !config_.token.empty(); }

    static bool isReadMethod(const std::string& method) { return method == "GET" || method == "HEAD"; }

    static bool isProtectedPath(const std::string& path) {
        return StringUtil::startsWith(path, "/api/") || path == "/ws" || StringUtil::startsWith(path, "/ws/");
    }

    /** 请求是否需要 token（与是否携带无关） */
    bool requiresAuth(const HttpRequest& request) const {
        if (!enabled()) return false;
        if (!isProtectedPath(request.path)) return false;
        if (request.path == "/api/v1/health") return false;
        if (request.method == "OPTIONS") return false;
        bool isWebSocket = StringUtil::toLower(request.header("upgrade")) == "websocket";
        if ((isReadMethod(request.method) || isWebSocket) && !config_.protectReads) return false;
        return true;
    }

    /** 从请求中提取凭据；未提供返回空串 */
    static std::string extractToken(const HttpRequest& request) {
        std::string auth = request.header("authorization");
        if (!auth.empty()) {
            std::string lower = StringUtil::toLower(auth);
            if (StringUtil::startsWith(lower, "bearer ")) return StringUtil::trim(auth.substr(7));
            return "";
        }
        std::string x = request.header("x-auth-token");
        if (!x.empty()) return StringUtil::trim(x);
        bool isWebSocket = StringUtil::toLower(request.header("upgrade")) == "websocket";
        if (isReadMethod(request.method) || isWebSocket) return request.query("access_token");
        return "";
    }

    /** 常量时间比较，避免通过响应时间逐字节猜测 */
    static bool constantTimeEquals(const std::string& a, const std::string& b) {
        unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
        size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        return diff == 0;
    }

    /** 作为 HttpServer::RequestFilter 使用 */
    bool authorize(const HttpRequest& request, HttpResponse& denied) const {
        if (!requiresAuth(request)) return true;
        std::string provided = extractToken(request);
        if (!provided.empty() && constantTimeEquals(provided, config_.token)) return true;
        denied = HttpResponse::error(401, provided.empty() ? "Authentication required" : "Invalid token");
        denied.headers["WWW-Authenticate"] = "Bearer realm=\"TestHub\"";
        return false;
    }

private:
    AuthConfig config_;
};

} // namespace testhub
