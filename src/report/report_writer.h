/*
 * TestHub - 报表导出
 * 把一条已结束的测试记录渲染为 JUnit XML（供 CI 系统消费）或自包含的 HTML 报告。
 */

#pragma once

#include "../model/types.h"

#include <string>

namespace testhub {

struct TestRecord;

enum class ReportFormat { JUNIT, HTML };

/** 解析 "junit" / "xml" / "html"；无法识别时返回 false */
bool parseReportFormat(const std::string& text, ReportFormat& out);
const char* reportContentType(ReportFormat format);
const char* reportFileExtension(ReportFormat format);

class ReportWriter {
public:
    static std::string render(const TestRecord& record, ReportFormat format);

    /**
     * JUnit XML：每个规范一个 <testsuite>，每个场景（含数据行）一个 <testcase>。
     * 失败 → <failure>，错误 → <error>，跳过/取消 → <skipped>，步骤列表与 Runner 消息写入 <system-out>。
     */
    static std::string junitXml(const TestRecord& record);

    /** 自包含 HTML（内联样式，无外部资源），可直接归档或作为 CI 产物浏览。 */
    static std::string html(const TestRecord& record);

    static std::string escapeXml(const std::string& text);
    static std::string escapeHtml(const std::string& text);
};

} // namespace testhub
