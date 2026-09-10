#include "test_framework.h"
#include "engine/execution_engine.h"
#include "util/file_util.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <unistd.h>

using namespace testhub;
namespace fs = std::filesystem;

namespace {

struct TempSpecs {
    std::string dir;
    TempSpecs() {
        dir = (fs::temp_directory_path() / ("testhub-engine-" + std::to_string(::getpid()) + "-" +
                                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
        fs::create_directories(dir);
    }
    ~TempSpecs() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const std::string& name, const std::string& content) { FileUtil::writeFile(dir + "/" + name, content); }
};

struct Harness {
    TempSpecs temp;
    spec::SpecRepository repo;
    RunnerBridge bridge;
    ExecutionEngine engine{repo, bridge};

    explicit Harness(int workers = 1) {
        repo.configure(temp.dir, "");
        RunnerConfig rc;
        rc.language = "mock";
        bridge.start(rc);
        EngineConfig ec;
        ec.workerThreads = workers;
        ec.historyLimit = 5;
        engine.configure(ec);
        engine.start();
    }
    ~Harness() {
        engine.stop();
        bridge.stopRunner();
    }

    TestStatus waitFor(const std::string& id, int timeoutMs = 10000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            auto st = engine.getStatus(id);
            if (st && isTerminalState(st->state)) return *st;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto st = engine.getStatus(id);
        return st ? *st : TestStatus{};
    }
};

const char* kPassing = "# 通过\ntags: smoke\n\n## 场景一\ntags: fast\n* 步骤 A\n* 步骤 B \"x\"\n\n## 场景二\ntags: slow\n* 步骤 C\n";
const char* kFailing = "# 失败\n* 上下文步骤\n\n## 会失败\n* 这一步 fail\n* 不应执行\n\n## 会出错\n* 这一步 error\n\n## 正常\n* ok\n___\n* 清理步骤\n";
const char* kDataDriven = "# 数据\n|name|\n|----|\n|a|\n|b|\n|c|\n\n## 行场景\n* 使用 <name>\n";

}

TEST_CASE("engine: runs passing spec and records result") {
    Harness h;
    h.temp.write("pass.spec", kPassing);
    TestRequest req;
    req.specFiles = {"pass.spec"};
    req.name = "smoke run";
    std::string id = h.engine.submit(req);
    REQUIRE(!id.empty());
    TestStatus st = h.waitFor(id);
    CHECK(st.state == TestState::PASSED);
    CHECK_EQ(st.totalScenarios, 2);
    CHECK_EQ(st.passedScenarios, 2);
    CHECK_EQ(st.progress, 1.0);

    auto result = h.engine.getResult(id);
    REQUIRE(result.has_value());
    REQUIRE_EQ(result->specResults.size(), static_cast<size_t>(1));
    CHECK_EQ(result->specResults[0].scenarioResults.size(), static_cast<size_t>(2));
    CHECK_EQ(result->specResults[0].scenarioResults[0].stepResults.size(), static_cast<size_t>(2));
    CHECK(result->specResults[0].scenarioResults[0].stepResults[1].messages.size() >= 1);
    CHECK(h.engine.exists(id));
    CHECK_EQ(h.engine.count(), static_cast<size_t>(1));
    CHECK_EQ(h.engine.list()[0].name, std::string("smoke run"));
}

TEST_CASE("engine: failures, errors, contexts and teardowns") {
    Harness h;
    h.temp.write("fail.spec", kFailing);
    TestRequest req;
    req.specFiles = {"fail.spec"};
    std::string id = h.engine.submit(req);
    TestStatus st = h.waitFor(id);
    CHECK(st.state == TestState::FAILED);
    CHECK_EQ(st.totalScenarios, 3);
    CHECK_EQ(st.passedScenarios, 1);
    CHECK_EQ(st.failedScenarios, 2);

    auto result = h.engine.getResult(id);
    REQUIRE(result.has_value());
    const SpecResult& spec = result->specResults[0];
    REQUIRE_EQ(spec.scenarioResults.size(), static_cast<size_t>(3));
    const ScenarioResult& failing = spec.scenarioResults[0];
    CHECK(failing.state == TestState::FAILED);
    CHECK_EQ(failing.contextSteps.size(), static_cast<size_t>(1));
    REQUIRE_EQ(failing.stepResults.size(), static_cast<size_t>(2));
    CHECK(failing.stepResults[0].state == TestState::FAILED);
    CHECK(failing.stepResults[1].state == TestState::SKIPPED);
    CHECK_EQ(failing.teardownSteps.size(), static_cast<size_t>(1));
    CHECK(spec.scenarioResults[1].state == TestState::TEST_ERROR);
    CHECK(spec.scenarioResults[2].state == TestState::PASSED);
}

TEST_CASE("engine: tag filter selects scenarios") {
    Harness h;
    h.temp.write("pass.spec", kPassing);
    TestRequest req;
    req.specFiles = {"pass.spec"};
    req.tags = {"smoke & !slow"};
    std::string id = h.engine.submit(req);
    TestStatus st = h.waitFor(id);
    CHECK(st.state == TestState::PASSED);
    CHECK_EQ(st.totalScenarios, 1);
    auto result = h.engine.getResult(id);
    REQUIRE(result.has_value());
    REQUIRE_EQ(result->specResults[0].scenarioResults.size(), static_cast<size_t>(1));
    CHECK_EQ(result->specResults[0].scenarioResults[0].scenarioName, std::string("场景一"));

    TestRequest none;
    none.specFiles = {"pass.spec"};
    none.tags = {"nonexistent"};
    std::string id2 = h.engine.submit(none);
    TestStatus st2 = h.waitFor(id2);
    CHECK_EQ(st2.totalScenarios, 0);
    CHECK(isTerminalState(st2.state));
}

TEST_CASE("engine: data driven spec runs one scenario per row") {
    Harness h;
    h.temp.write("data.spec", kDataDriven);
    TestRequest req;
    req.specFiles = {"data.spec"};
    std::string id = h.engine.submit(req);
    TestStatus st = h.waitFor(id);
    CHECK(st.state == TestState::PASSED);
    CHECK_EQ(st.totalScenarios, 3);
    auto result = h.engine.getResult(id);
    REQUIRE(result.has_value());
    REQUIRE_EQ(result->specResults[0].scenarioResults.size(), static_cast<size_t>(3));
    CHECK_EQ(result->specResults[0].scenarioResults[1].dataRowIndex, 1);
    CHECK_EQ(result->specResults[0].scenarioResults[2].dataRow.at("name"), std::string("c"));
    // 动态参数已被替换为静态值并传给 runner
    CHECK_EQ(result->specResults[0].scenarioResults[0].stepResults[0].stepText, std::string("使用 <name>"));
    CHECK(result->specResults[0].scenarioResults[0].stepResults[0].messages[0].find("arg[static]=a") != std::string::npos);
}

TEST_CASE("engine: invalid requests are rejected") {
    Harness h;
    TestRequest empty;
    CHECK_THROWS(h.engine.submit(empty), std::invalid_argument);
    TestRequest missing;
    missing.specFiles = {"does-not-exist.spec"};
    CHECK_THROWS(h.engine.submit(missing), std::invalid_argument);
    TestRequest badTag;
    h.temp.write("pass.spec", kPassing);
    badTag.specFiles = {"pass.spec"};
    badTag.tags = {"(unbalanced"};
    CHECK_THROWS(h.engine.submit(badTag), std::invalid_argument);
    CHECK(!h.engine.getStatus("nope").has_value());
    CHECK(!h.engine.cancel("nope"));
}

TEST_CASE("engine: cancel running test and fail_fast") {
    Harness h;
    std::string slow = "# 慢\n";
    for (int i = 0; i < 20; ++i) slow += "## s" + std::to_string(i) + "\n* sleep \"100\"\n\n";
    h.temp.write("slow.spec", slow);
    TestRequest req;
    req.specFiles = {"slow.spec"};
    std::string id = h.engine.submit(req);
    // 等待开始
    for (int i = 0; i < 200; ++i) {
        auto st = h.engine.getStatus(id);
        if (st && st->state == TestState::RUNNING) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(h.engine.cancel(id));
    TestStatus st = h.waitFor(id);
    CHECK(st.state == TestState::CANCELLED);
    CHECK(st.totalScenarios == 20);
    CHECK(st.executedScenarios < 20);

    TestRequest ff;
    h.temp.write("fail.spec", kFailing);
    ff.specFiles = {"fail.spec"};
    ff.failFast = true;
    std::string id2 = h.engine.submit(ff);
    TestStatus st2 = h.waitFor(id2);
    CHECK(st2.state == TestState::FAILED);
    auto result = h.engine.getResult(id2);
    REQUIRE(result.has_value());
    // 首个场景失败后停止，其余标记为跳过
    CHECK_EQ(result->specResults[0].scenarioResults.size(), static_cast<size_t>(3));
    CHECK(result->specResults[0].scenarioResults[1].state == TestState::SKIPPED);
    CHECK(result->specResults[0].scenarioResults[2].state == TestState::SKIPPED);
}

TEST_CASE("engine: test timeout marks result as error") {
    Harness h;
    std::string slow = "# 慢\n## a\n* sleep \"400\"\n\n## b\n* sleep \"400\"\n";
    h.temp.write("slow.spec", slow);
    TestRequest req;
    req.specFiles = {"slow.spec"};
    req.timeoutMs = 150;
    std::string id = h.engine.submit(req);
    TestStatus st = h.waitFor(id, 5000);
    CHECK(st.state == TestState::TEST_ERROR || st.state == TestState::FAILED);
    REQUIRE(!st.errors.empty());
    CHECK(st.errors[0].find("time") != std::string::npos);
}

TEST_CASE("engine: cancel queued test, queue position and history trimming") {
    Harness h;
    std::string slow = "# 慢\n## a\n* sleep \"150\"\n";
    h.temp.write("slow.spec", slow);
    h.temp.write("pass.spec", kPassing);
    TestRequest s;
    s.specFiles = {"slow.spec"};
    std::string running = h.engine.submit(s);
    TestRequest q;
    q.specFiles = {"pass.spec"};
    std::string queued1 = h.engine.submit(q);
    std::string queued2 = h.engine.submit(q);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_EQ(h.engine.queuePosition(queued1), 0);
    CHECK_EQ(h.engine.queuePosition(queued2), 1);
    CHECK(h.engine.cancel(queued2));
    CHECK(h.waitFor(queued2).state == TestState::CANCELLED);
    CHECK(h.waitFor(running).state == TestState::PASSED);
    CHECK(h.waitFor(queued1).state == TestState::PASSED);

    // 高优先级插队
    for (int i = 0; i < 6; ++i) h.waitFor(h.engine.submit(q));
    // historyLimit=5：旧的终态记录被裁剪
    CHECK(h.engine.count() <= 5);

    std::string rerun = h.engine.rerun(h.engine.list()[0].testId);
    CHECK(!rerun.empty());
    CHECK(h.waitFor(rerun).state == TestState::PASSED);
    CHECK(h.engine.remove(rerun));
    CHECK(!h.engine.exists(rerun));
    EngineStats stats = h.engine.stats();
    CHECK(stats.completed >= 9);
    CHECK(stats.cancelled >= 1);
}

TEST_CASE("engine: concurrent workers execute in parallel") {
    Harness h(3);
    std::string slow = "# 慢\n## a\n* sleep \"200\"\n";
    h.temp.write("slow.spec", slow);
    TestRequest req;
    req.specFiles = {"slow.spec"};
    auto start = std::chrono::steady_clock::now();
    std::vector<std::string> ids;
    for (int i = 0; i < 3; ++i) ids.push_back(h.engine.submit(req));
    for (const auto& id : ids) CHECK(h.waitFor(id).state == TestState::PASSED);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK_MSG(ms < 500, "took " + std::to_string(ms) + "ms; expected parallel execution");
}
