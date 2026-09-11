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
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <cstring>
#include <filesystem>
#include <mutex>
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
                   const std::string& contentType = "application/json", const std::string& extraHeaders = "") {
    HttpResult r;
    TcpClient c(port);
    if (!c.ok()) return r;
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + extraHeaders;
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
    std::string schedulesDir;
    bool ownsResultsDir = true;
    TestHub hub;
    int port = 0;

    // resultsDir 为空时使用新的临时目录；传入已有目录可模拟重启后回放历史；tweak 可在启动前调整配置
    explicit Server(const std::string& existingResultsDir = "", const std::function<void(TestHubConfig&)>& tweak = nullptr) {
        specsDir = makeTempDir("testhub-it-");
        fs::create_directories(specsDir + "/concepts");
        for (const auto& name : {"login.spec", "calculator.spec", "checkout.spec"}) {
            fs::copy_file(std::string(TESTHUB_SOURCE_DIR) + "/specs/" + name, specsDir + "/" + name);
        }
        fs::copy_file(std::string(TESTHUB_SOURCE_DIR) + "/specs/concepts/auth.cpt", specsDir + "/concepts/auth.cpt");
        if (existingResultsDir.empty()) resultsDir = makeTempDir("testhub-results-");
        else { resultsDir = existingResultsDir; ownsResultsDir = false; }
        schedulesDir = makeTempDir("testhub-sched-");

        TestHubConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 0;
        cfg.specsDir = specsDir;
        cfg.resultsDir = resultsDir;
        cfg.schedulesDir = schedulesDir;
        cfg.runnerLanguage = "mock";
        cfg.logLevel = "warn";
        cfg.logRequests = false;
        cfg.httpWorkerThreads = 4;
        cfg.maxConcurrentTests = 2;
        cfg.callbackRetryBackoffMs = 30;
        cfg.callbackTimeoutMs = 2000;
        if (tweak) tweak(cfg);
        if (!hub.initialize(cfg) || !hub.start()) throw std::runtime_error("failed to start TestHub");
        port = hub.boundPort();
    }
    ~Server() {
        hub.stop();
        std::error_code ec;
        fs::remove_all(specsDir, ec);
        if (ownsResultsDir) fs::remove_all(resultsDir, ec);
        fs::remove_all(schedulesDir, ec);
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

void copySelfcheckSpec(const Server& s) {
    fs::copy_file(std::string(TESTHUB_SOURCE_DIR) + "/specs/selfcheck.spec",
                  fs::path(s.specsDir) / "selfcheck.spec",
                  fs::copy_options::overwrite_existing);
}

void expectSelfcheckPassed(Server& s, int timeoutMs = 45000) {
    copySelfcheckSpec(s);
    HttpResult submit = request(s.port, "POST", "/api/v1/tests",
                                R"({"spec_files":["selfcheck.spec"],"name":"selfcheck"})");
    REQUIRE_EQ(submit.status, 202);
    std::string id = submit.json()["test_id"].asString();
    Json st = s.waitForTerminal(id, timeoutMs);
    REQUIRE(!st.isNull());
    if (st["state"].asString() != "passed") {
        std::cout << "selfcheck result:\n"
                  << request(s.port, "GET", "/api/v1/tests/" + id + "/result").body << "\n";
    }
    REQUIRE_EQ(st["state"].asString(), std::string("passed"));
    CHECK_EQ(st["total_scenarios"].asInt(), 3);
    CHECK_EQ(st["passed_scenarios"].asInt(), 3);

    Json list = request(s.port, "GET", "/api/v1/tests?limit=50").json();
    std::string childId;
    for (const auto& t : list["tests"].asArray()) {
        if (t["name"].asString() == "selfcheck-child") childId = t["test_id"].asString();
    }
    REQUIRE(!childId.empty());
    Json child = s.waitForTerminal(childId, timeoutMs);
    REQUIRE(!child.isNull());
    CHECK_EQ(child["state"].asString(), std::string("passed"));
}

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
    CHECK(status.json()["url"].asString().find(std::to_string(s.port)) != std::string::npos);
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
    CHECK_EQ(request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"],"step_retry":6})").status, 400);
    CHECK_EQ(request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"],"step_retry":-1})").status, 400);
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

TEST_CASE("integration: spec directory watcher reloads external changes") {
    Server s("", [](TestHubConfig& cfg) { cfg.specsWatchIntervalMs = 30; });
    Json status = request(s.port, "GET", "/api/v1/status").json();
    CHECK(status["spec_watcher"]["enabled"].asBool());
    CHECK_EQ(status["spec_watcher"]["interval_ms"].asInt(), 30);
    CHECK_EQ(status["spec_watcher"]["tracked_files"].asInt(), 4);  // 3 spec + 1 cpt

    auto watcherEvents = [&](const std::string& file) {
        int n = 0;
        Json evs = request(s.port, "GET", "/api/v1/events?limit=500").json()["events"];
        for (size_t i = 0; i < evs.size(); ++i) {
            if (evs[i]["event"].asString() != "specs.reloaded") continue;
            if (evs[i]["data"]["source"].asString() != "watcher") continue;
            if (file.empty() || evs[i]["data"]["files"].asString().find(file) != std::string::npos) ++n;
        }
        return n;
    };
    auto waitFor = [&](const std::string& file, int atLeast) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (watcherEvents(file) >= atLeast) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    // 外部工具直接写入规范目录（绕过 API）：监控器应发布 specs.reloaded 并让新文件出现在列表中
    FileUtil::writeFile(s.specsDir + "/external.spec", "# External\n\n## S\n\n* 用户 \"a\" 已登录\n");
    REQUIRE(waitFor("external.spec", 1));
    Json list = request(s.port, "GET", "/api/v1/specs").json();
    bool found = false;
    for (size_t i = 0; i < list["specs"].size(); ++i) if (list["specs"][i]["file"].asString() == "external.spec") found = true;
    CHECK(found);

    // 新概念文件：字典自动重载，可通过 /concepts 看到
    fs::create_directories(s.specsDir + "/concepts");
    FileUtil::writeFile(s.specsDir + "/concepts/extra.cpt", "# 外部概念 <x>\n* 使用 <x>\n");
    REQUIRE(waitFor("concepts/extra.cpt", 1));
    CHECK_EQ(request(s.port, "GET", "/api/v1/concepts").json()["count"].asInt(), 2);

    // 通过 API 写入的文件不会被监控器重复报告
    int before = watcherEvents("");
    CHECK_EQ(request(s.port, "PUT", "/api/v1/specs/via-api.spec", "# Via API\n## S\n* 用户 \"a\" 已登录\n", "text/plain").status, 201);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // 约 5 个轮询周期
    CHECK_EQ(watcherEvents(""), before);
    CHECK_EQ(watcherEvents("via-api.spec"), 0);

    // 外部删除
    fs::remove(s.specsDir + "/external.spec");
    REQUIRE(waitFor("external.spec", 2));
    status = request(s.port, "GET", "/api/v1/status").json();
    CHECK(status["spec_watcher"]["changes"].asInt() >= 3);
    CHECK(!status["spec_watcher"]["last_change_at"].asString().empty());

    // 手动 reload 返回 changes 字段并标注 source=manual
    Json reload = request(s.port, "POST", "/api/v1/specs/reload").json();
    CHECK(reload["changes"]["created"].isArray());
    CHECK_EQ(reload["specs"].asInt(), 4);  // login/calculator/checkout/via-api
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
    // 等待服务端确认过滤器生效，否则新测试的早期事件可能在过滤器安装前就已发出
    bool acked = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!acked && std::chrono::steady_clock::now() < deadline) {
        if (!ws.readText(payload, 3000)) break;
        Json msg = Json::parse(payload);
        if (msg["type"].asString() == "subscribed") {
            acked = true;
            CHECK_EQ(msg["events"].size(), static_cast<size_t>(1));
        }
    }
    REQUIRE(acked);
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

TEST_CASE("integration: websocket survives rapid connect/disconnect churn") {
    Server s;
    // 回归：客户端在升级完成后立刻断开，读线程可能先于 handleUpgrade 的句柄赋值结束，
    // 曾导致 joinable 线程随 Connection 析构而 std::terminate
    for (int i = 0; i < 40; ++i) {
        WsClient ws(s.port);
        REQUIRE(ws.upgraded);
        if (i % 2 == 0) {
            // 一半连接发送 close 帧，另一半直接关闭 TCP
            std::string frame;
            frame.push_back(static_cast<char>(0x88));
            frame.push_back(static_cast<char>(0x80));
            frame.append("\x12\x34\x56\x78", 4);
            ws.c.sendAll(frame);
        }
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (s.hub.getWebSocketServer().connectionCount() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(s.hub.getWebSocketServer().connectionCount(), static_cast<size_t>(0));
    // 服务仍然正常：新的连接与请求都能被处理
    WsClient again(s.port);
    REQUIRE(again.upgraded);
    std::string payload;
    REQUIRE(again.readText(payload, 3000));
    CHECK_EQ(Json::parse(payload)["type"].asString(), std::string("welcome"));
    CHECK_EQ(request(s.port, "GET", "/api/v1/status").status, 200);
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

TEST_CASE("integration: callback_url receives completion payload with retry") {
    // 接收端：第一次返回 503 触发重试，之后 200；记录收到的请求
    struct Received { std::string body; std::map<std::string, std::string> headers; };
    std::mutex mu;
    std::vector<Received> received;
    std::atomic<int> hits{0};
    HttpServerConfig rc;
    rc.host = "127.0.0.1";
    rc.port = 0;
    rc.workerThreads = 2;
    rc.logRequests = false;
    HttpServer receiver(rc);
    receiver.post("/hook", [&](const HttpRequest& req) {
        int n = ++hits;
        {
            std::lock_guard<std::mutex> lock(mu);
            received.push_back({req.body, req.headers});
        }
        if (n == 1) return HttpResponse::error(503, "try again");
        return HttpResponse::json(200, Json::object());
    });
    receiver.post("/never", [&](const HttpRequest&) { return HttpResponse::error(500, "down"); });
    REQUIRE(receiver.start());
    std::string hookUrl = "http://127.0.0.1:" + std::to_string(receiver.port()) + "/hook";

    Server s;
    CHECK_EQ(request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"],"callback_url":"ftp://x/y"})").status, 400);
    CHECK_EQ(request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"],"callback_url":"https://x/y"})").status, 400);

    HttpResult submit = request(s.port, "POST", "/api/v1/tests",
                                R"({"spec_files":["login.spec"],"name":"cb","metadata":{"build":"7"},"callback_url":")" + hookUrl + "\"}");
    REQUIRE_EQ(submit.status, 202);
    std::string id = submit.json()["test_id"].asString();
    CHECK_EQ(s.waitForTerminal(id)["state"].asString(), std::string("passed"));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (hits < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE_EQ(hits.load(), 2);
    // 等待 callback.delivered 事件进入历史
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Json delivered;
    while (std::chrono::steady_clock::now() < deadline) {
        Json evs = request(s.port, "GET", "/api/v1/tests/" + id + "/events").json()["events"];
        for (size_t i = 0; i < evs.size(); ++i) if (evs[i]["event"].asString() == "callback.delivered") delivered = evs[i];
        if (!delivered.isNull()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(!delivered.isNull());
    CHECK_EQ(delivered["data"]["attempts"].asString(), std::string("2"));
    CHECK_EQ(delivered["data"]["status"].asString(), std::string("200"));

    std::lock_guard<std::mutex> lock(mu);
    REQUIRE_EQ(received.size(), static_cast<size_t>(2));
    Json p1 = Json::tryParse(received[0].body);
    Json p2 = Json::tryParse(received[1].body);
    CHECK_EQ(p1["attempt"].asInt(), 1);
    CHECK_EQ(p2["attempt"].asInt(), 2);
    CHECK_EQ(p2["event"].asString(), std::string("test.completed"));
    CHECK_EQ(p2["test_id"].asString(), id);
    CHECK_EQ(p2["name"].asString(), std::string("cb"));
    CHECK_EQ(p2["state"].asString(), std::string("passed"));
    CHECK_EQ(p2["total_scenarios"].asInt(), 3);
    CHECK_EQ(p2["passed_scenarios"].asInt(), 3);
    CHECK_EQ(p2["metadata"]["build"].asString(), std::string("7"));
    CHECK_EQ(p2["failed_scenarios_detail"].size(), static_cast<size_t>(0));
    CHECK_EQ(p2["links"]["report_junit"].asString(), "/api/v1/tests/" + id + "/report?format=junit");
    CHECK_EQ(received[1].headers.at("x-testhub-event"), std::string("test.completed"));
    CHECK_EQ(received[1].headers.at("x-testhub-attempt"), std::string("2"));
    CHECK_EQ(received[1].headers.at("content-type"), std::string("application/json; charset=utf-8"));

    // 状态里能看到回调计数
    Json status = request(s.port, "GET", "/api/v1/status").json();
    CHECK_EQ(status["callbacks"]["delivered"].asInt(), 1);
    CHECK(status["callbacks"]["attempts"].asInt() >= 2);
    receiver.stop();
}

TEST_CASE("integration: callback gives up after max attempts and reports failure") {
    Server s;
    // 无人监听的端口：连接被拒绝 → 重试 3 次后放弃
    HttpServerConfig rc;
    rc.host = "127.0.0.1";
    rc.port = 0;
    HttpServer probe(rc);
    REQUIRE(probe.start());
    int freePort = probe.port();
    probe.stop();
    std::string url = "http://127.0.0.1:" + std::to_string(freePort) + "/hook";
    HttpResult submit = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"],"callback_url":")" + url + "\"}");
    REQUIRE_EQ(submit.status, 202);
    std::string id = submit.json()["test_id"].asString();
    s.waitForTerminal(id);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Json failed;
    while (std::chrono::steady_clock::now() < deadline) {
        Json evs = request(s.port, "GET", "/api/v1/tests/" + id + "/events").json()["events"];
        for (size_t i = 0; i < evs.size(); ++i) if (evs[i]["event"].asString() == "callback.failed") failed = evs[i];
        if (!failed.isNull()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(!failed.isNull());
    CHECK_EQ(failed["data"]["attempts"].asString(), std::string("3"));
    CHECK(!failed["data"]["error"].asString().empty());
    Json status = request(s.port, "GET", "/api/v1/status").json();
    CHECK_EQ(status["callbacks"]["failed"].asInt(), 1);
    CHECK_EQ(status["callbacks"]["pending"].asInt(), 0);
}

TEST_CASE("integration: bearer token protects write operations") {
    Server s("", [](TestHubConfig& cfg) { cfg.authToken = "top-secret"; });
    const std::string good = "Authorization: Bearer top-secret\r\n";

    Json health = request(s.port, "GET", "/api/v1/health").json();
    CHECK(health["auth_required"].asBool());
    CHECK(!health["auth_protect_reads"].asBool());
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests").status, 200);               // 读操作放行
    CHECK_EQ(request(s.port, "GET", "/").status, 200);                           // UI 放行
    Json cfg = request(s.port, "GET", "/api/v1/config").json();
    CHECK_EQ(cfg["server"]["auth_token"].asString(), std::string("***"));      // 不泄露 token

    HttpResult denied = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"]})");
    CHECK_EQ(denied.status, 401);
    CHECK_EQ(denied.headers["www-authenticate"], std::string("Bearer realm=\"TestHub\""));
    CHECK_EQ(request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"]})", "application/json",
                     "Authorization: Bearer nope\r\n").status, 401);
    CHECK_EQ(request(s.port, "DELETE", "/api/v1/tests").status, 401);
    CHECK_EQ(request(s.port, "PUT", "/api/v1/specs/x.spec", "# X\n## S\n* a\n", "text/plain").status, 401);
    // 预检不受影响
    CHECK_EQ(request(s.port, "OPTIONS", "/api/v1/tests").status, 204);

    HttpResult ok = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["login.spec"]})", "application/json", good);
    REQUIRE_EQ(ok.status, 202);
    std::string id = ok.json()["test_id"].asString();
    CHECK_EQ(s.waitForTerminal(id)["state"].asString(), std::string("passed"));
    CHECK_EQ(request(s.port, "DELETE", "/api/v1/tests/" + id, "", "application/json", "X-Auth-Token: top-secret\r\n").status, 200);

    WsClient ws(s.port);
    CHECK(ws.upgraded);                                                          // 未保护读操作时 WS 放行
}

TEST_CASE("integration: bearer token can also protect reads and websocket") {
    Server s("", [](TestHubConfig& cfg) { cfg.authToken = "t0k3n"; cfg.authProtectReads = true; });
    CHECK_EQ(request(s.port, "GET", "/api/v1/health").status, 200);
    CHECK_EQ(request(s.port, "GET", "/").status, 200);
    CHECK_EQ(request(s.port, "GET", "/app.js").status, 200);
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests").status, 401);
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests", "", "application/json", "Authorization: Bearer t0k3n\r\n").status, 200);
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests?access_token=t0k3n").status, 200);   // 下载链接形式
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests?access_token=wrong").status, 401);
    CHECK_EQ(request(s.port, "HEAD", "/api/v1/tests").status, 401);

    WsClient refused(s.port);
    CHECK(!refused.upgraded);
    WsClient accepted(s.port, "/ws/v1/events?access_token=t0k3n");
    REQUIRE(accepted.upgraded);
    std::string welcome;
    REQUIRE(accepted.readText(welcome, 3000));
    CHECK(welcome.find("\"welcome\"") != std::string::npos);
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

TEST_CASE("integration: python runner pool runs concurrent tests in separate processes") {
    if (std::system("python3 -c 'import sys' >/dev/null 2>&1") != 0) {
        std::cout << "    (python3 not available; skipping)\n";
        return;
    }
    std::string runnerDir = std::string(TESTHUB_SOURCE_DIR) + "/runners/python";
    Server s("", [&](TestHubConfig& cfg) {
        cfg.runnerLanguage = "python";
        cfg.runnerCommand = "python3 " + runnerDir + "/testhub_runner.py";
        cfg.projectPath = runnerDir;
        cfg.maxConcurrentTests = 2;
        cfg.runnerPoolSize = 0;   // 跟随并发数 -> 2 个进程
    });
    Json runner = request(s.port, "GET", "/api/v1/runner/status").json();
    REQUIRE_EQ(runner["pool_size"].asInt(), 2);
    REQUIRE_EQ(runner["alive"].asInt(), 2);
    CHECK_EQ(runner["state"].asString(), std::string("connected"));
    REQUIRE_EQ(runner["runners"].asArray().size(), static_cast<size_t>(2));
    CHECK(runner["runners"][0]["pid"].asInt() > 0);
    CHECK(runner["runners"][0]["pid"].asInt() != runner["runners"][1]["pid"].asInt());
    CHECK(runner["step_count"].asInt() > 0);

    // 每个场景等待 0.8 秒；两个测试若真正并行，总耗时应远小于串行的 1.6 秒
    FileUtil::writeFile(s.specsDir + "/parallel.spec",
                        "# 并行\n\n## 慢场景\n* 等待 \"0.8\" 秒\n* 输入第一个数 \"1\"\n* 输入第二个数 \"2\"\n* 点击加号\n* 结果应该是 \"3\"\n");
    auto start = std::chrono::steady_clock::now();
    HttpResult a = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["parallel.spec"],"name":"pool-a"})");
    HttpResult b = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["parallel.spec"],"name":"pool-b"})");
    REQUIRE_EQ(a.status, 202);
    REQUIRE_EQ(b.status, 202);
    Json ra = s.waitForTerminal(a.json()["test_id"].asString(), 15000);
    Json rb = s.waitForTerminal(b.json()["test_id"].asString(), 15000);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK_EQ(ra["state"].asString(), std::string("passed"));
    CHECK_EQ(rb["state"].asString(), std::string("passed"));
    CHECK_MSG(seconds < 1.5, "two 0.8 s tests took " + std::to_string(seconds) + " s; expected parallel execution");

    runner = request(s.port, "GET", "/api/v1/runner/status").json();
    CHECK_EQ(runner["busy"].asInt(), 0);
    CHECK(runner["runners"][0]["steps_executed"].asNumber() > 0);
    CHECK(runner["runners"][1]["steps_executed"].asNumber() > 0);

    // 外部杀掉槽位 0 的进程（`sh -c` 包装进程）：真正的 python 进程会成为孤儿并继续持有管道。
    // 槽位应在下次使用时自愈，且孤儿进程组被清理，其他槽位不受影响。
    int killedPid = runner["runners"][0]["pid"].asInt();
    REQUIRE(::kill(killedPid, SIGKILL) == 0);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           request(s.port, "GET", "/api/v1/runner/status").json()["alive"].asInt() != 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    runner = request(s.port, "GET", "/api/v1/runner/status").json();
    CHECK_EQ(runner["alive"].asInt(), 1);
    CHECK_EQ(runner["state"].asString(), std::string("connected"));   // 降级但可用
    CHECK_EQ(runner["runners"][0]["state"].asString(), std::string("error"));
    CHECK(runner["runners"][0]["last_error"].asString().find("restarted on next use") != std::string::npos);

    // 两个并发测试：其中一个必须落在死掉的槽位上并触发重启
    a = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["parallel.spec"],"name":"heal-a"})");
    b = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["parallel.spec"],"name":"heal-b"})");
    ra = s.waitForTerminal(a.json()["test_id"].asString(), 15000);
    rb = s.waitForTerminal(b.json()["test_id"].asString(), 15000);
    CHECK_EQ(ra["state"].asString(), std::string("passed"));
    CHECK_EQ(rb["state"].asString(), std::string("passed"));
    runner = request(s.port, "GET", "/api/v1/runner/status").json();
    CHECK_EQ(runner["alive"].asInt(), 2);
    CHECK_EQ(runner["restart_count"].asInt(), 1);
    CHECK_EQ(runner["runners"][0]["restart_count"].asInt(), 1);
    CHECK(runner["runners"][0]["pid"].asInt() != killedPid);
    CHECK_EQ(runner["runners"][1]["restart_count"].asInt(), 0);
    // 被杀进程的进程组（含孤儿 python）已不存在
    CHECK(::kill(-killedPid, 0) != 0);

    // 手动重启：两个进程都被替换，PID 变化
    int oldPid0 = runner["runners"][0]["pid"].asInt();
    HttpResult restart = request(s.port, "POST", "/api/v1/runner/restart", "{}");
    CHECK_EQ(restart.status, 200);
    CHECK(restart.json()["restarted"].asBool());
    CHECK_EQ(restart.json()["alive"].asInt(), 2);
    CHECK(restart.json()["runners"][0]["pid"].asInt() != oldPid0);
    CHECK_EQ(restart.json()["restart_count"].asInt(), 3);
}

TEST_CASE("integration: node runner executes the bundled specs end to end") {
    if (std::system("node -e 0 >/dev/null 2>&1") != 0) {
        std::cout << "    (node not available; skipping)\n";
        return;
    }
    std::string runnerDir = std::string(TESTHUB_SOURCE_DIR) + "/runners/node";
    Server s("", [&](TestHubConfig& cfg) {
        cfg.runnerLanguage = "node";
        cfg.runnerCommand = "node " + runnerDir + "/testhub_runner.js";
        cfg.projectPath = runnerDir;
        cfg.maxConcurrentTests = 1;
    });
    Json runner = request(s.port, "GET", "/api/v1/runner/status").json();
    REQUIRE_EQ(runner["state"].asString(), std::string("connected"));
    CHECK_EQ(runner["language"].asString(), std::string("node"));
    CHECK_EQ(runner["version"].asString().rfind("node-", 0), static_cast<size_t>(0));
    CHECK(runner["pid"].asInt() > 0);
    CHECK(runner["step_count"].asInt() >= 18);

    // 与 Python Runner 使用完全相同的规范（含概念 auth.cpt、内联表格、数据表驱动）
    HttpResult submit = request(s.port, "POST", "/api/v1/tests",
                                R"({"spec_files":["login.spec","calculator.spec","checkout.spec"],"name":"node"})");
    REQUIRE_EQ(submit.status, 202);
    Json st = s.waitForTerminal(submit.json()["test_id"].asString(), 20000);
    REQUIRE(!st.isNull());
    CHECK_EQ(st["state"].asString(), std::string("passed"));
    CHECK_EQ(st["total_scenarios"].asInt(), 12);
    CHECK_EQ(st["passed_scenarios"].asInt(), 12);

    Json result = request(s.port, "GET", "/api/v1/tests/" + st["test_id"].asString() + "/result").json();
    REQUIRE_EQ(result["specs"].size(), static_cast<size_t>(3));
    // after_scenario 钩子写入的消息随场景最后一步（teardown）/结果返回；DataTable 步骤消息可见
    const Json& checkout = result["specs"][2];
    CHECK_EQ(checkout["file"].asString(), std::string("checkout.spec"));
    const Json& multi = checkout["scenarios"][1];
    CHECK_EQ(multi["name"].asString(), std::string("购买多个商品"));
    bool sawTableMessage = false;
    for (const auto& step : multi["steps"].asArray()) {
        if (step["parameterized_text"].asString() == "批量加入以下商品 {}") {
            sawTableMessage = step["messages"].isArray() && step["messages"].size() == 1 &&
                              step["messages"][0].asString() == "added 2 line(s)";
        }
    }
    CHECK(sawTableMessage);

    // 断言失败 -> failed，且 Node 的 AssertionError 信息/堆栈被带回
    FileUtil::writeFile(s.specsDir + "/node_fail.spec",
                        "# Node 失败\n\n## 错误的结果\n* 输入第一个数 \"1\"\n* 输入第二个数 \"2\"\n* 点击加号\n* 结果应该是 \"4\"\n\n## 未实现的步骤\n* 这个步骤没有实现\n");
    submit = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["node_fail.spec"],"name":"node-fail"})");
    REQUIRE_EQ(submit.status, 202);
    st = s.waitForTerminal(submit.json()["test_id"].asString(), 20000);
    CHECK_EQ(st["state"].asString(), std::string("failed"));
    CHECK_EQ(st["failed_scenarios"].asInt(), 2);
    result = request(s.port, "GET", "/api/v1/tests/" + st["test_id"].asString() + "/result").json();
    const Json& wrong = result["specs"][0]["scenarios"][0];
    CHECK_EQ(wrong["state"].asString(), std::string("failed"));
    const Json& lastStep = wrong["steps"][3];
    CHECK_EQ(lastStep["state"].asString(), std::string("failed"));
    CHECK(lastStep["error"].asString().find("expected 4, got 3") != std::string::npos);
    CHECK(lastStep["stack_trace"].asString().find("AssertionError") != std::string::npos);
    const Json& missing = result["specs"][0]["scenarios"][1]["steps"][0];
    CHECK(missing["state"].asString() == "error" || missing["state"].asString() == "failed");
    CHECK(missing["error"].asString().find("No implementation") != std::string::npos);

    // 通过 HTTP 重启 Runner：新进程、同样的步骤数
    int oldPid = runner["pid"].asInt();
    HttpResult restart = request(s.port, "POST", "/api/v1/runner/restart", "{}");
    CHECK_EQ(restart.status, 200);
    CHECK(restart.json()["restarted"].asBool());
    CHECK(restart.json()["pid"].asInt() != oldPid);
    CHECK_EQ(restart.json()["step_count"].asInt(), runner["step_count"].asInt());

    expectSelfcheckPassed(s);
}

TEST_CASE("integration: parallel_streams split one test across runner processes") {
    if (std::system("python3 -c 'import sys' >/dev/null 2>&1") != 0) {
        std::cout << "    (python3 not available; skipping)\n";
        return;
    }
    std::string runnerDir = std::string(TESTHUB_SOURCE_DIR) + "/runners/python";
    Server s("", [&](TestHubConfig& cfg) {
        cfg.runnerLanguage = "python";
        cfg.runnerCommand = "python3 " + runnerDir + "/testhub_runner.py";
        cfg.projectPath = runnerDir;
        cfg.maxConcurrentTests = 1;   // 一个 worker 也能占用多个 Runner 进程
        cfg.runnerPoolSize = 2;
    });
    FileUtil::writeFile(s.specsDir + "/streams.spec",
                        "# 流\n\n## 甲\n* 等待 \"0.8\" 秒\n* 输入第一个数 \"1\"\n* 输入第二个数 \"2\"\n* 点击加号\n* 结果应该是 \"3\"\n\n"
                        "## 乙\n* 等待 \"0.8\" 秒\n* 输入第一个数 \"4\"\n* 输入第二个数 \"5\"\n* 点击加号\n* 结果应该是 \"9\"\n");
    auto start = std::chrono::steady_clock::now();
    HttpResult submit = request(s.port, "POST", "/api/v1/tests",
                                R"({"spec_files":["streams.spec"],"name":"streams","parallel_streams":2})");
    REQUIRE_EQ(submit.status, 202);
    Json st = s.waitForTerminal(submit.json()["test_id"].asString(), 15000);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK_EQ(st["state"].asString(), std::string("passed"));
    CHECK_EQ(st["total_scenarios"].asInt(), 2);
    CHECK_EQ(st["passed_scenarios"].asInt(), 2);
    CHECK_MSG(seconds < 1.5, "two 0.8 s scenarios in one test took " + std::to_string(seconds) +
                             " s; expected parallel_streams to overlap them");
    Json result = request(s.port, "GET", "/api/v1/tests/" + st["test_id"].asString() + "/result").json();
    REQUIRE_EQ(result["specs"][0]["scenarios"].size(), static_cast<size_t>(2));
    CHECK_EQ(result["specs"][0]["scenarios"][0]["name"].asString(), std::string("甲"));
    CHECK_EQ(result["specs"][0]["scenarios"][1]["name"].asString(), std::string("乙"));
    Json runner = request(s.port, "GET", "/api/v1/runner/status").json();
    CHECK_EQ(runner["pool_size"].asInt(), 2);
    CHECK(runner["runners"][0]["steps_executed"].asNumber() > 0);
    CHECK(runner["runners"][1]["steps_executed"].asNumber() > 0);
}

TEST_CASE("integration: step_retry retries a flaky mock step") {
    Server s;
    HttpResult put = request(s.port, "PUT", "/api/v1/specs/flaky.spec",
                             R"({"content":"# Flaky\n\n## Alpha\n* flaky alpha\n\n## Beta\n* flaky beta\n"})");
    CHECK_EQ(put.status, 201);

    HttpResult noRetry = request(s.port, "POST", "/api/v1/tests",
                                 R"({"spec_files":["flaky.spec"],"scenarios":["Alpha"],"name":"no-retry"})");
    REQUIRE_EQ(noRetry.status, 202);
    CHECK_EQ(s.waitForTerminal(noRetry.json()["test_id"].asString())["state"].asString(), std::string("failed"));
    Json failed = request(s.port, "GET",
                          "/api/v1/tests/" + noRetry.json()["test_id"].asString() + "/result").json();
    CHECK_EQ(failed["specs"][0]["scenarios"][0]["steps"][0]["attempts"].asInt(), 1);

    HttpResult yes = request(s.port, "POST", "/api/v1/tests",
                             R"({"spec_files":["flaky.spec"],"scenarios":["Beta"],"step_retry":1,"name":"retry"})");
    REQUIRE_EQ(yes.status, 202);
    std::string id = yes.json()["test_id"].asString();
    CHECK_EQ(s.waitForTerminal(id)["state"].asString(), std::string("passed"));
    Json ok = request(s.port, "GET", "/api/v1/tests/" + id + "/result").json();
    CHECK_EQ(ok["specs"][0]["scenarios"][0]["steps"][0]["state"].asString(), std::string("passed"));
    CHECK_EQ(ok["specs"][0]["scenarios"][0]["steps"][0]["attempts"].asInt(), 2);
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests/" + id).json()["request"]["step_retry"].asInt(), 1);
}

TEST_CASE("integration: selfcheck spec verifies the running server through its own HTTP API") {
    if (std::system("python3 -c 'import sys' >/dev/null 2>&1") != 0) {
        std::cout << "    (python3 not available; skipping)\n";
        return;
    }
    std::string runnerDir = std::string(TESTHUB_SOURCE_DIR) + "/runners/python";
    Server s("", [&](TestHubConfig& cfg) {
        cfg.runnerLanguage = "python";
        cfg.runnerCommand = "python3 " + runnerDir + "/testhub_runner.py";
        cfg.projectPath = runnerDir;
        cfg.maxConcurrentTests = 1;  // 提交子测试但不在步骤里等待，避免占满 worker
    });
    expectSelfcheckPassed(s);
}

TEST_CASE("integration: schedules CRUD, fire-now, invalid cron") {
    Server s;
    CHECK_EQ(request(s.port, "GET", "/api/v1/status").json()["scheduler"]["count"].asInt(), 0);

    HttpResult bad = request(s.port, "POST", "/api/v1/schedules",
                             R"({"name":"bad","cron":"60 * * * *","spec_files":["login.spec"]})");
    CHECK_EQ(bad.status, 400);

    HttpResult created = request(s.port, "POST", "/api/v1/schedules",
                                 R"({"name":"hourly-login","cron":"@hourly","spec_files":["login.spec"],"tags":["smoke"]})");
    REQUIRE_EQ(created.status, 201);
    Json plan = created.json();
    std::string id = plan["id"].asString();
    REQUIRE(!id.empty());
    CHECK_EQ(plan["cron"].asString(), std::string("@hourly"));
    CHECK(plan["enabled"].asBool());
    CHECK(!plan["next_run_at"].asString("").empty());

    Json listed = request(s.port, "GET", "/api/v1/schedules").json();
    CHECK_EQ(listed["count"].asInt(), 1);
    CHECK_EQ(request(s.port, "GET", "/api/v1/schedules/" + id).json()["name"].asString(), std::string("hourly-login"));

    HttpResult run = request(s.port, "POST", "/api/v1/schedules/" + id + "/run");
    REQUIRE_EQ(run.status, 202);
    std::string testId = run.json()["test_id"].asString();
    REQUIRE(!testId.empty());
    Json st = s.waitForTerminal(testId);
    REQUIRE(!st.isNull());
    CHECK_EQ(st["state"].asString(), std::string("passed"));
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests/" + testId).json()["request"]["submitted_by"].asString(),
             std::string("schedule:") + id);

    HttpResult disable = request(s.port, "PUT", "/api/v1/schedules/" + id, R"({"enabled":false})");
    CHECK_EQ(disable.status, 200);
    CHECK(!disable.json()["enabled"].asBool());

    CHECK_EQ(request(s.port, "DELETE", "/api/v1/schedules/" + id).status, 200);
    CHECK_EQ(request(s.port, "GET", "/api/v1/schedules/" + id).status, 404);
    CHECK_EQ(request(s.port, "GET", "/api/v1/schedules").json()["count"].asInt(), 0);
}

TEST_CASE("integration: trends and compare against previous login.spec run") {
    Server s;
    CHECK_EQ(request(s.port, "GET", "/api/v1/trends").json()["count"].asInt(), 0);
    CHECK_EQ(request(s.port, "GET", "/api/v1/tests/nope/compare").status, 404);

    auto runLogin = [&](const char* name) {
        HttpResult submit = request(s.port, "POST", "/api/v1/tests",
                                    std::string("{\"spec_files\":[\"login.spec\"],\"name\":\"") + name + "\"}");
        REQUIRE_EQ(submit.status, 202);
        std::string id = submit.json()["test_id"].asString();
        Json st = s.waitForTerminal(id);
        REQUIRE(!st.isNull());
        CHECK_EQ(st["state"].asString(), std::string("passed"));
        return id;
    };
    std::string first = runLogin("trend-a");
    std::string second = runLogin("trend-b");

    HttpResult trends = request(s.port, "GET", "/api/v1/trends?spec=login.spec&limit=10");
    REQUIRE_EQ(trends.status, 200);
    Json series = trends.json();
    REQUIRE_EQ(series["count"].asInt(), 1);
    CHECK_EQ(series["specs"][0]["spec"].asString(), std::string("login.spec"));
    CHECK(series["specs"][0]["runs"].asInt() >= 2);
    CHECK(series["specs"][0]["latest_pass_rate"].asNumber() > 0.99);
    CHECK_EQ(series["specs"][0]["points"].size(), static_cast<size_t>(series["specs"][0]["runs"].asInt()));

    Json all = request(s.port, "GET", "/api/v1/trends").json();
    CHECK(all["count"].asInt() >= 2);
    CHECK_EQ(all["specs"][0]["spec"].asString(), std::string("(all)"));

    HttpResult self = request(s.port, "GET", "/api/v1/tests/" + second + "/compare?with=" + second);
    CHECK_EQ(self.status, 400);

    HttpResult cmp = request(s.port, "GET", "/api/v1/tests/" + second + "/compare");
    REQUIRE_EQ(cmp.status, 200);
    Json body = cmp.json();
    CHECK(body["baseline_auto"].asBool());
    CHECK_EQ(body["baseline"]["test_id"].asString(), first);
    CHECK_EQ(body["current"]["test_id"].asString(), second);
    CHECK_EQ(body["summary"]["unchanged"].asInt(), 3);
    CHECK_EQ(body["summary"]["regressed"].asInt(), 0);
    CHECK_EQ(body["count"].asInt(), 3);

    HttpResult with = request(s.port, "GET", "/api/v1/tests/" + second + "/compare?with=" + first);
    CHECK_EQ(with.status, 200);
    CHECK(!with.json()["baseline_auto"].asBool());
}

TEST_CASE("integration: switch spec projects") {
    std::string alt = makeTempDir("testhub-alt-");
    FileUtil::writeFile(alt + "/hello.spec", "# Hello\n\n## S\n\n* sleep \"0\"\n");
    Server s("", [&](TestHubConfig& cfg) {
        SpecProject main;
        main.id = "main";
        main.name = "主规范";
        main.dir = cfg.specsDir;
        SpecProject other;
        other.id = "alt";
        other.name = "备用";
        other.dir = alt;
        cfg.projects = {main, other};
        cfg.currentProjectId = "main";
    });

    HttpResult listed = request(s.port, "GET", "/api/v1/projects");
    REQUIRE_EQ(listed.status, 200);
    CHECK_EQ(listed.json()["count"].asInt(), 2);
    CHECK_EQ(listed.json()["current"].asString(), std::string("main"));
    CHECK_EQ(request(s.port, "GET", "/api/v1/status").json()["current_project"].asString(), std::string("main"));

    Json specs = request(s.port, "GET", "/api/v1/specs").json();
    CHECK_EQ(specs["current_project"].asString(), std::string("main"));
    bool hasLogin = false, hasHello = false;
    for (const auto& f : specs["specs"].asArray()) {
        if (f["file"].asString() == "login.spec") hasLogin = true;
        if (f["file"].asString() == "hello.spec") hasHello = true;
    }
    CHECK(hasLogin);
    CHECK(!hasHello);

    CHECK_EQ(request(s.port, "POST", "/api/v1/projects/nope/select", "{}").status, 404);

    HttpResult sw = request(s.port, "POST", "/api/v1/projects/alt/select", "{}");
    REQUIRE_EQ(sw.status, 200);
    CHECK_EQ(sw.json()["current"].asString(), std::string("alt"));
    Json altSpecs = request(s.port, "GET", "/api/v1/specs").json();
    hasLogin = false;
    hasHello = false;
    for (const auto& f : altSpecs["specs"].asArray()) {
        if (f["file"].asString() == "login.spec") hasLogin = true;
        if (f["file"].asString() == "hello.spec") hasHello = true;
    }
    CHECK(hasHello);
    CHECK(!hasLogin);
    CHECK_EQ(request(s.port, "GET", "/api/v1/status").json()["current_project"].asString(), std::string("alt"));
    CHECK_EQ(request(s.port, "POST", "/api/v1/projects/alt/select", "{}").status, 200);

    REQUIRE_EQ(request(s.port, "POST", "/api/v1/projects/main/select", "{}").status, 200);
    std::string slow = "# Slow\n## s\n* sleep \"800\"\n";
    FileUtil::writeFile(s.specsDir + "/slow.spec", slow);
    HttpResult submit = request(s.port, "POST", "/api/v1/tests", R"({"spec_files":["slow.spec"]})");
    REQUIRE_EQ(submit.status, 202);
    CHECK_EQ(request(s.port, "POST", "/api/v1/projects/alt/select", "{}").status, 409);
    Json st = s.waitForTerminal(submit.json()["test_id"].asString());
    REQUIRE(!st.isNull());
    CHECK_EQ(request(s.port, "POST", "/api/v1/projects/alt/select", "{}").status, 200);

    std::error_code ec;
    fs::remove_all(alt, ec);
}
