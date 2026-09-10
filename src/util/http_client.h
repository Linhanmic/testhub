/*
 * TestHub - 极简 HTTP/1.1 客户端（零依赖，仅 http://）
 * 用于回调通知等出站请求：阻塞式，带连接/读写超时，不支持 TLS、重定向与分块编码之外的高级特性。
 */

#pragma once

#include <map>
#include <string>

namespace testhub {

struct HttpClientResponse {
    bool ok = false;          // 收到了完整的 HTTP 响应（无论状态码）
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;  // 小写键
    std::string error;        // ok == false 时的原因
};

struct ParsedUrl {
    std::string scheme;
    std::string host;
    int port = 80;
    std::string path = "/";   // 含查询串
};

class HttpClient {
public:
    /** 解析 http://host[:port]/path?query；不支持的 scheme 返回 false */
    static bool parseUrl(const std::string& url, ParsedUrl& out, std::string* error = nullptr);

    static HttpClientResponse request(const std::string& method, const std::string& url, const std::string& body = "",
                                      const std::map<std::string, std::string>& headers = {}, int timeoutMs = 10000);

    static HttpClientResponse post(const std::string& url, const std::string& body,
                                   const std::string& contentType = "application/json; charset=utf-8",
                                   int timeoutMs = 10000, const std::map<std::string, std::string>& headers = {});
};

} // namespace testhub
