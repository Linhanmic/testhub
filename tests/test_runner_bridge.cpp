/*
 * RunnerBridge 单元测试：Runner 池的槽位分配、会话绑定、并行执行、逐槽自愈、钩子广播与生命周期。
 * 通过 setRunnerFactory 注入一个可观测的假 Runner，无需外部进程。
 */

#include "test_framework.h"
#include "runner/runner_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>

using namespace testhub;

namespace {

/**
 * 记录每一次调用落在哪个 Runner 实例上的假 Runner（非并发安全，模拟有状态的进程 Runner）
 */
struct FakeWorld {
    std::mutex mutex;
    std::atomic<int> created{0};
    std::atomic<int> inFlight{0};
    std::atomic<int> maxInFlight{0};
    std::vector<std::pair<int, std::string>> steps;   // (实例 id, 步骤文本)
    std::vector<std::pair<int, HookType>> hooks;      // (实例 id, 钩子)
    bool concurrencySafe = false;
};

class FakeRunner : public Runner {
public:
    explicit FakeRunner(FakeWorld& world) : world_(world), id_(++world.created) {}

    bool start() override { alive_ = true; return true; }
    void stop() override { alive_ = false; }
    bool isAlive() const override { return alive_; }
    std::string name() const override { return "fake#" + std::to_string(id_); }
    int pid() const override { return 1000 + id_; }
    std::string version() const override { return "fake"; }
    bool isConcurrencySafe() const override { return world_.concurrencySafe; }

    StepResult executeStep(const StepExecutionRequest& request) override {
        int now = ++world_.inFlight;
        int seen = world_.maxInFlight.load();
        while (now > seen && !world_.maxInFlight.compare_exchange_weak(seen, now)) {}
        {
            std::lock_guard<std::mutex> lock(world_.mutex);
            world_.steps.emplace_back(id_, request.stepText);
        }
        size_t pos = request.stepText.find("sleep ");
        if (pos != std::string::npos) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(request.stepText.c_str() + pos + 6)));
        }
        --world_.inFlight;
        StepResult r;
        r.stepText = request.stepText;
        r.state = TestState::PASSED;
        if (request.stepText.find("crash") != std::string::npos) {
            alive_ = false;
            r.state = TestState::TEST_ERROR;
            r.errorMessage = "runner crashed";
        }
        return r;
    }

    HookResult runHook(HookType type, const ExecutionContext&) override {
        std::lock_guard<std::mutex> lock(world_.mutex);
        world_.hooks.emplace_back(id_, type);
        return HookResult{};
    }

    std::vector<StepValue> getAllSteps() override {
        StepValue v;
        v.stepText = "fake step";
        v.parameterizedStepText = "fake step";
        return {v};
    }

    int id() const { return id_; }

private:
    FakeWorld& world_;
    int id_;
    std::atomic<bool> alive_{false};
};

struct PoolFixture {
    FakeWorld world;
    RunnerBridge bridge;

    explicit PoolFixture(int poolSize, int maxRestarts = 5, bool autoRestart = true) {
        bridge.setRunnerFactory([this] { return std::make_unique<FakeRunner>(world); });
        RunnerConfig cfg;
        cfg.language = "fake";
        cfg.poolSize = poolSize;
        cfg.maxRestarts = maxRestarts;
        cfg.autoRestart = autoRestart;
        REQUIRE(bridge.start(cfg));
    }
    ~PoolFixture() { bridge.stopRunner(); }

    StepExecutionRequest step(const std::string& text) {
        StepExecutionRequest r;
        r.stepText = text;
        r.parameterizedText = text;
        return r;
    }

    std::set<int> instancesFor(const std::string& stepPrefix) {
        std::lock_guard<std::mutex> lock(world.mutex);
        std::set<int> ids;
        for (const auto& s : world.steps) {
            if (s.second.rfind(stepPrefix, 0) == 0) ids.insert(s.first);
        }
        return ids;
    }
};

struct Barrier {
    explicit Barrier(int n) : n_(n) {}
    void wait() {
        ++arrived_;
        while (arrived_.load() < n_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int n_;
    std::atomic<int> arrived_{0};
};

const RunnerSlotStatus& slotStatus(const RunnerStatus& st, int index) {
    for (const auto& s : st.slots) if (s.index == index) return s;
    throw std::runtime_error("slot not found");
}

} // namespace

TEST_CASE("runner pool: scenarios run in parallel on distinct runners and stay pinned to their session") {
    PoolFixture f(3);
    RunnerStatus st = f.bridge.getStatus();
    CHECK_EQ(st.poolSize, 3);
    CHECK_EQ(st.aliveCount, 3);
    CHECK_EQ(st.busyCount, 0);
    CHECK(st.state == RunnerState::CONNECTED);
    CHECK_EQ(st.slots.size(), static_cast<size_t>(3));
    CHECK_EQ(f.world.created.load(), 3);
    std::set<int> pids;
    for (const auto& s : st.slots) pids.insert(s.pid);
    CHECK_EQ(pids.size(), static_cast<size_t>(3));

    Barrier allAcquired(3);
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&f, &allAcquired, i] {
            RunnerBridge::Session session = f.bridge.acquireSession();
            REQUIRE(session.active());
            allAcquired.wait();   // 三个会话同时持有槽位，保证它们拿到的是三个不同的 Runner
            std::string tag = "s" + std::to_string(i) + " ";
            for (int k = 0; k < 3; ++k) {
                StepResult r = f.bridge.executeStep(f.step(tag + "sleep 60"));
                CHECK(r.state == TestState::PASSED);
            }
        });
    }
    for (auto& t : threads) t.join();

    CHECK_EQ(f.world.maxInFlight.load(), 3);
    std::set<int> all;
    for (int i = 0; i < 3; ++i) {
        std::set<int> ids = f.instancesFor("s" + std::to_string(i) + " ");
        CHECK_EQ(ids.size(), static_cast<size_t>(1));   // 同一会话的步骤全部落在同一个 Runner
        all.insert(ids.begin(), ids.end());
    }
    CHECK_EQ(all.size(), static_cast<size_t>(3));       // 三个会话使用了三个不同的 Runner

    st = f.bridge.getStatus();
    CHECK_EQ(st.busyCount, 0);
    unsigned long long total = 0;
    for (const auto& s : st.slots) {
        CHECK_EQ(s.stepsExecuted, 3ULL);
        total += s.stepsExecuted;
    }
    CHECK_EQ(total, 9ULL);
}

TEST_CASE("runner pool: acquireSession blocks until a slot is free and reports busy state") {
    PoolFixture f(1);
    std::atomic<bool> holderReady{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        RunnerBridge::Session session = f.bridge.acquireSession();
        holderReady = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });
    while (!holderReady) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    RunnerStatus st = f.bridge.getStatus();
    CHECK_EQ(st.busyCount, 1);
    CHECK(st.state == RunnerState::BUSY);
    CHECK(slotStatus(st, 0).busy);

    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        RunnerBridge::Session session = f.bridge.acquireSession();
        acquired = session.active();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(!acquired.load());
    release = true;
    holder.join();
    waiter.join();
    CHECK(acquired.load());
    CHECK_EQ(f.bridge.getStatus().busyCount, 0);
}

TEST_CASE("runner pool: a crashed runner is restarted on its own slot only") {
    PoolFixture f(2);
    int crashedInstance = 0;
    {
        RunnerBridge::Session session = f.bridge.acquireSession();
        REQUIRE(session.active());
        StepResult r = f.bridge.executeStep(f.step("about to crash"));
        CHECK(r.state == TestState::TEST_ERROR);
        crashedInstance = *f.instancesFor("about to crash").begin();
        // 同一会话内继续执行：桥接应在同一槽位上重启 Runner
        StepResult again = f.bridge.executeStep(f.step("after restart"));
        CHECK(again.state == TestState::PASSED);
        int restarted = *f.instancesFor("after restart").begin();
        CHECK(restarted != crashedInstance);
        CHECK_EQ(session.slotIndex(), 0);
    }
    CHECK_EQ(f.world.created.load(), 3);
    RunnerStatus st = f.bridge.getStatus();
    CHECK_EQ(st.aliveCount, 2);
    CHECK_EQ(st.restartCount, 1);
    CHECK_EQ(slotStatus(st, 0).restartCount, 1);
    CHECK_EQ(slotStatus(st, 1).restartCount, 0);
    CHECK(slotStatus(st, 1).pid != 1000 + crashedInstance);
}

TEST_CASE("runner pool: restart limit is per slot and healthy slots are preferred") {
    PoolFixture f(2, /*maxRestarts=*/0);
    {
        RunnerBridge::Session session = f.bridge.acquireSession();
        CHECK(f.bridge.executeStep(f.step("crash one")).state == TestState::TEST_ERROR);
    }
    RunnerStatus st = f.bridge.getStatus();
    CHECK_EQ(st.aliveCount, 1);
    CHECK(st.state == RunnerState::CONNECTED);     // 池仍可用（降级）
    CHECK(!st.lastError.empty());
    CHECK(slotStatus(st, 0).state == RunnerState::RUNNER_ERROR);

    // 新会话应绕开失效槽位
    {
        RunnerBridge::Session session = f.bridge.acquireSession();
        REQUIRE(session.active());
        CHECK_EQ(session.slotIndex(), 1);
        CHECK(f.bridge.executeStep(f.step("still fine")).state == TestState::PASSED);
        CHECK(f.bridge.executeStep(f.step("crash two")).state == TestState::TEST_ERROR);
    }
    // 全部失效：不应无限等待，而是返回明确错误
    {
        RunnerBridge::Session session = f.bridge.acquireSession();
        REQUIRE(session.active());
        StepResult r = f.bridge.executeStep(f.step("nothing left"));
        CHECK(r.state == TestState::TEST_ERROR);
        CHECK(r.errorMessage.find("restart limit") != std::string::npos);
    }
    st = f.bridge.getStatus();
    CHECK_EQ(st.aliveCount, 0);
    CHECK(st.state == RunnerState::RUNNER_ERROR);
    CHECK(!f.bridge.isConnected());
    CHECK_EQ(f.world.created.load(), 2);

    // 手动重启让池恢复
    CHECK(f.bridge.restartRunner());
    st = f.bridge.getStatus();
    CHECK_EQ(st.aliveCount, 2);
    CHECK_EQ(f.world.created.load(), 4);
}

TEST_CASE("runner pool: hooks and steps inside a session stay on the bound runner") {
    PoolFixture f(3);
    ExecutionContext ctx;
    {
        RunnerBridge::Session session = f.bridge.acquireSession();
        REQUIRE(session.active());
        CHECK(f.bridge.runHook(HookType::BeforeSuite, ctx).success);
        CHECK(f.bridge.runHook(HookType::BeforeSpec, ctx).success);
        CHECK(f.bridge.runHook(HookType::BeforeScenario, ctx).success);
        CHECK(f.bridge.executeStep(f.step("pinned")).state == TestState::PASSED);
        CHECK(f.bridge.runHook(HookType::AfterScenario, ctx).success);
        CHECK(f.bridge.runHook(HookType::AfterSpec, ctx).success);
        CHECK(f.bridge.runHook(HookType::AfterSuite, ctx).success);
        std::lock_guard<std::mutex> lock(f.world.mutex);
        int pinned = f.world.steps.back().first;
        CHECK_EQ(f.world.hooks.size(), static_cast<size_t>(6));
        for (const auto& h : f.world.hooks) CHECK_EQ(h.first, pinned);
        CHECK_EQ(session.slotIndex(), 0);
        CHECK_EQ(pinned, 1);   // 槽位 0 的 Runner 总是最先创建（用于探测并发安全性）
    }
    // 会话外的钩子/步骤临时占用一个空闲槽位；被占用的槽位不会被选中
    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        RunnerBridge::Session session = f.bridge.acquireSession();
        holding = true;
        CHECK_EQ(session.slotIndex(), 0);
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!holding) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(f.bridge.runHook(HookType::BeforeSuite, ctx).success);
    CHECK(f.bridge.executeStep(f.step("unpinned")).state == TestState::PASSED);
    release = true;
    holder.join();
    RunnerStatus st = f.bridge.getStatus();
    CHECK_EQ(st.busyCount, 0);
    CHECK_EQ(slotStatus(st, 0).stepsExecuted, 1ULL);   // 只有第一段会话里的 "pinned"
    CHECK_EQ(slotStatus(st, 1).stepsExecuted, 1ULL);   // 槽位 0 被占用时 "unpinned" 落在槽位 1
    CHECK_EQ(slotStatus(st, 2).stepsExecuted, 0ULL);
}

TEST_CASE("runner pool: concurrency-safe runners collapse to one shared slot") {
    FakeWorld world;
    world.concurrencySafe = true;
    RunnerBridge bridge;
    bridge.setRunnerFactory([&world] { return std::make_unique<FakeRunner>(world); });
    RunnerConfig cfg;
    cfg.language = "fake";
    cfg.poolSize = 4;
    REQUIRE(bridge.start(cfg));
    CHECK_EQ(bridge.poolSize(), 1);
    CHECK_EQ(world.created.load(), 1);
    CHECK_EQ(bridge.getStatus().poolSize, 1);

    Barrier allAcquired(4);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            RunnerBridge::Session session = bridge.acquireSession();
            CHECK(session.active());
            CHECK_EQ(session.slotIndex(), 0);
            allAcquired.wait();
            CHECK(bridge.executeStep([] { StepExecutionRequest r; r.stepText = "shared sleep 60"; return r; }()).state == TestState::PASSED);
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(world.maxInFlight.load(), 4);   // 共享槽位不互斥
    bridge.stopRunner();
    CHECK(!bridge.isConnected());
}

TEST_CASE("runner pool: stop waits for active sessions, restart replaces every process") {
    PoolFixture f(2);
    std::atomic<bool> holding{false};
    std::thread holder([&] {
        RunnerBridge::Session session = f.bridge.acquireSession();
        holding = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        CHECK(f.bridge.executeStep(f.step("late step")).state == TestState::PASSED);
    });
    while (!holding) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto start = std::chrono::steady_clock::now();
    f.bridge.stopRunner();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    holder.join();
    CHECK(ms >= 40.0);
    RunnerStatus st = f.bridge.getStatus();
    CHECK(st.state == RunnerState::DISCONNECTED);
    CHECK_EQ(st.aliveCount, 0);
    CHECK_EQ(st.poolSize, 2);

    // 停止后再次使用：按配置自动拉起（与单 Runner 时代的语义一致）
    {
        RunnerBridge::Session session = f.bridge.acquireSession();
        REQUIRE(session.active());
        CHECK(f.bridge.executeStep(f.step("revive")).state == TestState::PASSED);
    }
    CHECK_EQ(f.bridge.getStatus().aliveCount, 1);

    int before = f.world.created.load();
    CHECK(f.bridge.restartRunner());
    CHECK_EQ(f.world.created.load(), before + 2);
    st = f.bridge.getStatus();
    CHECK_EQ(st.aliveCount, 2);
    for (const auto& s : st.slots) CHECK(s.restartCount >= 1);
    CHECK_EQ(f.bridge.getAllSteps().size(), static_cast<size_t>(1));
    CHECK(f.bridge.hasStep("fake step"));
    CHECK(!f.bridge.hasStep("missing"));
}
