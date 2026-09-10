/*
 * TestHub - 规范解析器
 * 解析 Gauge 风格 Markdown 规范（.spec）与概念文件（.cpt）
 */

#pragma once

#include "spec.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace testhub {
namespace spec {

/**
 * 概念字典：加载并解析 .cpt 文件，用于在规范中展开概念步骤
 */
class ConceptDictionary {
public:
    /**
     * 解析概念文件内容
     * @return 解析错误列表（为空表示成功）
     */
    std::vector<ParseError> parse(const std::string& content, const std::string& fileName);

    /**
     * 根据步骤的参数化文本查找概念
     */
    const Concept* find(const std::string& parameterizedText) const;

    size_t size() const { return concepts_.size(); }
    const std::map<std::string, Concept>& all() const { return concepts_; }
    void clear() { concepts_.clear(); }

private:
    std::map<std::string, Concept> concepts_;
};

/**
 * 解析结果
 */
struct ParseResult {
    std::shared_ptr<Specification> specification;
    std::vector<ParseError> errors;
    std::vector<ParseError> warnings;

    bool ok() const { return errors.empty() && specification != nullptr; }
};

/**
 * 规范解析器
 */
class SpecParser {
public:
    SpecParser() = default;
    explicit SpecParser(const ConceptDictionary* concepts) : concepts_(concepts) {}

    void setConceptDictionary(const ConceptDictionary* concepts) { concepts_ = concepts; }

    /**
     * 解析规范文本
     */
    ParseResult parse(const std::string& content, const std::string& fileName = "");

    /**
     * 解析单个步骤文本（不含 "* " 前缀），生成参数化文本与参数列表
     * @return 是否成功（引号未闭合等返回 false）
     */
    static bool parseStepText(const std::string& text, Step& step, std::string* error = nullptr);

    /**
     * 解析表格行：|a|b|c| -> [a,b,c]
     */
    static std::vector<std::string> parseTableRow(const std::string& line);

    /**
     * 是否为表格分隔行 |---|---|
     */
    static bool isTableSeparator(const std::string& line);

private:
    const ConceptDictionary* concepts_ = nullptr;

    void expandConcept(Step& step, int depth) const;
};

} // namespace spec
} // namespace testhub
