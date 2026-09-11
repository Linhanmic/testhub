/*
 * ReportWriter 单元测试：JUnit XML 与 HTML 报表
 */

#include "test_framework.h"

#include "../src/engine/execution_engine.h"
#include "../src/report/report_writer.h"
#include "../src/util/time_util.h"

using namespace testhub;

namespace {

StepResult step(const std::string& text, TestState state, double duration = 0.01) {
    StepResult s;
    s.stepText = text;
    s.parameterizedText = text;
    s.state = state;
    s.duration = duration;
    return s;
}

TestRecord sampleRecord() {
    TestRecord rec;
    rec.request.id = "test-20260910-000001-001";
    rec.request.name = "Nightly <smoke> & \"regression\"";
    rec.request.specFiles = {"calc/calculator.spec", "login.spec"};
    rec.request.tags = {"smoke"};
    rec.request.metadata["build"] = "42";
    rec.hasResult = true;
    rec.status.testId = rec.request.id;
    rec.status.state = TestState::FAILED;

    TestResult& r = rec.result;
    r.testId = rec.request.id;
    r.finalState = TestState::FAILED;
    r.totalDuration = 1.5;
    r.startTime = TimeUtil::fromIso8601("2026-09-10T10:00:00.000Z");
    r.endTime = TimeUtil::fromIso8601("2026-09-10T10:00:01.500Z");
    r.totalScenarios = 4;
    r.passedScenarios = 1;
    r.failedScenarios = 1;
    r.skippedScenarios = 2;
    r.warnings = {"one warning"};

    SpecResult calc;
    calc.specFile = "calc/calculator.spec";
    calc.specName = "计算器";
    calc.tags = {"unit", "data-driven"};
    calc.state = TestState::FAILED;
    calc.duration = 1.0;
    calc.totalScenarios = 3;
    calc.passedScenarios = 1;
    calc.failedScenarios = 1;
    calc.skippedScenarios = 1;

    ScenarioResult ok;
    ok.scenarioName = "两数相加";
    ok.state = TestState::PASSED;
    ok.duration = 0.2;
    ok.lineNumber = 12;
    ok.dataRowIndex = 0;
    ok.dataRow = {{"a", "1"}, {"b", "2"}, {"sum", "3"}};
    ok.contextSteps = {step("打开计算器", TestState::PASSED)};
    ok.stepResults = {step("输入第一个数 <a>", TestState::PASSED), step("结果应该是 <sum>", TestState::PASSED)};
    ok.stepResults[1].messages = {"display=3"};
    calc.scenarioResults.push_back(ok);

    ScenarioResult bad;
    bad.scenarioName = "两数相加";
    bad.state = TestState::FAILED;
    bad.duration = 0.3;
    bad.lineNumber = 12;
    bad.dataRowIndex = 1;
    bad.dataRow = {{"a", "10"}, {"b", "20"}, {"sum", "31"}};
    StepResult conceptStep = step("登录并输入 <a>", TestState::FAILED);
    conceptStep.isConcept = true;
    StepResult inner = step("结果应该是 <sum>", TestState::FAILED);
    inner.errorMessage = "expected 31 but got 30 <>&";
    inner.stackTrace = "  File \"calc.py\", line 3";
    conceptStep.conceptSteps = {step("输入第一个数 <a>", TestState::PASSED), inner};
    bad.stepResults = {conceptStep, step("点击清空", TestState::SKIPPED)};
    calc.scenarioResults.push_back(bad);

    ScenarioResult skipped;
    skipped.scenarioName = "清空操作";
    skipped.state = TestState::SKIPPED;
    skipped.errorMessage = "Skipped because the test was cancelled";
    skipped.stepResults = {step("点击清空", TestState::SKIPPED, 0.0)};
    calc.scenarioResults.push_back(skipped);
    r.specResults.push_back(calc);

    SpecResult login;
    login.specFile = "login.spec";
    login.specName = "用户登录";
    login.state = TestState::TEST_ERROR;
    login.errorMessage = "Runner disconnected";
    ScenarioResult err;
    err.scenarioName = "正常登录";
    err.state = TestState::TEST_ERROR;
    err.errorMessage = "Runner disconnected";
    login.scenarioResults.push_back(err);
    login.totalScenarios = 1;
    r.specResults.push_back(login);
    return rec;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

} // namespace

TEST_CASE("report: format parsing and content types") {
    ReportFormat f;
    CHECK(parseReportFormat("", f));
    CHECK(f == ReportFormat::JUNIT);
    CHECK(parseReportFormat("XML", f));
    CHECK(f == ReportFormat::JUNIT);
    CHECK(parseReportFormat(" html ", f));
    CHECK(f == ReportFormat::HTML);
    CHECK(!parseReportFormat("pdf", f));
    CHECK_EQ(std::string(reportContentType(ReportFormat::JUNIT)), "application/xml; charset=utf-8");
    CHECK_EQ(std::string(reportFileExtension(ReportFormat::HTML)), "html");
}

TEST_CASE("report: xml escaping strips control characters") {
    CHECK_EQ(ReportWriter::escapeXml("a<b>&\"c'\x01\t"), "a&lt;b&gt;&amp;&quot;c&apos;\t");
    CHECK_EQ(ReportWriter::escapeHtml("<x> & 'y'"), "&lt;x&gt; &amp; &#39;y&#39;");
}

TEST_CASE("report: junit xml has suites, cases, failures, errors and skipped") {
    TestRecord rec = sampleRecord();
    std::string xml = ReportWriter::junitXml(rec);

    CHECK(contains(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
    CHECK(contains(xml, "<testsuites name=\"Nightly &lt;smoke&gt; &amp; &quot;regression&quot;\" tests=\"4\" failures=\"1\" errors=\"1\" skipped=\"1\" time=\"1.500\" timestamp=\"2026-09-10T10:00:00.000Z\">"));
    CHECK(contains(xml, "<testsuite id=\"0\" name=\"计算器\" file=\"calc/calculator.spec\" tests=\"3\" failures=\"1\" errors=\"0\" skipped=\"1\" time=\"1.000\""));
    CHECK(contains(xml, "<testsuite id=\"1\" name=\"用户登录\" file=\"login.spec\" tests=\"1\" failures=\"0\" errors=\"1\" skipped=\"0\""));
    CHECK(contains(xml, "<property name=\"tags\" value=\"unit,data-driven\"/>"));
    CHECK(contains(xml, "<property name=\"build\" value=\"42\"/>"));
    CHECK(contains(xml, "<property name=\"testhub.test_id\" value=\"test-20260910-000001-001\"/>"));

    // 数据行编号进入用例名，classname 由文件路径派生
    CHECK(contains(xml, "<testcase name=\"两数相加 [row 1]\" classname=\"calc.calculator\" time=\"0.200\" file=\"calc/calculator.spec\" line=\"12\">"));
    CHECK(contains(xml, "<testcase name=\"两数相加 [row 2]\""));
    // 失败摘要取自概念内部的失败步骤，并替换数据行参数
    CHECK(contains(xml, "<failure message=\"expected 31 but got 30 &lt;&gt;&amp;\" type=\"StepFailure\">Step: 结果应该是 &quot;31&quot;\nexpected 31 but got 30 &lt;&gt;&amp;\n\n  File &quot;calc.py&quot;, line 3\n</failure>"));
    CHECK(contains(xml, "<error message=\"Runner disconnected\" type=\"ExecutionError\">"));
    CHECK(contains(xml, "<skipped message=\"Skipped because the test was cancelled\"/>"));
    // system-out 含分段、步骤状态、Runner 消息与数据行
    CHECK(contains(xml, "Data row 1: a=1 b=2 sum=3"));
    CHECK(contains(xml, "[context]\n  PASS  打开计算器  (10 ms)"));
    CHECK(contains(xml, "PASS  输入第一个数 &quot;1&quot;  (10 ms)"));
    CHECK(contains(xml, "      &gt; display=3"));
    CHECK(contains(xml, "  SKIP  点击清空\n"));
    CHECK(contains(xml, "<system-err>Runner disconnected</system-err>"));
    CHECK(contains(xml, "WARNING: one warning"));
    CHECK(contains(xml, "</testsuites>\n"));

    // 标签配对粗检
    auto count = [&](const std::string& s) {
        size_t n = 0;
        for (size_t p = xml.find(s); p != std::string::npos; p = xml.find(s, p + s.size())) n++;
        return n;
    };
    CHECK_EQ(count("<testsuite "), static_cast<size_t>(3));  // 2 个规范 + 1 个顶层警告
    CHECK_EQ(count("</testsuite>"), static_cast<size_t>(3));
    CHECK_EQ(count("<testcase "), static_cast<size_t>(4));
    CHECK_EQ(count("</testcase>"), static_cast<size_t>(4));
}

TEST_CASE("report: junit xml for empty result is still well formed") {
    TestRecord rec;
    rec.request.id = "t";
    rec.hasResult = true;
    rec.result.finalState = TestState::SKIPPED;
    std::string xml = ReportWriter::junitXml(rec);
    CHECK(contains(xml, "<testsuites name=\"testhub-t\" tests=\"0\" failures=\"0\" errors=\"0\" skipped=\"0\" time=\"0.000\">"));
    CHECK(!contains(xml, "timestamp="));
    CHECK(contains(xml, "</testsuites>"));
}

TEST_CASE("report: html is self-contained and escapes content") {
    TestRecord rec = sampleRecord();
    std::string html = ReportWriter::html(rec);
    CHECK(contains(html, "<!DOCTYPE html>"));
    CHECK(contains(html, "<title>Nightly &lt;smoke&gt; &amp; &quot;regression&quot;</title>"));
    CHECK(contains(html, "<style>"));
    CHECK(!contains(html, "<script"));
    CHECK(!contains(html, "href=\"http"));
    CHECK(contains(html, "<span class=\"pill failed\">失败</span>"));
    CHECK(contains(html, "<details class=\"spec\" open><summary><h2>计算器</h2>"));
    CHECK(contains(html, "<h3>两数相加 [row 2]</h3>"));
    CHECK(contains(html, "结果应该是 &quot;31&quot;"));
    CHECK(contains(html, "<pre class=\"err\">expected 31 but got 30 &lt;&gt;&amp;</pre>"));
    CHECK(contains(html, "<pre class=\"stack\">  File &quot;calc.py&quot;, line 3</pre>"));
    CHECK(contains(html, "<pre class=\"msg\">display=3</pre>"));
    CHECK(contains(html, "<div class=\"alert warn\">one warning</div>"));
    CHECK(contains(html, "<b>sum</b> = 3"));
    CHECK(contains(html, "</html>"));
    CHECK(contains(ReportWriter::render(rec, ReportFormat::HTML), "<!DOCTYPE html>"));
    CHECK_EQ(ReportWriter::render(rec, ReportFormat::JUNIT), ReportWriter::junitXml(rec));
}
