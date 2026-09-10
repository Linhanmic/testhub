#include "test_framework.h"
#include "server/http_server.h"

using namespace testhub;

namespace {
HttpRequest makeRequest(const std::string& method, const std::string& path) {
    HttpRequest r;
    r.method = method;
    r.path = path;
    r.rawTarget = path;
    return r;
}
}

TEST_CASE("http: parseRequestHead extracts method, path, query and headers") {
    HttpRequest req;
    std::string err;
    std::string head = "GET /api/v1/tests?state=running&name=a%20b&flag HTTP/1.1\r\n"
                       "Host: localhost:8080\r\n"
                       "Content-Type:  application/json \r\n"
                       "X-Custom: value\r\n";
    REQUIRE(HttpServer::parseRequestHead(head, req, err));
    CHECK_EQ(req.method, std::string("GET"));
    CHECK_EQ(req.path, std::string("/api/v1/tests"));
    CHECK_EQ(req.version, std::string("HTTP/1.1"));
    CHECK_EQ(req.query("state"), std::string("running"));
    CHECK_EQ(req.query("name"), std::string("a b"));
    CHECK(req.queryParams.count("flag") == 1);
    CHECK_EQ(req.header("content-type"), std::string("application/json"));
    CHECK_EQ(req.header("X-CUSTOM"), std::string("value"));
    CHECK_EQ(req.header("missing", "def"), std::string("def"));
}

TEST_CASE("http: malformed request line is rejected") {
    HttpRequest req;
    std::string err;
    CHECK(!HttpServer::parseRequestHead("GARBAGE\r\n", req, err));
    CHECK(!err.empty());
    CHECK(!HttpServer::parseRequestHead("GET /x FTP/9\r\n", req, err));
}

TEST_CASE("http: urlDecode and query parsing") {
    CHECK_EQ(HttpServer::urlDecode("a%20b+c%2F"), std::string("a b c/"));
    CHECK_EQ(HttpServer::urlDecode("%E4%B8%AD"), std::string("中"));
    CHECK_EQ(HttpServer::urlDecode("bad%zz"), std::string("bad%zz"));
    auto q = HttpServer::parseQueryString("a=1&b=&c&d=x%3Dy");
    CHECK_EQ(q["a"], std::string("1"));
    CHECK_EQ(q["b"], std::string(""));
    CHECK(q.count("c") == 1);
    CHECK_EQ(q["d"], std::string("x=y"));
}

TEST_CASE("http: routing with path params, wildcard, 404 and 405") {
    HttpServerConfig cfg;
    cfg.enableCors = true;
    HttpServer server(cfg);
    server.get("/api/v1/tests", [](const HttpRequest&) { return HttpResponse::text(200, "list"); });
    server.get("/api/v1/tests/{id}", [](const HttpRequest& r) { return HttpResponse::text(200, "id=" + r.param("id")); });
    server.get("/api/v1/tests/{id}/result", [](const HttpRequest& r) { return HttpResponse::text(200, "result=" + r.param("id")); });
    server.get("/api/v1/specs/validate", [](const HttpRequest&) { return HttpResponse::text(200, "validate"); });
    server.get("/api/v1/specs/*", [](const HttpRequest& r) { return HttpResponse::text(200, "spec=" + r.param("*")); });
    server.post("/api/v1/tests", [](const HttpRequest& r) { return HttpResponse::text(201, "body=" + r.body); });

    CHECK_EQ(server.dispatch(makeRequest("GET", "/api/v1/tests")).body, std::string("list"));
    CHECK_EQ(server.dispatch(makeRequest("GET", "/api/v1/tests/abc")).body, std::string("id=abc"));
    CHECK_EQ(server.dispatch(makeRequest("GET", "/api/v1/tests/abc/result")).body, std::string("result=abc"));
    // 静态段优先于通配
    CHECK_EQ(server.dispatch(makeRequest("GET", "/api/v1/specs/validate")).body, std::string("validate"));
    CHECK_EQ(server.dispatch(makeRequest("GET", "/api/v1/specs/dir/a.spec")).body, std::string("spec=dir/a.spec"));
    // 尾部斜杠容忍
    CHECK_EQ(server.dispatch(makeRequest("GET", "/api/v1/tests/")).body, std::string("list"));

    HttpRequest post = makeRequest("POST", "/api/v1/tests");
    post.body = "{}";
    HttpResponse created = server.dispatch(post);
    CHECK_EQ(created.statusCode, 201);
    CHECK_EQ(created.body, std::string("body={}"));
    CHECK_EQ(created.headers["Access-Control-Allow-Origin"], std::string("*"));

    CHECK_EQ(server.dispatch(makeRequest("GET", "/nope")).statusCode, 404);
    CHECK_EQ(server.dispatch(makeRequest("DELETE", "/api/v1/tests/abc")).statusCode, 405);
    CHECK_EQ(server.dispatch(makeRequest("OPTIONS", "/api/v1/tests")).statusCode, 204);

    HttpResponse head = server.dispatch(makeRequest("HEAD", "/api/v1/tests"));
    CHECK_EQ(head.statusCode, 200);
    CHECK(head.body.empty());
}

TEST_CASE("http: handler exceptions map to status codes") {
    HttpServer server(HttpServerConfig{});
    server.get("/bad", [](const HttpRequest&) -> HttpResponse { throw std::invalid_argument("bad input"); });
    server.get("/json", [](const HttpRequest&) -> HttpResponse { Json::parse("{oops"); return HttpResponse(); });
    server.get("/boom", [](const HttpRequest&) -> HttpResponse { throw std::runtime_error("boom"); });
    CHECK_EQ(server.dispatch(makeRequest("GET", "/bad")).statusCode, 400);
    CHECK(server.dispatch(makeRequest("GET", "/bad")).body.find("bad input") != std::string::npos);
    CHECK_EQ(server.dispatch(makeRequest("GET", "/json")).statusCode, 400);
    CHECK_EQ(server.dispatch(makeRequest("GET", "/boom")).statusCode, 500);
}

TEST_CASE("http: static assets with etag and SPA fallback") {
    HttpServer server(HttpServerConfig{});
    server.addStaticAsset("/index.html", "<html>hi</html>", "text/html; charset=utf-8");
    server.addStaticAsset("/app.js", "console.log(1)", "application/javascript");
    server.setFallback([](const HttpRequest& r) {
        if (r.method == "GET") return HttpResponse::html("<spa/>");
        return HttpResponse::error(404, "nf");
    });

    HttpResponse idx = server.dispatch(makeRequest("GET", "/"));
    CHECK_EQ(idx.statusCode, 200);
    CHECK_EQ(idx.body, std::string("<html>hi</html>"));
    CHECK(!idx.headers["ETag"].empty());

    HttpRequest cached = makeRequest("GET", "/app.js");
    cached.headers["if-none-match"] = idx.headers["ETag"];
    // ETag 属于另一个资源 -> 200
    CHECK_EQ(server.dispatch(cached).statusCode, 200);
    HttpResponse js = server.dispatch(makeRequest("GET", "/app.js"));
    cached.headers["if-none-match"] = js.headers["ETag"];
    CHECK_EQ(server.dispatch(cached).statusCode, 304);

    CHECK_EQ(server.dispatch(makeRequest("GET", "/dashboard")).body, std::string("<spa/>"));
    CHECK_EQ(server.dispatch(makeRequest("POST", "/dashboard")).statusCode, 404);
}

TEST_CASE("http: response serialization") {
    HttpResponse r = HttpResponse::text(200, "hello");
    std::string wire = HttpServer::serialize(r, true);
    CHECK(wire.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(wire.find("Content-Length: 5\r\n") != std::string::npos);
    CHECK(wire.find("Connection: keep-alive\r\n") != std::string::npos);
    CHECK(wire.find("\r\n\r\nhello") != std::string::npos);
    std::string closing = HttpServer::serialize(HttpResponse::noContent(), false);
    CHECK(closing.rfind("HTTP/1.1 204 No Content\r\n", 0) == 0);
    CHECK(closing.find("Connection: close\r\n") != std::string::npos);
    CHECK_EQ(std::string(HttpResponse::reasonPhrase(418)), std::string("I'm a teapot"));
    CHECK_EQ(HttpServer::mimeTypeFor("x/y.CSS"), std::string("text/css; charset=utf-8"));
    CHECK_EQ(HttpServer::mimeTypeFor("noext"), std::string("application/octet-stream"));
}

#include "server/auth.h"

TEST_CASE("auth: policy decides which requests need a token and where it may come from") {
    AuthPolicy off;
    HttpResponse denied;
    CHECK(!off.enabled());
    CHECK(off.authorize(makeRequest("DELETE", "/api/v1/tests"), denied));

    AuthPolicy writes(AuthConfig{"s3cret", false});
    CHECK(writes.authorize(makeRequest("GET", "/api/v1/tests"), denied));          // 读操作放行
    CHECK(writes.authorize(makeRequest("GET", "/"), denied));                      // 静态 UI 放行
    CHECK(writes.authorize(makeRequest("OPTIONS", "/api/v1/tests"), denied));      // 预检放行
    HttpRequest post = makeRequest("POST", "/api/v1/tests");
    CHECK(!writes.authorize(post, denied));
    CHECK_EQ(denied.statusCode, 401);
    CHECK_EQ(denied.headers["WWW-Authenticate"], std::string("Bearer realm=\"TestHub\""));
    CHECK(denied.body.find("Authentication required") != std::string::npos);
    post.headers["authorization"] = "Bearer wrong";
    CHECK(!writes.authorize(post, denied));
    CHECK(denied.body.find("Invalid token") != std::string::npos);
    post.headers["authorization"] = "bearer s3cret";  // scheme 不区分大小写
    CHECK(writes.authorize(post, denied));
    post.headers.erase("authorization");
    post.headers["x-auth-token"] = "s3cret";
    CHECK(writes.authorize(post, denied));
    post.headers.erase("x-auth-token");
    post.queryParams["access_token"] = "s3cret";       // 写操作不接受查询参数
    CHECK(!writes.authorize(post, denied));

    AuthPolicy all(AuthConfig{"s3cret", true});
    CHECK(all.authorize(makeRequest("GET", "/api/v1/health"), denied));           // 健康检查始终放行
    CHECK(all.authorize(makeRequest("GET", "/app.js"), denied));
    CHECK(!all.authorize(makeRequest("GET", "/api/v1/tests"), denied));
    HttpRequest get = makeRequest("GET", "/api/v1/tests/x/report");
    get.queryParams["access_token"] = "s3cret";
    CHECK(all.authorize(get, denied));                                            // 下载链接可用查询参数
    HttpRequest ws = makeRequest("GET", "/ws/v1/events");
    ws.headers["upgrade"] = "websocket";
    CHECK(!all.authorize(ws, denied));
    ws.queryParams["access_token"] = "s3cret";
    CHECK(all.authorize(ws, denied));
    HttpRequest head = makeRequest("HEAD", "/api/v1/tests");
    CHECK(!all.authorize(head, denied));

    CHECK(AuthPolicy::constantTimeEquals("abc", "abc"));
    CHECK(!AuthPolicy::constantTimeEquals("abc", "abd"));
    CHECK(!AuthPolicy::constantTimeEquals("abc", "abcd"));
    CHECK(!AuthPolicy::constantTimeEquals("", "a"));
}

TEST_CASE("http: request filter runs before routing and can be removed") {
    HttpServerConfig cfg;
    cfg.logRequests = false;
    HttpServer server(cfg);
    server.get("/api/v1/x", [](const HttpRequest&) { return HttpResponse::text(200, "x"); });
    int calls = 0;
    server.setRequestFilter([&](const HttpRequest& req, HttpResponse& denied) {
        calls++;
        if (req.header("x-ok") == "1") return true;
        denied = HttpResponse::error(401, "nope");
        return false;
    });
    HttpResponse r = server.dispatch(makeRequest("GET", "/api/v1/x"));
    CHECK_EQ(r.statusCode, 401);
    CHECK_EQ(r.headers["Access-Control-Allow-Origin"], std::string("*"));
    HttpRequest ok = makeRequest("GET", "/api/v1/x");
    ok.headers["x-ok"] = "1";
    CHECK_EQ(server.dispatch(ok).body, std::string("x"));
    // OPTIONS 预检不经过过滤器
    CHECK_EQ(server.dispatch(makeRequest("OPTIONS", "/api/v1/x")).statusCode, 204);
    CHECK_EQ(calls, 2);
    server.setRequestFilter(nullptr);
    CHECK_EQ(server.dispatch(makeRequest("GET", "/api/v1/x")).statusCode, 200);
}
