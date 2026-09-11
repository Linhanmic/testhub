/*
 * TestHub - 规范仓库
 * 负责规范目录扫描、概念加载、路径解析、解析缓存与校验
 */

#pragma once

#include "spec.h"
#include "spec_parser.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace testhub {
namespace spec {

/**
 * 规范文件摘要（用于列表）
 */
struct SpecSummary {
    std::string file;          // 相对于规范根目录的路径
    std::string absolutePath;
    std::string heading;
    std::vector<std::string> tags;
    int scenarioCount = 0;
    bool isDataDriven = false;
    bool valid = true;
    std::vector<ParseError> errors;
    long long size = 0;
    long long modifiedAt = 0;  // epoch millis
};

class SpecRepository {
public:
    SpecRepository() = default;

    /**
     * 配置目录（specsDir 为根目录，conceptsDir 可为相对 specsDir 的路径或绝对路径）
     */
    void configure(const std::string& specsDir, const std::string& conceptsDir);

    const std::string& specsDir() const { return specsDir_; }
    const std::string& conceptsDir() const { return conceptsDir_; }

    /**
     * 重新扫描概念文件
     * @return 概念解析错误
     */
    std::vector<ParseError> reloadConcepts();

    /**
     * 将请求中的路径（文件、目录、相对/绝对）解析为具体 .spec 文件列表；未匹配的项写入 missing
     */
    std::vector<std::string> resolve(const std::vector<std::string>& inputs, std::vector<std::string>* missing = nullptr) const;

    /**
     * 解析单个规范文件
     */
    ParseResult load(const std::string& path) const;

    /**
     * 直接解析文本
     */
    ParseResult parseText(const std::string& content, const std::string& fileName = "inline.spec") const;

    /**
     * 列出规范目录下所有规范摘要
     */
    std::vector<SpecSummary> list() const;

    /**
     * 读取原始文件内容；不存在返回 false
     */
    bool readRaw(const std::string& relativePath, std::string& content) const;

    /**
     * 写入规范文件（用于 Web UI 编辑）；路径必须在规范目录内
     */
    bool writeRaw(const std::string& relativePath, const std::string& content, std::string* error = nullptr) const;

    /**
     * 删除规范文件
     */
    bool remove(const std::string& relativePath, std::string* error = nullptr) const;

    /**
     * 把路径转换为相对规范目录的显示路径
     */
    std::string toRelative(const std::string& absolutePath) const;

    /**
     * 将相对路径解析为规范目录内的绝对路径；越界返回空
     */
    std::string toAbsoluteInside(const std::string& relativePath) const;

    const ConceptDictionary& concepts() const { return concepts_; }

    static bool isSpecFile(const std::string& path);
    static bool isConceptFile(const std::string& path);

private:
    std::string specsDir_;
    std::string conceptsDir_;
    ConceptDictionary concepts_;
    mutable std::mutex mutex_;

    std::vector<std::string> collectSpecFiles(const std::string& dir) const;
};

} // namespace spec
} // namespace testhub
