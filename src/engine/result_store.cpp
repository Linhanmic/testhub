#include "result_store.h"
#include "execution_engine.h"
#include "../model/json_convert.h"
#include "../util/file_util.h"
#include "../util/logger.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace testhub {

namespace fs = std::filesystem;

namespace {

bool safeId(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    }
    return id.find("..") == std::string::npos;
}

} // namespace

ResultStore::ResultStore(std::string dir) : dir_(std::move(dir)) {}

std::string ResultStore::pathFor(const std::string& testId) const {
    return FileUtil::joinPath(dir_, testId + ".json");
}

bool ResultStore::prepare() {
    if (!enabled()) return true;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec || !fs::is_directory(dir_)) {
        TH_LOG_ERROR("store", "Cannot create results directory '" + dir_ + "': " + ec.message() + "; persistence disabled");
        dir_.clear();
        return false;
    }
    return true;
}

Json ResultStore::toJson(const TestRecord& record) {
    Json j = Json::object();
    j["format"] = kFormatVersion;
    j["test_id"] = record.status.testId;
    j["request"] = testhub::toJson(record.request);
    j["status"] = testhub::toJson(record.status);
    j["resolved_specs"] = testhub::toJson(record.resolvedSpecs);
    if (record.hasResult) j["result"] = testhub::toJson(record.result);
    return j;
}

bool ResultStore::fromJson(const Json& json, TestRecord& record, std::string& error) {
    if (!json.isObject()) { error = "record is not an object"; return false; }
    if (json["format"].asInt(0) > kFormatVersion) { error = "unsupported format version"; return false; }
    std::string id = json["test_id"].asString("");
    if (id.empty()) { error = "missing test_id"; return false; }
    if (!json["status"].isObject()) { error = "missing status"; return false; }

    TestRequest request;
    std::string reqError;
    if (json["request"].isObject() && !testRequestFromJson(json["request"], request, reqError)) {
        error = "invalid request: " + reqError;
        return false;
    }
    request.id = id;
    record.request = request;
    record.status = testStatusFromJson(json["status"]);
    record.status.testId = id;
    record.resolvedSpecs = stringsFromJson(json["resolved_specs"]);
    record.hasResult = json["result"].isObject();
    if (record.hasResult) {
        record.result = testResultFromJson(json["result"]);
        record.result.testId = id;
    }
    if (!isTerminalState(record.status.state)) {
        // 只有终态记录才会被保存；若文件来自异常退出前的写入，按取消处理
        record.status.state = TestState::CANCELLED;
    }
    return true;
}

bool ResultStore::save(const TestRecord& record) {
    if (!enabled()) return false;
    const std::string& id = record.status.testId;
    if (!safeId(id)) {
        TH_LOG_WARN("store", "Refusing to persist record with unsafe id '" + id + "'");
        return false;
    }
    std::string finalPath = pathFor(id);
    std::string tmpPath = finalPath + ".tmp";
    if (!FileUtil::writeFile(tmpPath, toJson(record).dump(2))) {
        TH_LOG_WARN("store", "Failed to write " + tmpPath);
        return false;
    }
    std::error_code ec;
    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        TH_LOG_WARN("store", "Failed to move " + tmpPath + " into place: " + ec.message());
        fs::remove(tmpPath, ec);
        return false;
    }
    TH_LOG_DEBUG("store", "Persisted " + id + " -> " + finalPath);
    return true;
}

bool ResultStore::remove(const std::string& testId) {
    if (!enabled() || !safeId(testId)) return false;
    return FileUtil::deleteFile(pathFor(testId));
}

size_t ResultStore::clear() {
    if (!enabled()) return 0;
    size_t removed = 0;
    for (const auto& f : FileUtil::listFiles(dir_)) {
        if (FileUtil::getExtension(f) == ".json" && FileUtil::deleteFile(f)) ++removed;
    }
    return removed;
}

std::vector<TestRecord> ResultStore::loadAll() const {
    std::vector<TestRecord> records;
    if (!enabled() || !FileUtil::directoryExists(dir_)) return records;
    size_t skipped = 0;
    for (const auto& f : FileUtil::listFiles(dir_)) {
        if (FileUtil::getExtension(f) != ".json") continue;
        try {
            Json j = Json::parse(FileUtil::readFile(f));
            TestRecord record;
            std::string error;
            if (!fromJson(j, record, error)) {
                TH_LOG_WARN("store", "Skipping " + f + ": " + error);
                ++skipped;
                continue;
            }
            records.push_back(std::move(record));
        } catch (const std::exception& e) {
            TH_LOG_WARN("store", "Skipping unreadable result file " + f + ": " + e.what());
            ++skipped;
        }
    }
    std::sort(records.begin(), records.end(), [](const TestRecord& a, const TestRecord& b) {
        if (a.status.submitTime != b.status.submitTime) return a.status.submitTime < b.status.submitTime;
        return a.status.testId < b.status.testId;
    });
    TH_LOG_INFO("store", "Loaded " + std::to_string(records.size()) + " persisted result(s) from " + dir_ +
                         (skipped ? " (" + std::to_string(skipped) + " skipped)" : ""));
    return records;
}

} // namespace testhub
