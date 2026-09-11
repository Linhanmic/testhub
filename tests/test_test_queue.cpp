#include "test_framework.h"
#include "engine/test_queue.h"

#include <thread>

using namespace testhub;

namespace {
TestRequest req(const std::string& id, Priority p = Priority::NORMAL) {
    TestRequest r;
    r.id = id;
    r.priority = p;
    r.specFiles = {"login.spec"};
    return r;
}
}

TEST_CASE("queue: fifo within same priority, higher priority first") {
    TestQueue q;
    q.enqueue(req("a"));
    q.enqueue(req("b"));
    q.enqueue(req("urgent", Priority::URGENT));
    q.enqueue(req("low", Priority::LOW));
    q.enqueue(req("high", Priority::HIGH));
    CHECK_EQ(q.size(), static_cast<size_t>(5));
    CHECK_EQ(q.position("urgent"), 0);
    CHECK_EQ(q.position("high"), 1);
    CHECK_EQ(q.position("a"), 2);
    CHECK_EQ(q.position("b"), 3);
    CHECK_EQ(q.position("low"), 4);
    CHECK_EQ(q.position("missing"), -1);

    std::vector<std::string> order;
    while (auto t = q.dequeue()) order.push_back(t->request.id);
    REQUIRE_EQ(order.size(), static_cast<size_t>(5));
    CHECK_EQ(order[0], std::string("urgent"));
    CHECK_EQ(order[1], std::string("high"));
    CHECK_EQ(order[2], std::string("a"));
    CHECK_EQ(order[3], std::string("b"));
    CHECK_EQ(order[4], std::string("low"));
    CHECK(q.empty());
}

TEST_CASE("queue: generated ids are unique and well formed") {
    TestQueue q;
    std::string a = q.enqueue(req(""));
    std::string b = q.enqueue(req(""));
    CHECK(a != b);
    CHECK(a.rfind("test-", 0) == 0);
    CHECK_EQ(a.size(), std::string("test-20260101-000000-001").size());
}

TEST_CASE("queue: cancel removes queued task") {
    TestQueue q;
    q.enqueue(req("a"));
    q.enqueue(req("b"));
    CHECK(q.cancel("a"));
    CHECK(!q.cancel("a"));
    CHECK(!q.contains("a"));
    CHECK(q.contains("b"));
    CHECK_EQ(q.getTaskIds().size(), static_cast<size_t>(1));
}

TEST_CASE("queue: waitAndDequeue blocks until item or close") {
    TestQueue q;
    auto none = q.waitAndDequeue(20);
    CHECK(!none.has_value());

    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        q.enqueue(req("late"));
    });
    auto got = q.waitAndDequeue(2000);
    producer.join();
    REQUIRE(got.has_value());
    CHECK_EQ(got->request.id, std::string("late"));

    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        q.close();
    });
    auto start = std::chrono::steady_clock::now();
    auto after = q.waitAndDequeue(5000);
    closer.join();
    CHECK(!after.has_value());
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}
