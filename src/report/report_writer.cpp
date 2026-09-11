/*
 * TestHub - 报表导出实现
 */

#include "report_writer.h"

#include "../engine/execution_engine.h"
#include "../util/string_util.h"
#include "../util/time_util.h"

#include <cstdio>
#include <sstream>

namespace testhub {

namespace {

std::string fmtSeconds(double seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", seconds < 0 ? 0.0 : seconds);
    return buf;
}

std::string fmtDurationHuman(double seconds) {
    char buf[32];
    if (seconds < 1.0) {
        std::snprintf(buf, sizeof(buf), "%d ms", static_cast<int>(seconds * 1000.0 + 0.5));
    } else if (seconds < 60.0) {
        std::snprintf(buf, sizeof(buf), "%.2f s", seconds);
    } else {
        int total = static_cast<int>(seconds + 0.5);
        std::snprintf(buf, sizeof(buf), "%dm %02ds", total / 60, total % 60);
    }
    return buf;
}

std::string isoOrEmpty(const TimePoint& tp) {
    return tp.time_since_epoch().count() == 0 ? std::string() : TimeUtil::toIso8601(tp);
}

std::string scenarioDisplayName(const ScenarioResult& sc) {
    std::string name = sc.scenarioName;
    if (sc.dataRowIndex >= 0) name += " [row " + std::to_string(sc.dataRowIndex + 1) + "]";
    return name;
}

/** 数据驱动场景：把步骤中的 <列名> 替换为当前数据行的值 */
std::string substituteDataRow(const std::string& text, const std::map<std::string, std::string>& row) {
    if (row.empty() || text.find('<') == std::string::npos) return text;
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '<') {
            size_t end = text.find('>', i + 1);
            if (end != std::string::npos) {
                auto it = row.find(text.substr(i + 1, end - i - 1));
                if (it != row.end()) {
                    out += "\"" + it->second + "\"";
                    i = end + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

std::string classNameFor(const SpecResult& spec) {
    std::string file = spec.specFile.empty() ? spec.specName : spec.specFile;
    for (char& c : file) {
        if (c == '/' || c == '\\') c = '.';
    }
    size_t dot = file.rfind('.');
    if (dot != std::string::npos && dot > 0 && (StringUtil::endsWith(file, ".spec") || StringUtil::endsWith(file, ".md"))) {
        file = file.substr(0, dot);
    }
    return file;
}

/** 第一个失败/错误的步骤（含概念内部步骤），用于 <failure> 摘要 */
const StepResult* firstFailedStep(const std::vector<StepResult>& steps) {
    for (const auto& s : steps) {
        if (s.isConcept) {
            if (const StepResult* inner = firstFailedStep(s.conceptSteps)) return inner;
        }
        if (s.state == TestState::FAILED || s.state == TestState::TEST_ERROR) return &s;
    }
    return nullptr;
}

const StepResult* firstFailedStep(const ScenarioResult& sc) {
    if (const StepResult* s = firstFailedStep(sc.contextSteps)) return s;
    if (const StepResult* s = firstFailedStep(sc.stepResults)) return s;
    return firstFailedStep(sc.teardownSteps);
}

const char* stateMark(TestState s) {
    switch (s) {
        case TestState::PASSED: return "PASS";
        case TestState::FAILED: return "FAIL";
        case TestState::SKIPPED: return "SKIP";
        case TestState::CANCELLED: return "CANCEL";
        case TestState::TEST_ERROR: return "ERROR";
        case TestState::QUEUED: return "QUEUED";
        case TestState::RUNNING: return "RUNNING";
    }
    return "?";
}

using DataRow = std::map<std::string, std::string>;

void appendStepsText(std::ostringstream& out, const std::vector<StepResult>& steps, const char* section, int indent, const DataRow& row) {
    if (steps.empty()) return;
    std::string pad(static_cast<size_t>(indent), ' ');
    if (section) out << pad << "[" << section << "]\n";
    for (const auto& s : steps) {
        out << pad << "  " << stateMark(s.state) << "  " << substituteDataRow(s.stepText, row);
        if (s.state != TestState::SKIPPED) out << "  (" << fmtDurationHuman(s.duration) << ")";
        if (s.attempts > 1) out << "  [attempts=" << s.attempts << "]";
        out << "\n";
        for (const auto& m : s.messages) out << pad << "      > " << m << "\n";
        if (!s.errorMessage.empty()) out << pad << "      ! " << s.errorMessage << "\n";
        if (s.isConcept) appendStepsText(out, s.conceptSteps, nullptr, indent + 4, row);
    }
}

std::string stateLabelZh(TestState s) {
    switch (s) {
        case TestState::PASSED: return "通过";
        case TestState::FAILED: return "失败";
        case TestState::SKIPPED: return "跳过";
        case TestState::CANCELLED: return "已取消";
        case TestState::TEST_ERROR: return "错误";
        case TestState::QUEUED: return "排队中";
        case TestState::RUNNING: return "运行中";
    }
    return "未知";
}

void appendStepsHtml(std::ostringstream& out, const std::vector<StepResult>& steps, const char* section, const DataRow& row) {
    if (steps.empty()) return;
    if (section) out << "<div class=\"section\">" << section << "</div>";
    out << "<ul class=\"steps\">";
    for (const auto& s : steps) {
        std::string cls = testStateToString(s.state);
        out << "<li class=\"step " << cls << "\"><span class=\"mark\">" << stateMark(s.state) << "</span>"
            << "<span class=\"text\">" << ReportWriter::escapeHtml(substituteDataRow(s.stepText, row)) << "</span>";
        if (s.attempts > 1) out << "<span class=\"retry\">×" << s.attempts << "</span>";
        if (s.state != TestState::SKIPPED) out << "<span class=\"dur\">" << fmtDurationHuman(s.duration) << "</span>";
        if (!s.messages.empty()) {
            out << "<pre class=\"msg\">";
            for (size_t i = 0; i < s.messages.size(); ++i) {
                if (i) out << "\n";
                out << ReportWriter::escapeHtml(s.messages[i]);
            }
            out << "</pre>";
        }
        if (!s.errorMessage.empty()) out << "<pre class=\"err\">" << ReportWriter::escapeHtml(s.errorMessage) << "</pre>";
        if (!s.stackTrace.empty()) out << "<pre class=\"stack\">" << ReportWriter::escapeHtml(s.stackTrace) << "</pre>";
        if (s.isConcept && !s.conceptSteps.empty()) appendStepsHtml(out, s.conceptSteps, nullptr, row);
        out << "</li>";
    }
    out << "</ul>";
}

const char* kHtmlStyle =
    "*{box-sizing:border-box}body{margin:0;padding:24px;font:14px/1.5 -apple-system,'Segoe UI',Roboto,'PingFang SC','Microsoft YaHei',sans-serif;"
    "color:#1f2933;background:#f5f7fa}h1{margin:0 0 4px;font-size:22px}h2{margin:0;font-size:16px}h3{margin:0;font-size:14px}"
    ".mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}.muted{color:#6b7280}.small{font-size:12px}"
    ".card{background:#fff;border:1px solid #e3e8ef;border-radius:8px;margin-bottom:16px;overflow:hidden}"
    ".card-body{padding:16px}.head{display:flex;align-items:center;gap:12px;flex-wrap:wrap}"
    ".pill{display:inline-block;padding:2px 10px;border-radius:999px;font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:.03em}"
    ".pill.passed{background:#dcfce7;color:#166534}.pill.failed{background:#fee2e2;color:#991b1b}.pill.error{background:#fee2e2;color:#991b1b}"
    ".pill.skipped{background:#f3f4f6;color:#4b5563}.pill.cancelled{background:#fef3c7;color:#92400e}"
    ".stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:12px;margin-top:12px}"
    ".stat{padding:12px;border-radius:8px;background:#f8fafc;border:1px solid #e3e8ef}.stat .v{font-size:22px;font-weight:700}"
    ".stat.pass .v{color:#16a34a}.stat.fail .v{color:#dc2626}.stat.skip .v{color:#6b7280}"
    "dl.kv{display:grid;grid-template-columns:max-content 1fr;gap:4px 16px;margin:12px 0 0}dl.kv dt{color:#6b7280}dl.kv dd{margin:0}"
    ".bar{height:8px;border-radius:4px;background:#e5e7eb;overflow:hidden;display:flex;margin-top:12px}"
    ".bar i{display:block;height:100%}.bar .p{background:#22c55e}.bar .f{background:#ef4444}.bar .s{background:#9ca3af}"
    "details{border-top:1px solid #eef2f6}details>summary{list-style:none;cursor:pointer;padding:10px 16px;display:flex;align-items:center;gap:10px}"
    "details>summary::-webkit-details-marker{display:none}details>summary::before{content:'▸';color:#9ca3af;width:12px}"
    "details[open]>summary::before{content:'▾'}.spec>summary{background:#f8fafc}.scenario>summary{padding-left:32px}"
    ".scenario .body{padding:4px 16px 12px 48px}.section{font-size:11px;text-transform:uppercase;letter-spacing:.06em;color:#9ca3af;margin:8px 0 2px}"
    "ul.steps{list-style:none;margin:0;padding:0}ul.steps ul.steps{margin-left:24px;border-left:2px solid #e5e7eb;padding-left:8px}"
    ".step{padding:4px 8px;border-radius:4px;display:flex;flex-wrap:wrap;align-items:baseline;gap:8px}"
    ".step .mark{font-family:ui-monospace,monospace;font-size:11px;font-weight:700;width:52px;flex:none}"
    ".step.passed .mark{color:#16a34a}.step.failed .mark,.step.error .mark{color:#dc2626}.step.skipped .mark{color:#9ca3af}"
    ".step.failed,.step.error{background:#fef2f2}.step .text{flex:1;font-family:ui-monospace,monospace;font-size:13px}"
    ".step .retry{font-size:11px;color:#b45309;background:#fef3c7;border-radius:4px;padding:0 6px}"
    ".step .dur{color:#9ca3af;font-size:12px}pre{flex-basis:100%;margin:2px 0 0 60px;padding:8px 10px;border-radius:4px;font-size:12px;"
    "white-space:pre-wrap;word-break:break-word;background:#f3f4f6}pre.err{background:#fee2e2;color:#7f1d1d}pre.stack{color:#6b7280}"
    ".tags span{display:inline-block;background:#eef2ff;color:#3730a3;border-radius:4px;padding:0 6px;font-size:11px;margin-right:4px}"
    ".alert{padding:10px 12px;border-radius:6px;margin-top:8px;font-size:13px}.alert.error{background:#fee2e2;color:#7f1d1d}.alert.warn{background:#fef3c7;color:#78350f}"
    ".datarow{font-size:12px;color:#6b7280}.datarow b{color:#374151}footer{margin-top:24px;color:#9ca3af;font-size:12px;text-align:center}";

} // namespace

bool parseReportFormat(const std::string& text, ReportFormat& out) {
    std::string f = StringUtil::toLower(StringUtil::trim(text));
    if (f.empty() || f == "junit" || f == "xml" || f == "junit-xml") {
        out = ReportFormat::JUNIT;
        return true;
    }
    if (f == "html" || f == "htm") {
        out = ReportFormat::HTML;
        return true;
    }
    return false;
}

const char* reportContentType(ReportFormat format) {
    return format == ReportFormat::HTML ? "text/html; charset=utf-8" : "application/xml; charset=utf-8";
}

const char* reportFileExtension(ReportFormat format) {
    return format == ReportFormat::HTML ? "html" : "xml";
}

std::string ReportWriter::escapeXml(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (unsigned char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // XML 1.0 不允许除 \t \n \r 以外的控制字符
                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') break;
                out += static_cast<char>(c);
        }
    }
    return out;
}

std::string ReportWriter::escapeHtml(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string ReportWriter::render(const TestRecord& record, ReportFormat format) {
    return format == ReportFormat::HTML ? html(record) : junitXml(record);
}

// ------------------------------------------------------------
// JUnit XML
// ------------------------------------------------------------

std::string ReportWriter::junitXml(const TestRecord& record) {
    const TestResult& result = record.result;
    const TestRequest& request = record.request;

    struct Counts { int tests = 0, failures = 0, errors = 0, skipped = 0; };
    auto countScenario = [](Counts& c, const ScenarioResult& sc) {
        c.tests++;
        switch (sc.state) {
            case TestState::FAILED: c.failures++; break;
            case TestState::TEST_ERROR: c.errors++; break;
            case TestState::SKIPPED:
            case TestState::CANCELLED:
            case TestState::QUEUED:
            case TestState::RUNNING: c.skipped++; break;
            case TestState::PASSED: break;
        }
    };

    Counts total;
    for (const auto& spec : result.specResults)
        for (const auto& sc : spec.scenarioResults) countScenario(total, sc);

    std::ostringstream x;
    x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    std::string suitesName = request.name.empty() ? "testhub-" + record.request.id : request.name;
    x << "<testsuites name=\"" << escapeXml(suitesName) << "\" tests=\"" << total.tests << "\" failures=\"" << total.failures
      << "\" errors=\"" << total.errors << "\" skipped=\"" << total.skipped << "\" time=\"" << fmtSeconds(result.totalDuration) << "\"";
    std::string ts = isoOrEmpty(result.startTime);
    if (!ts.empty()) x << " timestamp=\"" << escapeXml(ts) << "\"";
    x << ">\n";

    int suiteId = 0;
    for (const auto& spec : result.specResults) {
        Counts c;
        for (const auto& sc : spec.scenarioResults) countScenario(c, sc);
        x << "  <testsuite id=\"" << suiteId++ << "\" name=\"" << escapeXml(spec.specName.empty() ? spec.specFile : spec.specName)
          << "\" file=\"" << escapeXml(spec.specFile) << "\" tests=\"" << c.tests << "\" failures=\"" << c.failures
          << "\" errors=\"" << c.errors << "\" skipped=\"" << c.skipped << "\" time=\"" << fmtSeconds(spec.duration) << "\"";
        if (!ts.empty()) x << " timestamp=\"" << escapeXml(ts) << "\"";
        x << ">\n";

        x << "    <properties>\n";
        x << "      <property name=\"testhub.test_id\" value=\"" << escapeXml(record.request.id) << "\"/>\n";
        x << "      <property name=\"testhub.state\" value=\"" << testStateToString(spec.state) << "\"/>\n";
        if (!spec.tags.empty()) x << "      <property name=\"tags\" value=\"" << escapeXml(StringUtil::join(spec.tags, ",")) << "\"/>\n";
        if (!request.environment.empty()) x << "      <property name=\"environment\" value=\"" << escapeXml(request.environment) << "\"/>\n";
        for (const auto& kv : request.metadata) {
            x << "      <property name=\"" << escapeXml(kv.first) << "\" value=\"" << escapeXml(kv.second) << "\"/>\n";
        }
        x << "    </properties>\n";

        std::string className = classNameFor(spec);
        for (const auto& sc : spec.scenarioResults) {
            x << "    <testcase name=\"" << escapeXml(scenarioDisplayName(sc)) << "\" classname=\"" << escapeXml(className)
              << "\" time=\"" << fmtSeconds(sc.duration) << "\"";
            if (!spec.specFile.empty()) x << " file=\"" << escapeXml(spec.specFile) << "\"";
            if (sc.lineNumber > 0) x << " line=\"" << sc.lineNumber << "\"";
            x << ">\n";

            if (sc.state == TestState::FAILED || sc.state == TestState::TEST_ERROR) {
                const StepResult* failed = firstFailedStep(sc);
                std::string message = !sc.errorMessage.empty() ? sc.errorMessage : (failed ? failed->errorMessage : std::string());
                if (message.empty()) message = sc.state == TestState::FAILED ? "Scenario failed" : "Scenario error";
                const char* tag = sc.state == TestState::FAILED ? "failure" : "error";
                x << "      <" << tag << " message=\"" << escapeXml(message) << "\" type=\""
                  << (sc.state == TestState::FAILED ? "StepFailure" : "ExecutionError") << "\">";
                std::ostringstream body;
                if (failed) {
                    body << "Step: " << substituteDataRow(failed->stepText, sc.dataRow) << "\n";
                    if (!failed->errorMessage.empty()) body << failed->errorMessage << "\n";
                    if (!failed->stackTrace.empty()) body << "\n" << failed->stackTrace << "\n";
                } else {
                    body << message << "\n";
                }
                x << escapeXml(body.str()) << "</" << tag << ">\n";
            } else if (sc.state != TestState::PASSED) {
                std::string message = !sc.errorMessage.empty() ? sc.errorMessage
                                      : (sc.state == TestState::CANCELLED ? "Scenario cancelled" : "Scenario skipped");
                x << "      <skipped message=\"" << escapeXml(message) << "\"/>\n";
            }

            std::ostringstream sysout;
            if (sc.dataRowIndex >= 0 && !sc.dataRow.empty()) {
                sysout << "Data row " << (sc.dataRowIndex + 1) << ":";
                for (const auto& kv : sc.dataRow) sysout << " " << kv.first << "=" << kv.second;
                sysout << "\n";
            }
            appendStepsText(sysout, sc.contextSteps, "context", 0, sc.dataRow);
            appendStepsText(sysout, sc.stepResults, sc.contextSteps.empty() && sc.teardownSteps.empty() ? nullptr : "steps", 0, sc.dataRow);
            appendStepsText(sysout, sc.teardownSteps, "teardown", 0, sc.dataRow);
            std::string so = sysout.str();
            if (!so.empty()) x << "      <system-out>" << escapeXml(so) << "</system-out>\n";
            x << "    </testcase>\n";
        }

        if (!spec.errorMessage.empty()) {
            x << "    <system-err>" << escapeXml(spec.errorMessage) << "</system-err>\n";
        }
        x << "  </testsuite>\n";
    }

    if (!result.errors.empty() || !result.warnings.empty()) {
        // 顶层错误/警告放入一个空 testsuite 的 system-err，保持与消费方兼容
        x << "  <testsuite name=\"testhub\" tests=\"0\" failures=\"0\" errors=\"0\" skipped=\"0\" time=\"0.000\">\n";
        std::ostringstream err;
        for (const auto& e : result.errors) err << "ERROR: " << e << "\n";
        for (const auto& w : result.warnings) err << "WARNING: " << w << "\n";
        x << "    <system-err>" << escapeXml(err.str()) << "</system-err>\n";
        x << "  </testsuite>\n";
    }

    x << "</testsuites>\n";
    return x.str();
}

// ------------------------------------------------------------
// HTML
// ------------------------------------------------------------

std::string ReportWriter::html(const TestRecord& record) {
    const TestResult& r = record.result;
    const TestRequest& req = record.request;
    std::string state = testStateToString(r.finalState);
    std::string title = req.name.empty() ? "TestHub 报告 · " + req.id : req.name;

    std::ostringstream h;
    h << "<!DOCTYPE html>\n<html lang=\"zh-CN\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      << "<link rel=\"icon\" href=\"data:,\"><title>" << escapeHtml(title) << "</title><style>" << kHtmlStyle << "</style></head><body>\n";

    // 概览
    h << "<div class=\"card\"><div class=\"card-body\"><div class=\"head\"><h1>" << escapeHtml(title) << "</h1>"
      << "<span class=\"pill " << state << "\">" << stateLabelZh(r.finalState) << "</span>"
      << "<span class=\"mono muted small\">" << escapeHtml(req.id) << "</span></div>";

    int total = r.totalScenarios, passed = r.passedScenarios, failed = r.failedScenarios, skipped = r.skippedScenarios;
    h << "<div class=\"stats\">"
      << "<div class=\"stat\"><div class=\"muted small\">场景总数</div><div class=\"v\">" << total << "</div></div>"
      << "<div class=\"stat pass\"><div class=\"muted small\">通过</div><div class=\"v\">" << passed << "</div></div>"
      << "<div class=\"stat fail\"><div class=\"muted small\">失败</div><div class=\"v\">" << failed << "</div></div>"
      << "<div class=\"stat skip\"><div class=\"muted small\">跳过</div><div class=\"v\">" << skipped << "</div></div>"
      << "<div class=\"stat\"><div class=\"muted small\">耗时</div><div class=\"v\">" << fmtDurationHuman(r.totalDuration) << "</div></div>"
      << "</div>";
    if (total > 0) {
        auto pct = [total](int n) { return n * 100.0 / total; };
        char p[32], f[32], s[32];
        std::snprintf(p, sizeof(p), "%.2f", pct(passed));
        std::snprintf(f, sizeof(f), "%.2f", pct(failed));
        std::snprintf(s, sizeof(s), "%.2f", pct(skipped));
        h << "<div class=\"bar\"><i class=\"p\" style=\"width:" << p << "%\"></i><i class=\"f\" style=\"width:" << f
          << "%\"></i><i class=\"s\" style=\"width:" << s << "%\"></i></div>";
    }

    h << "<dl class=\"kv\">";
    h << "<dt>规范</dt><dd>" << escapeHtml(req.specFiles.empty() ? "(全部)" : StringUtil::join(req.specFiles, ", ")) << "</dd>";
    if (!req.tags.empty()) h << "<dt>标签</dt><dd class=\"tags\">";
    for (const auto& t : req.tags) h << "<span>" << escapeHtml(t) << "</span>";
    if (!req.tags.empty()) h << "</dd>";
    if (!req.scenarios.empty()) h << "<dt>场景过滤</dt><dd>" << escapeHtml(StringUtil::join(req.scenarios, ", ")) << "</dd>";
    h << "<dt>环境</dt><dd>" << escapeHtml(req.environment) << "</dd>";
    h << "<dt>开始</dt><dd class=\"mono\">" << escapeHtml(isoOrEmpty(r.startTime)) << "</dd>";
    h << "<dt>结束</dt><dd class=\"mono\">" << escapeHtml(isoOrEmpty(r.endTime)) << "</dd>";
    for (const auto& kv : req.metadata) h << "<dt>" << escapeHtml(kv.first) << "</dt><dd>" << escapeHtml(kv.second) << "</dd>";
    h << "</dl>";
    for (const auto& e : r.errors) h << "<div class=\"alert error\">" << escapeHtml(e) << "</div>";
    for (const auto& w : r.warnings) h << "<div class=\"alert warn\">" << escapeHtml(w) << "</div>";
    h << "</div></div>\n";

    // 结果树
    h << "<div class=\"card\">";
    if (r.specResults.empty()) h << "<div class=\"card-body muted\">没有执行任何规范</div>";
    for (const auto& spec : r.specResults) {
        bool openSpec = spec.state != TestState::PASSED;
        h << "<details class=\"spec\"" << (openSpec ? " open" : "") << "><summary><h2>" << escapeHtml(spec.specName.empty() ? spec.specFile : spec.specName)
          << "</h2><span class=\"pill " << testStateToString(spec.state) << "\">" << stateLabelZh(spec.state) << "</span>"
          << "<span class=\"mono muted small\">" << escapeHtml(spec.specFile) << "</span>"
          << "<span class=\"muted small\">" << spec.passedScenarios << "/" << spec.totalScenarios << " 通过 · " << fmtDurationHuman(spec.duration) << "</span>";
        if (!spec.tags.empty()) {
            h << "<span class=\"tags\">";
            for (const auto& t : spec.tags) h << "<span>" << escapeHtml(t) << "</span>";
            h << "</span>";
        }
        h << "</summary>";
        if (!spec.errorMessage.empty()) h << "<div class=\"card-body\"><div class=\"alert error\">" << escapeHtml(spec.errorMessage) << "</div></div>";
        for (const auto& sc : spec.scenarioResults) {
            bool openSc = sc.state == TestState::FAILED || sc.state == TestState::TEST_ERROR;
            h << "<details class=\"scenario\"" << (openSc ? " open" : "") << "><summary><h3>" << escapeHtml(scenarioDisplayName(sc)) << "</h3>"
              << "<span class=\"pill " << testStateToString(sc.state) << "\">" << stateLabelZh(sc.state) << "</span>";
            if (sc.state != TestState::SKIPPED) h << "<span class=\"muted small\">" << fmtDurationHuman(sc.duration) << "</span>";
            if (sc.lineNumber > 0) h << "<span class=\"muted small mono\">L" << sc.lineNumber << "</span>";
            h << "</summary><div class=\"body\">";
            if (!sc.dataRow.empty()) {
                h << "<div class=\"datarow\">数据行：";
                bool first = true;
                for (const auto& kv : sc.dataRow) {
                    if (!first) h << " · ";
                    first = false;
                    h << "<b>" << escapeHtml(kv.first) << "</b> = " << escapeHtml(kv.second);
                }
                h << "</div>";
            }
            // 场景级错误若只是首个失败步骤错误的复述，则不重复展示
            const StepResult* failedStep = firstFailedStep(sc);
            bool duplicate = failedStep && failedStep->errorMessage == sc.errorMessage;
            if (!sc.errorMessage.empty() && !duplicate) h << "<div class=\"alert error\">" << escapeHtml(sc.errorMessage) << "</div>";
            appendStepsHtml(h, sc.contextSteps, "上下文", sc.dataRow);
            appendStepsHtml(h, sc.stepResults, sc.contextSteps.empty() && sc.teardownSteps.empty() ? nullptr : "步骤", sc.dataRow);
            appendStepsHtml(h, sc.teardownSteps, "清理", sc.dataRow);
            h << "</div></details>";
        }
        h << "</details>";
    }
    h << "</div>\n";

    h << "<footer>由 TestHub 生成 · " << escapeHtml(TimeUtil::toIso8601(TimeUtil::now())) << "</footer>\n";
    h << "</body></html>\n";
    return h.str();
}

} // namespace testhub
