/*
 * HttpClient 与 CallbackNotifier::buildPayload 单元测试
 */

#include "test_framework.h"

#include "../src/engine/execution_engine.h"
#include "../src/notify/callback_notifier.h"
#include "../src/server/http_server.h"
#include "../src/util/http_client.h"

#include <chrono>
#include <thread>

using namespace testhub;

TEST_CASE("http client: url parsing") {
    ParsedUrl u;
    std::string err;
    REQUIRE(HttpClient::parseUrl("http://example.com/hook?x=1#frag", u, &err));
    CHECK_EQ(u.host, std::string("example.com"));
    CHECK_EQ(u.port, 80);
    CHECK_EQ(u.path, std::string("/hook?x=1"));

    REQUIRE(HttpClient::parseUrl("HTTP://user:pw@10.0.0.1:9090", u));
    CHECK_EQ(u.host, std::string("10.0.0.1"));
    CHECK_EQ(u.port, 9090);
    CHECK_EQ(u.path, std::string("/"));

    REQUIRE(HttpClient::parseUrl("http://[::1]:8081/a", u));
    CHECK_EQ(u.host, std::string("::1"));
    CHECK_EQ(u.port, 8081);

    CHECK(!HttpClient::parseUrl("https://example.com/", u, &err));
    CHECK(err.find("https") != std::string::npos);
    CHECK(!HttpClient::parseUrl("example.com/hook", u, &err));
    CHECK(!HttpClient::parseUrl("http://", u, &err));
    CHECK(!HttpClient::parseUrl("http://host:99999/", u, &err));
    CHECK(!HttpClient::parseUrl("ftp://host/", u, &err));
}

TEST_CASE("http client: post against local server, errors and timeouts") {
    HttpServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.workerThreads = 2;
    cfg.logRequests = false;
    HttpServer server(cfg);
    server.post("/echo", [](const HttpRequest& req) {
        Json j = Json::object();
        j["body"] = req.body;
        j["ct"] = req.header("content-type");
        j["custom"] = req.header("x-custom");
        j["ua"] = req.header("user-agent");
        return HttpResponse::json(201, j);
    });
    server.get("/slow", [](const HttpRequest&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return HttpResponse::text(200, "late");
    });
    server.get("/empty", [](const HttpRequest&) { return HttpResponse::noContent(); });
    REQUIRE(server.start());
    std::string base = "http://127.0.0.1:" + std::to_string(server.port());

    HttpClientResponse r = HttpClient::post(base + "/echo", "{\"a\":1}", "application/json", 2000, {{"X-Custom", "yes"}});
    REQUIRE(r.ok);
    CHECK_EQ(r.status, 201);
    Json j = Json::tryParse(r.body);
    CHECK_EQ(j["body"].asString(), std::string("{\"a\":1}"));
    CHECK_EQ(j["ct"].asString(), std::string("application/json"));
    CHECK_EQ(j["custom"].asString(), std::string("yes"));
    CHECK_EQ(j["ua"].asString(), std::string("TestHub"));
    CHECK(r.headers.count("content-type") == 1);

    HttpClientResponse nf = HttpClient::request("GET", base + "/missing");
    CHECK(nf.ok);
    CHECK_EQ(nf.status, 404);

    HttpClientResponse empty = HttpClient::request("GET", base + "/empty");
    CHECK(empty.ok);
    CHECK_EQ(empty.status, 204);
    CHECK(empty.body.empty());

    HttpClientResponse slow = HttpClient::request("GET", base + "/slow", "", {}, 100);
    CHECK(!slow.ok);
    CHECK(slow.error.find("timed out") != std::string::npos);

    server.stop();
    HttpClientResponse refused = HttpClient::request("GET", base + "/echo", "", {}, 1000);
    CHECK(!refused.ok);
    CHECK(!refused.error.empty());

    HttpClientResponse bad = HttpClient::post("https://127.0.0.1/", "{}");
    CHECK(!bad.ok);
    CHECK(bad.error.find("https") != std::string::npos);
}

TEST_CASE("callback: payload summarises the record and links to reports") {
    TestRecord rec;
    rec.request.id = "test-1";
    rec.request.name = "nightly";
    rec.request.specFiles = {"a.spec"};
    rec.request.tags = {"smoke"};
    rec.request.metadata["build"] = "9";
    rec.status.state = TestState::FAILED;
    rec.status.totalScenarios = 2;
    rec.status.executedScenarios = 2;
    rec.status.passedScenarios = 1;
    rec.status.failedScenarios = 1;
    rec.hasResult = true;
    rec.result.totalDuration = 2.5;
    rec.result.errors = {"e1"};
    SpecResult sr;
    sr.specFile = "a.spec";
    ScenarioResult ok;
    ok.scenarioName = "ok";
    ScenarioResult bad;
    bad.scenarioName = "bad";
    bad.state = TestState::FAILED;
    bad.errorMessage = "boom";
    bad.dataRowIndex = 1;
    sr.scenarioResults = {ok, bad};
    rec.result.specResults.push_back(sr);

    Json p = CallbackNotifier::buildPayload(rec, "test.completed", "http://ci.local:8080");
    CHECK_EQ(p["event"].asString(), std::string("test.completed"));
    CHECK_EQ(p["test_id"].asString(), std::string("test-1"));
    CHECK_EQ(p["state"].asString(), std::string("failed"));
    CHECK_EQ(p["duration"].asNumber(), 2.5);
    CHECK_EQ(p["metadata"]["build"].asString(), std::string("9"));
    CHECK_EQ(p["errors"][0].asString(), std::string("e1"));
    REQUIRE_EQ(p["failed_scenarios_detail"].size(), static_cast<size_t>(1));
    CHECK_EQ(p["failed_scenarios_detail"][0]["scenario"].asString(), std::string("bad"));
    CHECK_EQ(p["failed_scenarios_detail"][0]["data_row_index"].asInt(), 1);
    CHECK_EQ(p["failed_scenarios_detail"][0]["error"].asString(), std::string("boom"));
    CHECK_EQ(p["links"]["status"].asString(), std::string("http://ci.local:8080/api/v1/tests/test-1"));
    CHECK_EQ(p["links"]["report_html"].asString(), std::string("http://ci.local:8080/api/v1/tests/test-1/report?format=html"));
    CHECK_EQ(p["links"]["ui"].asString(), std::string("http://ci.local:8080/#/tests/test-1"));
    CHECK(!p["sent_at"].asString().empty());

    CHECK(CallbackNotifier::isRetryableStatus(503));
    CHECK(CallbackNotifier::isRetryableStatus(429));
    CHECK(!CallbackNotifier::isRetryableStatus(404));
    CHECK(!CallbackNotifier::isRetryableStatus(200));
}
