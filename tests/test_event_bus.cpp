#include "test_framework.h"
#include "event/event_bus.h"

#include <atomic>
#include <mutex>

using namespace testhub;

TEST_CASE("events: wildcard and prefix subscriptions receive matching events") {
    EventBus& bus = EventBus::getInstance();
    std::atomic<int> all{0}, testOnly{0}, exact{0};
    std::string idAll = bus.subscribe("*", [&](const Event&) { ++all; });
    std::string idTest = bus.subscribe("test.*", [&](const Event&) { ++testOnly; });
    std::string idExact = bus.subscribe(EventType::STEP_COMPLETED, [&](const Event&) { ++exact; });

    publishEvent(EventType::TEST_STARTED, "t1");
    publishEvent(EventType::TEST_COMPLETED, "t1");
    publishEvent(EventType::STEP_COMPLETED, "t1");
    publishEvent(EventType::RUNNER_LOG, "");
    bus.waitForIdle(2000);

    CHECK_EQ(all.load(), 4);
    CHECK_EQ(testOnly.load(), 2);
    CHECK_EQ(exact.load(), 1);

    bus.unsubscribe(idAll);
    bus.unsubscribe(idTest);
    bus.unsubscribe(idExact);
    publishEvent(EventType::TEST_STARTED, "t2");
    bus.waitForIdle(2000);
    CHECK_EQ(all.load(), 4);
}

TEST_CASE("events: history is retained and filterable by test id") {
    EventBus& bus = EventBus::getInstance();
    bus.clear();
    publishEvent(EventType::TEST_STARTED, "A", {{"k", "v"}});
    publishEvent(EventType::TEST_STARTED, "B");
    publishEvent(EventType::TEST_COMPLETED, "A");
    bus.waitForIdle(2000);

    auto recent = bus.recentEvents(10);
    CHECK_EQ(recent.size(), static_cast<size_t>(3));
    auto onlyA = bus.recentEvents(10, "A");
    REQUIRE_EQ(onlyA.size(), static_cast<size_t>(2));
    CHECK_EQ(onlyA[0].type, EventType::TEST_STARTED);
    CHECK_EQ(onlyA[0].data.at("k"), std::string("v"));
    CHECK_EQ(onlyA[1].type, EventType::TEST_COMPLETED);
    CHECK_EQ(bus.recentEvents(1).size(), static_cast<size_t>(1));
}

TEST_CASE("events: handlers are invoked in publish order and exceptions are isolated") {
    EventBus& bus = EventBus::getInstance();
    std::mutex m;
    std::vector<std::string> seen;
    std::string throwing = bus.subscribe("*", [&](const Event&) { throw std::runtime_error("boom"); });
    std::string collector = bus.subscribe("*", [&](const Event& e) {
        std::lock_guard<std::mutex> lock(m);
        seen.push_back(e.testId);
    });
    for (int i = 0; i < 20; ++i) publishEvent(EventType::TEST_PROGRESS, std::to_string(i));
    bus.waitForIdle(2000);
    bus.unsubscribe(throwing);
    bus.unsubscribe(collector);
    REQUIRE_EQ(seen.size(), static_cast<size_t>(20));
    for (int i = 0; i < 20; ++i) CHECK_EQ(seen[i], std::to_string(i));
}
