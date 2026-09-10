/*
 * 集成测试：启动完整的 TestHub（随机端口、mock runner），通过真实 TCP 连接验证
 * HTTP API、keep-alive、并发、WebSocket 事件流与规范 CRUD。
 */

#include "test_framework.h"
#include "server/websocket_server.h"
#include "testhub.h"
#include "util/base64.h"
#include "util/file_util.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

using namespace testhub;
namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------
// 极简 HTTP/WS 客户端
// ------------------------------------------------------------

struct TcpClient {
    int fd = -1;
    std::string buffer;

    explicit TcpClient(int port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            fd = -1;
        }
    }
    ~TcpClient() { if (fd >= 0) ::close(fd); }
    bool ok() const { return fd >= 0; }

    bool sendAll(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // 读取直到 buffer 中至少有 want 字节或超时
    bool fill(size_t want, int timeoutMs) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (buffer.size() < want) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;
            pollfd p{fd, POLLIN, 0};
            int r = ::poll(&p, 1, static_cast<int>(remaining));
            if (r <= 0) return false;
            char tmp[8192];
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            buffer.append(tmp, static_cast<size_t>(n));
        }
        return true;
    }

    bool readUntil(const std::string& marker, int timeoutMs) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (buffer.find(marker) == std::string::npos) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;
            if (!fill(buffer.size() + 1, static_cast<int>(remaining))) return false;
        }
        return true;
    }
};

struct HttpResult {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    Json json() const { return Json::tryParse(body); }
};

bool readHttpResponse(TcpClient& c, HttpResult& out, int timeoutMs = 5000) {
    if (!c.readUntil("\r\n\r\n", timeoutMs)) return false;
    size_t headEnd = c.buffer.find("\r\n\r\n");
    std::string head = c.buffer.substr(0, headEnd);
    c.buffer.erase(0, headEnd + 4);
    size_t lineEnd = head.find("\r\n");
    std::string statusLine = head.substr(0, lineEnd);
    out.status = std::atoi(statusLine.substr(9, 3).c_str());
    out.headers.clear();
    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            std::string v = line.substr(colon + 1);
            while (!v.empty() && v.front() == ' ') v.erase(0, 1);
            out.headers[k] = v;
        }
        if (e == std::string::npos) break;
        pos = e + 2;
    }
    size_t len = static_cast<size_t>(std::atol(out.headers["content-length"].c_str()));
    if (!c.fill(len, timeoutMs)) return false;
    out.body = c.buffer.substr(0, len);
    c.buffer.erase(0, len);
    return true;
}

HttpResult request(int port, const std::string& method, const std::string& path, const std::string& body = "",
                   const std::string& contentType = "application/json") {
    HttpResult r;
    TcpClient c(port);
    if (!c.ok()) return r;
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    if (!body.empty()) req += "Content-Type: " + contentType + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    c.sendAll(req);
    readHttpResponse(c, r);
    return r;
}

struct WsClient {
    TcpClient c;
    explicit WsClient(int port, const std::string& path = "/ws/v1/events") : c(port) {
        if (!c.ok()) return;
        std::string key = Base64::encode("0123456789abcdef");
        c.sendAll("GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n");
        upgraded = c.readUntil("\r\n\r\n", 5000);
        if (upgraded) {
            size_t e = c.buffer.find("\r\n\r\n");
            std::string head = c.buffer.substr(0, e);
            c.buffer.erase(0, e + 4);
            upgraded = head.find("101") != std::string::npos &&
                       head.find(WebSocketServer::computeAcceptKey(key)) != std::string::npos;
        }
    }
    bool upgraded = false;

    // 读取一个文本帧；返回 false 表示超时/关闭
    bool readText(std::string& payload, int timeoutMs) {
        if (!c.fill(2, timeoutMs)) return false;
        auto b0 = static_cast<unsigned char>(c.buffer[0]);
        auto b1 = static_cast<unsigned char>(c.buffer[1]);
        size_t len = b1 & 0x7f;
        size_t idx = 2;
        if (len == 126) {
            if (!c.fill(4, timeoutMs)) return false;
            len = (static_cast<size_t>(static_cast<unsigned char>(c.buffer[2])) << 8) | static_cast<unsigned char>(c.buffer[3]);
            idx = 4;
        } else if (len == 127) {
            if (!c.fill(10, timeoutMs)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<unsigned char>(c.buffer[2 + i]);
            idx = 10;
        }
        if (!c.fill(idx + len, timeoutMs)) return false;
        payload = c.buffer.substr(idx, len);
        c.buffer.erase(0, idx + len);
        int opcode = b0 & 0x0f;
        if (opcode == 0x8) return false;
        if (opcode != 0x1) return readText(payload, timeoutMs);
        return true;
    }

    void sendText(const std::string& payload) {
        // 客户端帧必须掩码
        std::string frame;
        frame.push_back(static_cast<char>(0x81));
        if (payload.size() < 126) {
            frame.push_back(static_cast<char>(0x80 | payload.size()));
        } else {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
            frame.push_back(static_cast<char>(payload.size() & 0xff));
        }
        unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
        frame.append(reinterpret_cast<const char*>(mask), 4);
        for (size_t i = 0; i < payload.size(); ++i) frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        c.sendAll(frame);
    }
};

// ------------------------------------------------------------
// 服务器夹具
// ------------------------------------------------------------

std::string makeTempDir(const char* prefix) {
    std::string dir = (fs::temp_directory_path() / (std::string(prefix) + std::to_string(::getpid()) + "-" +
                                                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    fs::create_directories(dir);
    return dir;
}

struct Server {
    std::string specsDir;
    std::string resultsDir;
    bool ownsResultsDir = true;
    TestHub hub;
    int port = 0;

    // resultsDir 为空时使用新的临时目录；传入已有目录可模拟重启后回放历史
    explicit Server(const std::string& existingResultsDir = "") {
        specsDir = makeTempDir("testhub-it-");
        fs::create_directories(specsDir + "/concepts");
        for (const auto& name : {"login.spec", "calculator.spec", "checkout.spec"}) {
            fs::copy_file(std::string(TESTHUB_SOURCE_DIR) + "/specs/" + name, specsDir + "/" + name);
        }
        fs::copy_file(std::string(TESTHUB_SOURCE_DIR) + "/specs/concepts/auth.cpt", specsDir + "/concepts/auth.cpt");
        if (existingResultsDir.empty()) resultsDir = makeTempDir("testhub-results-");
        else { resultsDir = existingResultsDir; ownsResultsDir = false; }

        TestHubConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 0;
        cfg.specsDir = specsDir;
        cfg.resultsDir = resultsDir;
        cfg.runnerLanguage = "mock";
        cfg.logLevel = "warn";
        cfg.logRequests = false;
        cfg.httpWorkerThreads = 4;
        cfg.maxConcurrentTests = 2;
        if (!hub.initialize(cfg) || !hub.start()) throw std::runtime_error("failed to start TestHub");
        port = hub.boundPort();
    }
    ~Server() {
        hub.stop();
        std::error_code ec;
        fs::remove_all(specsDir, ec);
        if (ownsResultsDir) fs::remove_all(resultsDir, ec);
    }

    Json waitForTerminal(const std::string& id, int timeoutMs = 10000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            Json st = request(port, "GET", "/api/v1/tests/" + id).json();
            std::string state = st["state"].asString();
            if (state == "passed" || state == "failed" || state == "error" || state == "cancelled") return st;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return Json();
    }
};

} // namespace

TEST_CASE("integration: health, status and web ui are served") {
    Server s;
    REQUIRE(s.port > 0);
    HttpResult health = request(s.port, "GET", "/api/v1/health");
    CHECK_EQ(health.status, 200);
    CHECK_EQ(health.json()["status"].asString(), std::string("ok"));

    HttpResult status = request(s.port, "GET", "/api/v1/status");
    CHECK_EQ(status.status, 200);
    CHECK_EQ(status.json()["version"].asString(), std::string(TestHub::version()));
    CHECK_EQ(status.json()["runner"]["state"].asString(), std::string("connected"));
    CHECK_EQ(status.headers["content-type"], std::string("application/json; charset=utf-8"));
    CHECK_EQ(status.headers["access-control-allow-origin"], std::string("*"));

    HttpResult index = request(s.port, "GET", "/");
    CHECK_EQ(index.status, 200);
    CHECK(index.body.find("<title>") != std::string::npos);
    CHECK_EQ(request(s.port, "GET", "/app.js").headers["content-type"], std::string("application/javascript; charset=utf-8"));
    CHECK_EQ(request(s.port, "GET", "/tests").status, 200);  // SPA fallback
    CHECK_EQ(request(s.port, "GET", "/api/v1/nothing").status, 404);
    CHECK_EQ(request(s.port, "PUT", "/api/v1/status", "{}").status, 405);
}

TEST_CASE("integration: submit test, follow status and fetch result") {
    Server s;
    HttpResult submit = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec","calculator.spec"],"name":"it"})");
    REQUIRE_EQ(submit.status, 202);
    Json j = submit.json();
    std::string id = j["test_id"].asString();
    REQUIRE(!id.empty());
    CHECK_EQ(submit.headers["location"], "/api/v1/tests/" + id);

    Json st = s.waitForTerminal(id);
    REQUIRE(!st.isNull());
    CHECK_EQ(st["state"].asString(), std::string("passed"));
    CHECK_EQ(st["total_scenarios"].asInt(), 9);
    CHECK_EQ(st["passed_scenarios"].asInt(), 9);

    HttpResult result = request(s.port, "GET", "/api/v1/tests/" + id + "/result");
    REQUIRE_EQ(result.status, 200);
    Json r = result.json();
    CHECK_EQ(r["specs"].size(), static_cast<size_t>(2));
    CHECK_EQ(r["specs"][0]["file"].asString(), std::string("login.spec"));
    CHECK_EQ(r["specs"][0]["scenarios"][0]["steps"].size(), static_cast<size_t>(4));
    CHECK_EQ(r["specs"][1]["scenarios"].size(), static_cast<size_t>(6));

    HttpResult list = request(s.port, "GET", "/api/v1/tests?state=passed");
    CHECK_EQ(list.status, 200);
    CHECK(list.json()["tests"].size() >= 1);
    CHECK_EQ(list.json()["tests"][0]["name"].asString(), std::string("it"));

    HttpResult events = request(s.port, "GET", "/api/v1/tests/" + id + "/events");
    CHECK_EQ(events.status, 200);
    CHECK(events.json()["events"].size() > 5);

    HttpResult junit = request(s.port, "GET", "/api/v1/tests/" + id + "/report?format=junit&download=1");
    CHECK_EQ(junit.status, 200);
    CHECK_EQ(junit.headers["content-type"], std::string("application/xml; charset=utf-8"));
    CHECK_EQ(junit.headers["content-disposition"], "attachment; filename=\"" + id + ".xml\"");
    CHECK(junit.body.find("<testsuites name=\"it\" tests=\"9\" failures=\"0\" errors=\"0\" skipped=\"0\"") != std::string::npos);
    CHECK(junit.body.find("<testsuite id=\"0\" name=\"用户登录\" file=\"login.spec\" tests=\"3\"") != std::string::npos);
    CHECK(junit.body.find("<testcase name=\"两数相加 [row 1]\" classname=\"calculator\"") != std::string::npos);
    HttpResult html = request(s.port, "GET", "/api/v1/tests/" + id + "/report?format=html");
    CHECK_EQ(html.status, 200);
    CHECK_EQ(html.headers["content-type"], std::string("text/html; charset=utf-8"));
    CHECK(html.headers.count("content-disposition") == 0);
    CHECK(html.body.find("<!DOCTYPE html>") == 0);
    CHECK(html.body.find("<span class=\"pill passed\">通过</span>") != std::string::npos);
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests/" + id + "/report?format=pdf").status, 400);
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests/nope/report").status, 404);

    HttpResult rerun = request(s.port, "POST", "/api/v1/tests/" + id + "/rerun", "{}");
    CHECK_EQ(rerun.status, 202);
    std::string rerunId = rerun.json()["test_id"].asString();
    CHECK(!rerunId.empty());
    CHECK_EQ(s.waitForTerminal(rerunId)["state"].asString(), std::string("passed"));

    CHECK_EQ(request(s.port, "DELETE", "/api/v1/tests/" + rerunId).status, 200);
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests/" + rerunId).status, 404);
}

TEST_CASE("integration: validation errors and bad json") {
    Server s;
    CHECK_EQ(request(s.port, "POST", "/api/v1/tests", "{not json").status, 400);
    // 空请求体 = 运行规范目录下的全部规范
    HttpResult all = request(s.port, "POST", "/api/v1/tests", "{}");
    CHECK_EQ(all.status, 202);
    CHECK_EQ(s.waitForTerminal(all.json()["test_id"].asString())["total_specs"].asInt(), 3);
    HttpResult missing = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["missing.spec"]})");
    CHECK_EQ(missing.status, 400);
    CHECK(missing.json()["error"].asString().find("missing.spec") != std::string::npos);
    CHECK_EQ(request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"],"tags":["(bad"]})").status, 400);
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests/does-not-exist").status, 404);
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests/does-not-exist/result").status, 404);
}

TEST_CASE("integration: spec CRUD, validation and concepts") {
    Server s;
    Json list = request(s.port, "GET", "/api/v1/specs").json();
    CHECK_EQ(list["count"].asInt(), 3);
    CHECK_EQ(list["total_scenarios"].asInt(), 8);

    HttpResult one = request(s.port, "GET", "/api/v1/specs/login.spec");
    CHECK_EQ(one.status, 200);
    CHECK_EQ(one.json()["spec"]["heading"].asString(), std::string("用户登录"));
    CHECK_EQ(one.json()["spec"]["scenarios"].size(), static_cast<size_t>(3));
    CHECK_EQ(one.json()["valid"].asBool(), true);
    HttpResult raw = request(s.port, "GET", "/api/v1/specs/login.spec?raw=true");
    CHECK_EQ(raw.status, 200);
    CHECK(raw.body.find("# 用户登录") != std::string::npos);

    // 校验
    HttpResult valid = request(s.port, "POST", "/api/v1/specs/validate", R"({"content":"# A\n## B\n* step\n"})");
    CHECK_EQ(valid.status, 200);
    CHECK_EQ(valid.json()["valid"].asBool(), true);
    HttpResult invalid = request(s.port, "POST", "/api/v1/specs/validate", R"({"content":"* orphan\n"})");
    CHECK_EQ(invalid.json()["valid"].asBool(), false);
    CHECK(invalid.json()["results"][0]["errors"].size() >= 1);

    // 新建 / 覆盖 / 删除
    HttpResult put = request(s.port, "PUT", "/api/v1/specs/new/created.spec", R"({"content":"# New\n## S\n* x\n"})");
    CHECK_EQ(put.status, 201);
    CHECK(fs::exists(s.specsDir + "/new/created.spec"));
    CHECK_EQ(request(s.port, "PUT", "/api/v1/specs/new/created.spec", "# New2\n## S\n* y\n", "text/plain").status, 200);
    CHECK_EQ(request(s.port, "GET", "/api/v1/specs").json()["count"].asInt(), 4);
    // 新建的规范可直接运行
    HttpResult run = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["new/created.spec"]})");
    REQUIRE_EQ(run.status, 202);
    CHECK_EQ(s.waitForTerminal(run.json()["test_id"].asString())["state"].asString(), std::string("passed"));
    CHECK_EQ(request(s.port, "DELETE", "/api/v1/specs/new/created.spec").status, 200);
    CHECK_EQ(request(s.port, "GET", "/api/v1/specs/new/created.spec").status, 404);

    // 越界与非法扩展名
    CHECK_EQ(request(s.port, "PUT", "/api/v1/specs/../evil.spec", R"({"content":"x"})").status, 400);
    CHECK_EQ(request(s.port, "PUT", "/api/v1/specs/evil.txt", R"({"content":"x"})").status, 400);

    Json concepts = request(s.port, "GET", "/api/v1/concepts").json();
    CHECK_EQ(concepts["count"].asInt(), 1);
}

TEST_CASE("integration: websocket receives welcome and test events") {
    Server s;
    WsClient ws(s.port);
    REQUIRE(ws.upgraded);
    std::string payload;
    REQUIRE(ws.readText(payload, 3000));
    CHECK_EQ(Json::parse(payload)["type"].asString(), std::string("welcome"));

    HttpResult submit = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"]})");
    REQUIRE_EQ(submit.status, 202);
    std::string id = submit.json()["test_id"].asString();

    std::map<std::string, int> counts;
    bool completed = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!completed && std::chrono::steady_clock::now() < deadline) {
        if (!ws.readText(payload, 3000)) break;
        Json msg = Json::parse(payload);
        if (msg["type"].asString() != "event") continue;
        counts[msg["event"].asString()]++;
        if (msg["event"].asString() == EventType::TEST_COMPLETED && msg["test_id"].asString() == id) completed = true;
    }
    CHECK(completed);
    CHECK(counts[EventType::TEST_STARTED] >= 1);
    CHECK_EQ(counts[EventType::SCENARIO_COMPLETED], 3);
    CHECK(counts[EventType::STEP_COMPLETED] >= 10);

    // 订阅过滤：只接收 test.* 事件
    ws.sendText(R"({"action":"subscribe","events":["test.*"]})");
    HttpResult second = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"]})");
    std::string id2 = second.json()["test_id"].asString();
    bool sawStep = false, done = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        if (!ws.readText(payload, 3000)) break;
        Json msg = Json::parse(payload);
        if (msg["type"].asString() != "event") continue;
        if (msg["test_id"].asString() != id2) continue;
        if (msg["event"].asString().rfind("step.", 0) == 0) sawStep = true;
        if (msg["event"].asString() == EventType::TEST_COMPLETED) done = true;
    }
    CHECK(done);
    CHECK(!sawStep);
    CHECK(s.hub.getWebSocketServer().connectionCount() >= 1);
}

TEST_CASE("integration: keep-alive, concurrency and large bodies") {
    Server s;
    // 同一连接上两个请求
    TcpClient c(s.port);
    REQUIRE(c.ok());
    c.sendAll("GET /api/v1/health HTTP/1.1\r\nHost: x\r\n\r\nGET /api/v1/status HTTP/1.1\r\nHost: x\r\n\r\n");
    HttpResult r1, r2;
    REQUIRE(readHttpResponse(c, r1));
    REQUIRE(readHttpResponse(c, r2));
    CHECK_EQ(r1.status, 200);
    CHECK_EQ(r2.status, 200);
    CHECK(r2.json()["running"].asBool());

    // 并发请求
    std::vector<std::thread> threads;
    std::atomic<int> okCount{0};
    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&] {
            HttpResult r = request(s.port, "GET", "/api/v1/specs");
            if (r.status == 200 && r.json()["count"].asInt() == 3) ++okCount;
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(okCount.load(), 16);

    // 大请求体（200KB 规范校验）
    std::string big = "# Big\n";
    for (int i = 0; i < 4000; ++i) big += "## scenario " + std::to_string(i) + "\n* step " + std::to_string(i) + "\n";
    Json payload = Json::object();
    payload["content"] = big;
    HttpResult v = request(s.port, "POST", "/api/v1/specs/validate", payload.dump());
    CHECK_EQ(v.status, 200);
    CHECK_EQ(v.json()["results"][0]["scenario_count"].asInt(), 4000);

    // 慢客户端：只发一半头部然后断开，服务器不应崩溃
    {
        TcpClient half(s.port);
        half.sendAll("GET /api/v1/health HTTP/1.1\r\nHo");
    }
    CHECK_EQ(request(s.port, "GET", "/api/v1/health").status, 200);
}

TEST_CASE("integration: cancel a running test via API") {
    Server s;
    std::string slow = "# Slow\n";
    for (int i = 0; i < 30; ++i) slow += "## s" + std::to_string(i) + "\n* sleep \"100\"\n\n";
    FileUtil::writeFile(s.specsDir + "/slow.spec", slow);
    HttpResult submit = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["slow.spec"]})");
    REQUIRE_EQ(submit.status, 202);
    std::string id = submit.json()["test_id"].asString();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests/" + id + "/report").status, 409);
    HttpResult cancel = request(s.port, "POST", "/api/v1/tests/" + id + "/cancel", "{}");
    CHECK_EQ(cancel.status, 200);
    Json st = s.waitForTerminal(id);
    CHECK_EQ(st["state"].asString(), std::string("cancelled"));
    CHECK(st["executed_scenarios"].asInt() < 30);

    // 取消后的报表：未执行的场景记为 skipped
    HttpResult report = request(s.port, "GET", "/api/v1/tests/" + id + "/report?format=xml");
    CHECK_EQ(report.status, 200);
    CHECK(report.body.find("<skipped message=\"Skipped because the test was cancelled\"/>") != std::string::npos);
    CHECK(report.body.find("tests=\"30\"") != std::string::npos);

    Json queue = request(s.port, "GET", "/api/v1/queue").json();
    CHECK_EQ(queue["size"].asInt(), 0);
    Json runner = request(s.port, "GET", "/api/v1/runner/status").json();
    CHECK_EQ(runner["language"].asString(), std::string("mock"));
}

TEST_CASE("integration: results persist across server restarts") {
    std::string resultsDir;
    std::string id;
    {
        Server s;
        resultsDir = s.resultsDir;
        s.ownsResultsDir = false;
        HttpResult submit = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["calculator.spec"],"name":"persisted run","tags":["unit"]})");
        REQUIRE_EQ(submit.status, 202);
        id = submit.json()["test_id"].asString();
        Json st = s.waitForTerminal(id);
        CHECK_EQ(st["state"].asString(), std::string("passed"));
        CHECK(fs::exists(resultsDir + "/" + id + ".json"));

        // 删除记录同时删除文件
        HttpResult other = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"]})");
        std::string otherId = other.json()["test_id"].asString();
        s.waitForTerminal(otherId);
        CHECK(fs::exists(resultsDir + "/" + otherId + ".json"));
        CHECK_EQ(request(s.port, "DELETE", "/api/v1/tests/" + otherId).status, 200);
        CHECK(!fs::exists(resultsDir + "/" + otherId + ".json"));
    }
    {
        Server restarted(resultsDir);
        HttpResult list = request(restarted.port, "GET", "/api/v1/tests");
        REQUIRE_EQ(list.status, 200);
        CHECK_EQ(list.json()["total"].asInt(), 1);
        CHECK_EQ(list.json()["tests"][0]["test_id"].asString(), id);
        CHECK_EQ(list.json()["tests"][0]["name"].asString(), std::string("persisted run"));

        HttpResult status = request(restarted.port, "GET", "/api/v1/tests/" + id);
        CHECK_EQ(status.status, 200);
        CHECK_EQ(status.json()["state"].asString(), std::string("passed"));
        CHECK_EQ(status.json()["request"]["tags"][0].asString(), std::string("unit"));

        HttpResult result = request(restarted.port, "GET", "/api/v1/tests/" + id + "/result");
        REQUIRE_EQ(result.status, 200);
        CHECK_EQ(result.json()["specs"][0]["file"].asString(), std::string("calculator.spec"));
        CHECK_EQ(result.json()["specs"][0]["scenarios"].asArray().size(), static_cast<size_t>(6));
        CHECK_EQ(result.json()["specs"][0]["scenarios"][0]["data_row"]["a"].asString(), std::string("1"));

        Json stats = request(restarted.port, "GET", "/api/v1/status").json();
        CHECK(stats["stats"]["passed"].asInt() >= 1);

        // 重跑历史记录仍然可用
        HttpResult rerun = request(restarted.port, "POST", "/api/v1/tests/" + id + "/rerun", "{}");
        CHECK_EQ(rerun.status, 202);
        restarted.waitForTerminal(rerun.json()["test_id"].asString());

        CHECK_EQ(request(restarted.port, "DELETE", "/api/v1/tests").status, 200);
        CHECK(!fs::exists(resultsDir + "/" + id + ".json"));
    }
    std::error_code ec;
    fs::remove_all(resultsDir, ec);
}
