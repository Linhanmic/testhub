/*
 * TestHub - 结果持久化
 * 把已结束测试的记录（请求 + 状态 + 结果）以 JSON 文件保存到目录中，重启后回放为历史。
 * 每个测试一个文件：<dir>/<test_id>.json，写入采用临时文件 + rename 保证原子性。
 */

#pragma once

#include "../model/types.h"
#include "../util/json.h"

#include <string>
#include <vector>

namespace testhub {

struct TestRecord;

class ResultStore {
public:
    static constexpr int kFormatVersion = 1;

    ResultStore() = default;
    explicit ResultStore(std::string dir);

    /** 目录为空表示禁用持久化 */
    bool enabled() const { return !dir_.empty(); }
    const std::string& dir() const { return dir_; }

    /** 确保目录存在；失败时返回 false 并禁用 */
    bool prepare();

    bool save(const TestRecord& record);
    bool remove(const std::string& testId);
    size_t clear();

    /** 读取全部记录，按提交时间升序；损坏的文件被跳过并记录警告 */
    std::vector<TestRecord> loadAll() const;

    /** 单条记录的 JSON 表示（供测试与调试使用） */
    static Json toJson(const TestRecord& record);
    static bool fromJson(const Json& json, TestRecord& record, std::string& error);

private:
    std::string dir_;
    std::string pathFor(const std::string& testId) const;
};

} // namespace testhub
