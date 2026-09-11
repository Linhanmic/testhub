#include "test_framework.h"
#include "event/event_bus.h"
#include "spec/spec_repository.h"
#include "spec/spec_watcher.h"
#include "util/file_util.h"

#include "compat.h"
#include <chrono>
#include <filesystem>
#include <thread>

using namespace testhub;
using namespace testhub::spec;
namespace fs = std::filesystem;

namespace {

struct WatchedDir {
    std::string dir;
    SpecRepository repo;
    WatchedDir() {
        dir = (fs::temp_directory_path() / ("testhub-watch-" + std::to_string(testhubGetPid()) + "-" +
                                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
        fs::create_directories(dir + "/concepts");
        write("a.spec", "# A\n## S\n* step a\n");
        write("concepts/c.cpt", "# 登录 <user>\n* 输入 <user>\n");
        repo.configure(dir, "");
        repo.reloadConcepts();
    }
    ~WatchedDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const std::string& name, const std::string& content) {
        fs::create_directories(fs::path(dir + "/" + name).parent_path());
        FileUtil::writeFile(dir + "/" + name, content);
    }
    void remove(const std::string& name) { fs::remove(dir + "/" + name); }
};

SpecWatcherConfig immediate() {
    SpecWatcherConfig c;
    c.enabled = false;   // 不启动线程，测试手动调用 scan()
    c.settleMs = 0;
    return c;
}

std::vector<Event> watcherEvents() {
    EventBus::getInstance().waitForIdle(1000);
    std::vector<Event> out;
    for (const auto& e : EventBus::getInstance().recentEvents(100)) {
        auto it = e.data.find("source");
        if (e.type == EventType::SPECS_RELOADED && it != e.data.end() && it->second == "watcher") out.push_back(e);
    }
    return out;
}

} // namespace

TEST_CASE("watcher: detects created, updated and deleted spec files") {
    WatchedDir d;
    SpecWatcher w(d.repo);
    w.configure(immediate());
    w.start();
    CHECK_EQ(w.stats().trackedFiles, static_cast<size_t>(2));
    CHECK(w.scan().empty());

    size_t before = watcherEvents().size();
    d.write("nested/b.spec", "# B\n## S\n* step b\n");
    SpecChangeSet c1 = w.scan();
    REQUIRE_EQ(c1.created.size(), static_cast<size_t>(1));
    CHECK_EQ(c1.created[0], std::string("nested/b.spec"));
    CHECK(c1.updated.empty());
    CHECK(!c1.conceptsChanged);

    d.write("a.spec", "# A\n## S\n* step a\n* step a2 (longer content changes the size)\n");
    SpecChangeSet c2 = w.scan();
    REQUIRE_EQ(c2.updated.size(), static_cast<size_t>(1));
    CHECK_EQ(c2.updated[0], std::string("a.spec"));

    d.remove("nested/b.spec");
    d.write("notes.txt", "ignored: not a spec");
    SpecChangeSet c3 = w.scan();
    REQUIRE_EQ(c3.deleted.size(), static_cast<size_t>(1));
    CHECK_EQ(c3.deleted[0], std::string("nested/b.spec"));
    CHECK(c3.created.empty());

    std::vector<Event> evs = watcherEvents();
    REQUIRE_EQ(evs.size(), before + 3);
    const Event& last = evs.back();
    CHECK_EQ(last.data.at("deleted"), std::string("1"));
    CHECK_EQ(last.data.at("files"), std::string("nested/b.spec"));
    SpecWatcherStats st = w.stats();
    CHECK_EQ(st.changes, 3ULL);
    CHECK_EQ(st.reloads, 3ULL);
    CHECK_EQ(st.scans, 4ULL);
    CHECK(!st.lastChangeAt.empty());
    CHECK_EQ(st.trackedFiles, static_cast<size_t>(2));
    w.stop();
}

TEST_CASE("watcher: concept changes reload the dictionary") {
    WatchedDir d;
    SpecWatcher w(d.repo);
    w.configure(immediate());
    w.start();
    CHECK_EQ(d.repo.concepts().size(), static_cast<size_t>(1));

    d.write("concepts/more.cpt", "# 下单 <sku> 件数 <n>\n* 加入 <sku>\n* 数量 <n>\n");
    SpecChangeSet c = w.scan();
    CHECK(c.conceptsChanged);
    CHECK_EQ(d.repo.concepts().size(), static_cast<size_t>(2));
    std::vector<Event> evs = watcherEvents();
    REQUIRE(!evs.empty());
    CHECK_EQ(evs.back().data.at("concepts"), std::string("2"));
    CHECK_EQ(evs.back().data.at("concepts_reloaded"), std::string("true"));

    d.remove("concepts/c.cpt");
    CHECK(w.scan().conceptsChanged);
    CHECK_EQ(d.repo.concepts().size(), static_cast<size_t>(1));
}

TEST_CASE("watcher: acknowledge suppresses self-inflicted changes and settle defers fresh writes") {
    WatchedDir d;
    SpecWatcher w(d.repo);
    w.configure(immediate());
    w.start();

    d.write("api-written.spec", "# X\n## S\n* s\n");
    w.acknowledge();
    CHECK(w.scan().empty());

    SpecWatcherConfig settling = immediate();
    settling.settleMs = 60000;  // 刚写入的文件视为“仍在写入”
    w.configure(settling);
    d.write("fresh.spec", "# Fresh\n## S\n* s\n");
    CHECK(w.scan().empty());
    w.configure(immediate());
    SpecChangeSet c = w.scan();
    REQUIRE_EQ(c.created.size(), static_cast<size_t>(1));
    CHECK_EQ(c.created[0], std::string("fresh.spec"));
}

TEST_CASE("watcher: background thread publishes specs.reloaded") {
    WatchedDir d;
    SpecWatcher w(d.repo);
    SpecWatcherConfig cfg;
    cfg.enabled = true;
    cfg.intervalMs = 20;
    cfg.settleMs = 0;
    w.configure(cfg);
    w.start();
    CHECK(w.running());
    CHECK(w.stats().enabled);

    size_t before = watcherEvents().size();
    d.write("bg.spec", "# BG\n## S\n* s\n");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool seen = false;
    while (!seen && std::chrono::steady_clock::now() < deadline) {
        for (const auto& e : watcherEvents()) {
            auto it = e.data.find("files");
            if (it != e.data.end() && it->second == "bg.spec") seen = true;
        }
        if (!seen) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(seen);
    CHECK(watcherEvents().size() >= before + 1);
    w.stop();
    CHECK(!w.running());
    CHECK(w.stats().scans >= 1ULL);
    // 停止后不再扫描
    unsigned long long scans = w.stats().scans;
    d.write("after-stop.spec", "# Z\n## S\n* s\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(w.stats().scans, scans);

    // 禁用配置：start() 只记录快照，不启动线程
    SpecWatcher off(d.repo);
    SpecWatcherConfig disabled;
    disabled.intervalMs = 0;
    off.configure(disabled);
    off.start();
    CHECK(!off.stats().enabled);
    CHECK(off.stats().trackedFiles >= 2);
    off.stop();
}
