/*
 * TestHub - 规范数据模型
 * Gauge 风格 Markdown 规范（.spec）与概念（.cpt）的内存表示
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace testhub {
namespace spec {

/**
 * 数据表
 */
struct Table {
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;

    bool empty() const { return headers.empty(); }
    size_t rowCount() const { return rows.size(); }
    size_t columnCount() const { return headers.size(); }

    int columnIndex(const std::string& name) const {
        for (size_t i = 0; i < headers.size(); ++i) {
            if (headers[i] == name) return static_cast<int>(i);
        }
        return -1;
    }

    std::string cell(size_t row, const std::string& column) const {
        int idx = columnIndex(column);
        if (idx < 0 || row >= rows.size() || static_cast<size_t>(idx) >= rows[row].size()) return "";
        return rows[row][static_cast<size_t>(idx)];
    }
};

/**
 * 步骤参数类型
 */
enum class ArgType {
    Static,        // "quoted text"
    Dynamic,       // <placeholder> 引用数据表列或概念参数
    SpecialString, // <file:path>
    SpecialTable,  // <table:path>
    Table          // 内联表格
};

inline std::string argTypeToString(ArgType t) {
    switch (t) {
        case ArgType::Static: return "static";
        case ArgType::Dynamic: return "dynamic";
        case ArgType::SpecialString: return "special_string";
        case ArgType::SpecialTable: return "special_table";
        case ArgType::Table: return "table";
    }
    return "static";
}

/**
 * 步骤参数
 */
struct StepArg {
    ArgType type = ArgType::Static;
    std::string value;   // 静态值 / 动态参数名 / 特殊参数路径
    Table table;         // 表格参数
};

/**
 * 步骤
 */
struct Step {
    std::string text;                 // 原始文本（不含 "* "）
    std::string parameterizedText;    // 参数替换为 {} 后的文本，用于匹配实现
    std::vector<StepArg> args;
    int lineNumber = 0;

    // 概念展开
    bool isConcept = false;
    std::vector<Step> conceptSteps;

    bool hasInlineTable() const {
        for (const auto& a : args) if (a.type == ArgType::Table) return true;
        return false;
    }
};

/**
 * 场景
 */
struct Scenario {
    std::string name;
    std::vector<std::string> tags;
    std::vector<Step> steps;
    std::vector<std::string> comments;
    int lineNumber = 0;
    int span = 0;

    bool hasTag(const std::string& tag) const {
        for (const auto& t : tags) if (t == tag) return true;
        return false;
    }
};

/**
 * 规范
 */
struct Specification {
    std::string fileName;
    std::string heading;
    std::vector<std::string> tags;
    std::vector<Step> contexts;
    std::vector<Step> teardowns;
    std::vector<Scenario> scenarios;
    Table dataTable;
    std::vector<std::string> comments;
    int headingLine = 0;

    bool hasTag(const std::string& tag) const {
        for (const auto& t : tags) if (t == tag) return true;
        return false;
    }
    bool isDataDriven() const { return !dataTable.empty() && dataTable.rowCount() > 0; }
};

/**
 * 概念定义（.cpt）
 */
struct Concept {
    std::string heading;              // 原始标题
    std::string parameterizedText;    // 标题参数替换为 {}
    std::vector<std::string> params;  // 参数名顺序
    std::vector<Step> steps;
    std::string fileName;
    int lineNumber = 0;
};

/**
 * 解析错误
 */
struct ParseError {
    std::string fileName;
    int lineNumber = 0;
    std::string message;
    std::string lineText;
};

} // namespace spec
} // namespace testhub
