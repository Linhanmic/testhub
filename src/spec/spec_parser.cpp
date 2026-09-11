/*
 * TestHub - 规范解析器实现
 */

#include "spec_parser.h"
#include "../util/string_util.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace testhub {
namespace spec {

namespace {

bool isUnderline(const std::string& line, char ch) {
    std::string t = StringUtil::trim(line);
    if (t.size() < 3) return false;
    for (char c : t) if (c != ch) return false;
    return true;
}

bool isTeardownMarker(const std::string& line) {
    return isUnderline(line, '_');
}

std::string stripHeadingMarker(const std::string& line, size_t level) {
    std::string t = StringUtil::trim(line);
    if (t.size() > level && t.compare(0, level, std::string(level, '#')) == 0 && t[level] != '#') {
        return StringUtil::trim(t.substr(level));
    }
    return "";
}

bool isTagLine(const std::string& line) {
    std::string t = StringUtil::toLower(StringUtil::trim(line));
    return StringUtil::startsWith(t, "tags:");
}

void appendTags(std::vector<std::string>& tags, const std::string& raw) {
    for (auto& tag : StringUtil::split(raw, ",")) {
        std::string t = StringUtil::trim(tag);
        if (!t.empty() && std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
    }
}

std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : content) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r') current.pop_back();
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        if (current.back() == '\r') current.pop_back();
        lines.push_back(current);
    }
    return lines;
}

} // namespace

// ============================================================
// 静态辅助
// ============================================================

std::vector<std::string> SpecParser::parseTableRow(const std::string& line) {
    std::string t = StringUtil::trim(line);
    std::vector<std::string> cells;
    if (t.empty() || t.front() != '|') return cells;
    // 去掉首尾竖线
    size_t start = 1;
    size_t end = t.size();
    if (t.size() > 1 && t.back() == '|') end = t.size() - 1;
    std::string cell;
    for (size_t i = start; i < end; ++i) {
        char c = t[i];
        if (c == '\\' && i + 1 < end && t[i + 1] == '|') {
            cell.push_back('|');
            ++i;
        } else if (c == '|') {
            cells.push_back(StringUtil::trim(cell));
            cell.clear();
        } else {
            cell.push_back(c);
        }
    }
    cells.push_back(StringUtil::trim(cell));
    return cells;
}

bool SpecParser::isTableSeparator(const std::string& line) {
    std::string t = StringUtil::trim(line);
    if (t.empty() || t.front() != '|') return false;
    bool sawDash = false;
    for (char c : t) {
        if (c == '-') sawDash = true;
        else if (c != '|' && c != ' ' && c != ':') return false;
    }
    return sawDash;
}

bool SpecParser::parseStepText(const std::string& text, Step& step, std::string* error) {
    step.text = StringUtil::trim(text);
    step.parameterizedText.clear();
    step.args.clear();

    const std::string& s = step.text;
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '<')) {
            out.push_back(s[i + 1]);
            i += 2;
            continue;
        }
        if (c == '"') {
            size_t j = i + 1;
            std::string value;
            bool closed = false;
            while (j < s.size()) {
                if (s[j] == '\\' && j + 1 < s.size()) {
                    char e = s[j + 1];
                    if (e == '"') value.push_back('"');
                    else if (e == 'n') value.push_back('\n');
                    else if (e == 't') value.push_back('\t');
                    else if (e == '\\') value.push_back('\\');
                    else { value.push_back('\\'); value.push_back(e); }
                    j += 2;
                    continue;
                }
                if (s[j] == '"') { closed = true; break; }
                value.push_back(s[j]);
                ++j;
            }
            if (!closed) {
                if (error) *error = "Unclosed quote in step: " + s;
                return false;
            }
            StepArg arg;
            arg.type = ArgType::Static;
            arg.value = value;
            step.args.push_back(arg);
            out += "{}";
            i = j + 1;
            continue;
        }
        if (c == '<') {
            size_t j = s.find('>', i + 1);
            if (j == std::string::npos) {
                if (error) *error = "Unclosed dynamic parameter in step: " + s;
                return false;
            }
            std::string inner = s.substr(i + 1, j - i - 1);
            StepArg arg;
            if (StringUtil::startsWith(inner, "file:")) {
                arg.type = ArgType::SpecialString;
                arg.value = StringUtil::trim(inner.substr(5));
            } else if (StringUtil::startsWith(inner, "table:")) {
                arg.type = ArgType::SpecialTable;
                arg.value = StringUtil::trim(inner.substr(6));
            } else {
                arg.type = ArgType::Dynamic;
                arg.value = StringUtil::trim(inner);
            }
            step.args.push_back(arg);
            out += "{}";
            i = j + 1;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    // 折叠多余空白
    std::string collapsed;
    bool lastSpace = false;
    for (char c : out) {
        bool sp = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (sp) {
            if (!lastSpace) collapsed.push_back(' ');
        } else {
            collapsed.push_back(c);
        }
        lastSpace = sp;
    }
    step.parameterizedText = StringUtil::trim(collapsed);
    if (step.parameterizedText.empty()) {
        if (error) *error = "Empty step";
        return false;
    }
    return true;
}

// ============================================================
// ConceptDictionary
// ============================================================

std::vector<ParseError> ConceptDictionary::parse(const std::string& content, const std::string& fileName) {
    std::vector<ParseError> errors;
    auto lines = splitLines(content);
    Concept* current = nullptr;
    Concept pending;
    bool hasPending = false;
    Step* lastStep = nullptr;
    std::vector<std::vector<std::string>> tableRows;
    bool inTable = false;

    auto flushTable = [&]() {
        if (inTable && lastStep && !tableRows.empty()) {
            StepArg arg;
            arg.type = ArgType::Table;
            arg.table.headers = tableRows.front();
            for (size_t r = 1; r < tableRows.size(); ++r) arg.table.rows.push_back(tableRows[r]);
            lastStep->args.push_back(arg);
            lastStep->parameterizedText += " {}";
        }
        tableRows.clear();
        inTable = false;
    };

    auto commit = [&]() {
        flushTable();
        if (hasPending) {
            if (pending.steps.empty()) {
                errors.push_back({fileName, pending.lineNumber, "Concept has no steps: " + pending.heading, pending.heading});
            } else if (concepts_.count(pending.parameterizedText)) {
                errors.push_back({fileName, pending.lineNumber, "Duplicate concept: " + pending.heading, pending.heading});
            } else {
                concepts_[pending.parameterizedText] = pending;
            }
        }
        pending = Concept();
        hasPending = false;
        current = nullptr;
        lastStep = nullptr;
    };

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        const std::string& line = lines[idx];
        int lineNo = static_cast<int>(idx) + 1;
        std::string trimmed = StringUtil::trim(line);

        std::string heading = stripHeadingMarker(line, 1);
        bool underlineHeading = false;
        if (heading.empty() && idx + 1 < lines.size() && isUnderline(lines[idx + 1], '=') && !trimmed.empty()
            && trimmed.front() != '*' && trimmed.front() != '|') {
            heading = trimmed;
            underlineHeading = true;
        }
        if (!heading.empty()) {
            commit();
            Step headingStep;
            std::string err;
            if (!SpecParser::parseStepText(heading, headingStep, &err)) {
                errors.push_back({fileName, lineNo, err, line});
                if (underlineHeading) ++idx;
                continue;
            }
            pending.heading = heading;
            pending.parameterizedText = headingStep.parameterizedText;
            for (const auto& a : headingStep.args) {
                if (a.type != ArgType::Dynamic) {
                    errors.push_back({fileName, lineNo, "Concept heading may only contain <dynamic> parameters: " + heading, line});
                }
                pending.params.push_back(a.value);
            }
            pending.fileName = fileName;
            pending.lineNumber = lineNo;
            hasPending = true;
            current = &pending;
            if (underlineHeading) ++idx;
            continue;
        }

        if (trimmed.empty()) {
            flushTable();
            continue;
        }

        if (trimmed.front() == '*') {
            flushTable();
            if (!current) {
                errors.push_back({fileName, lineNo, "Step outside of concept", line});
                continue;
            }
            Step step;
            std::string err;
            if (!SpecParser::parseStepText(trimmed.substr(1), step, &err)) {
                errors.push_back({fileName, lineNo, err, line});
                continue;
            }
            step.lineNumber = lineNo;
            current->steps.push_back(step);
            lastStep = &current->steps.back();
            continue;
        }

        if (trimmed.front() == '|' && lastStep) {
            if (!inTable) {
                inTable = true;
                tableRows.clear();
            }
            if (SpecParser::isTableSeparator(line)) continue;
            tableRows.push_back(SpecParser::parseTableRow(line));
            continue;
        }
        // 其他行视为注释
        flushTable();
    }
    commit();
    return errors;
}

const Concept* ConceptDictionary::find(const std::string& parameterizedText) const {
    auto it = concepts_.find(parameterizedText);
    return it == concepts_.end() ? nullptr : &it->second;
}

// ============================================================
// SpecParser
// ============================================================

void SpecParser::expandConcept(Step& step, int depth) const {
    if (!concepts_ || depth > 16) return;
    const Concept* def = concepts_->find(step.parameterizedText);
    if (!def) return;
    step.isConcept = true;
    step.conceptSteps.clear();

    // 参数名 -> 实参
    std::map<std::string, StepArg> binding;
    for (size_t i = 0; i < def->params.size() && i < step.args.size(); ++i) {
        binding[def->params[i]] = step.args[i];
    }

    for (const auto& inner : def->steps) {
        Step resolved = inner;
        for (auto& arg : resolved.args) {
            if (arg.type == ArgType::Dynamic) {
                auto it = binding.find(arg.value);
                if (it != binding.end()) arg = it->second;
            }
        }
        expandConcept(resolved, depth + 1);
        step.conceptSteps.push_back(resolved);
    }
}

ParseResult SpecParser::parse(const std::string& content, const std::string& fileName) {
    ParseResult result;
    auto specification = std::make_shared<Specification>();
    specification->fileName = fileName;

    auto lines = splitLines(content);

    enum class Section { BeforeHeading, SpecBody, Scenario, Teardown };
    Section section = Section::BeforeHeading;
    Scenario* currentScenario = nullptr;
    Step* lastStep = nullptr;
    std::vector<std::vector<std::string>> tableRows;
    bool inTable = false;
    bool tableForStep = false;
    bool sawStepInSpecBody = false;
    bool tagContinuation = false;
    std::vector<std::string>* tagTarget = nullptr;

    auto error = [&](int lineNo, const std::string& msg, const std::string& text) {
        result.errors.push_back({fileName, lineNo, msg, text});
    };
    auto warning = [&](int lineNo, const std::string& msg, const std::string& text) {
        result.warnings.push_back({fileName, lineNo, msg, text});
    };

    auto flushTable = [&]() {
        if (inTable && !tableRows.empty()) {
            Table table;
            table.headers = tableRows.front();
            for (size_t r = 1; r < tableRows.size(); ++r) {
                auto row = tableRows[r];
                row.resize(table.headers.size());
                table.rows.push_back(row);
            }
            if (tableForStep && lastStep) {
                StepArg arg;
                arg.type = ArgType::Table;
                arg.table = table;
                lastStep->args.push_back(arg);
                lastStep->parameterizedText += " {}";
            } else if (section == Section::SpecBody && !sawStepInSpecBody) {
                if (specification->dataTable.empty()) {
                    specification->dataTable = table;
                } else {
                    warning(lastStep ? lastStep->lineNumber : 0, "Multiple data tables in spec; only the first is used", "");
                }
            }
        }
        tableRows.clear();
        inTable = false;
        tableForStep = false;
    };

    auto addStep = [&](const std::string& text, int lineNo, const std::string& raw) {
        Step step;
        std::string err;
        if (!parseStepText(text, step, &err)) {
            error(lineNo, err, raw);
            lastStep = nullptr;
            return;
        }
        step.lineNumber = lineNo;
        expandConcept(step, 0);
        switch (section) {
            case Section::SpecBody:
                specification->contexts.push_back(step);
                lastStep = &specification->contexts.back();
                sawStepInSpecBody = true;
                break;
            case Section::Scenario:
                currentScenario->steps.push_back(step);
                lastStep = &currentScenario->steps.back();
                break;
            case Section::Teardown:
                specification->teardowns.push_back(step);
                lastStep = &specification->teardowns.back();
                break;
            default:
                error(lineNo, "Step found before specification heading", raw);
                lastStep = nullptr;
        }
    };

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        const std::string& line = lines[idx];
        int lineNo = static_cast<int>(idx) + 1;
        std::string trimmed = StringUtil::trim(line);

        // 标签续行（上一行以逗号结尾）
        if (tagContinuation && tagTarget && !trimmed.empty()) {
            appendTags(*tagTarget, trimmed);
            tagContinuation = !trimmed.empty() && trimmed.back() == ',';
            continue;
        }
        tagContinuation = false;

        // 规范标题
        std::string h1 = stripHeadingMarker(line, 1);
        bool underlined1 = false;
        if (h1.empty() && idx + 1 < lines.size() && isUnderline(lines[idx + 1], '=') && !trimmed.empty()
            && trimmed.front() != '*' && trimmed.front() != '|' && trimmed.front() != '#') {
            h1 = trimmed;
            underlined1 = true;
        }
        if (!h1.empty()) {
            flushTable();
            if (section != Section::BeforeHeading) {
                error(lineNo, "Multiple specification headings are not allowed", line);
            } else {
                specification->heading = h1;
                specification->headingLine = lineNo;
                section = Section::SpecBody;
                tagTarget = &specification->tags;
            }
            if (underlined1) ++idx;
            lastStep = nullptr;
            continue;
        }

        // 场景标题
        std::string h2 = stripHeadingMarker(line, 2);
        bool underlined2 = false;
        if (h2.empty() && idx + 1 < lines.size() && isUnderline(lines[idx + 1], '-') && !trimmed.empty()
            && trimmed.front() != '*' && trimmed.front() != '|' && trimmed.front() != '#') {
            h2 = trimmed;
            underlined2 = true;
        }
        if (!h2.empty()) {
            flushTable();
            if (section == Section::BeforeHeading) {
                error(lineNo, "Scenario found before specification heading", line);
            } else if (section == Section::Teardown) {
                error(lineNo, "Scenario found after teardown section", line);
            } else {
                if (currentScenario) currentScenario->span = lineNo - currentScenario->lineNumber;
                Scenario scenario;
                scenario.name = h2;
                scenario.lineNumber = lineNo;
                specification->scenarios.push_back(scenario);
                currentScenario = &specification->scenarios.back();
                section = Section::Scenario;
                tagTarget = &currentScenario->tags;
            }
            if (underlined2) ++idx;
            lastStep = nullptr;
            continue;
        }

        if (trimmed.empty()) {
            flushTable();
            continue;
        }

        // 清理段标记
        if (isTeardownMarker(line)) {
            flushTable();
            if (section == Section::BeforeHeading) {
                error(lineNo, "Teardown marker before specification heading", line);
            } else {
                if (currentScenario && currentScenario->span == 0) currentScenario->span = lineNo - currentScenario->lineNumber;
                section = Section::Teardown;
                tagTarget = nullptr;
            }
            lastStep = nullptr;
            continue;
        }

        // 标签
        if (isTagLine(line)) {
            flushTable();
            std::string raw = trimmed.substr(5);
            if (!tagTarget || section == Section::Teardown) {
                error(lineNo, "Tags are only allowed after a specification or scenario heading", line);
            } else if ((section == Section::SpecBody && sawStepInSpecBody) ||
                       (section == Section::Scenario && currentScenario && !currentScenario->steps.empty())) {
                error(lineNo, "Tags must appear before steps", line);
            } else {
                appendTags(*tagTarget, raw);
                tagContinuation = !raw.empty() && StringUtil::trim(raw).back() == ',';
            }
            lastStep = nullptr;
            continue;
        }

        // 步骤
        if (trimmed.front() == '*') {
            flushTable();
            addStep(trimmed.substr(1), lineNo, line);
            continue;
        }

        // 表格
        if (trimmed.front() == '|') {
            if (!inTable) {
                inTable = true;
                tableRows.clear();
                tableForStep = (lastStep != nullptr);
                if (!tableForStep && !(section == Section::SpecBody && !sawStepInSpecBody)) {
                    warning(lineNo, "Table is not attached to a step and is not a spec data table; ignored", line);
                    inTable = false;
                    continue;
                }
            }
            if (SpecParser::isTableSeparator(line)) continue;
            auto cells = parseTableRow(line);
            if (!tableRows.empty() && cells.size() != tableRows.front().size()) {
                warning(lineNo, "Table row has different column count than header", line);
            }
            tableRows.push_back(cells);
            continue;
        }

        // 注释
        flushTable();
        lastStep = nullptr;
        if (section == Section::Scenario && currentScenario) currentScenario->comments.push_back(trimmed);
        else specification->comments.push_back(trimmed);
    }
    flushTable();

    if (currentScenario && currentScenario->span == 0) {
        currentScenario->span = static_cast<int>(lines.size()) + 1 - currentScenario->lineNumber;
    }

    if (section == Section::BeforeHeading) {
        error(0, "Specification heading not found", "");
    } else if (specification->scenarios.empty()) {
        warning(specification->headingLine, "Specification has no scenarios: " + specification->heading, "");
    }
    for (const auto& scenario : specification->scenarios) {
        if (scenario.steps.empty()) {
            warning(scenario.lineNumber, "Scenario has no steps: " + scenario.name, "");
        }
    }
    // 校验动态参数是否存在于数据表
    if (!specification->dataTable.empty()) {
        auto check = [&](const Step& step) {
            for (const auto& a : step.args) {
                if (a.type == ArgType::Dynamic && specification->dataTable.columnIndex(a.value) < 0) {
                    error(step.lineNumber, "Dynamic parameter <" + a.value + "> not found in data table", step.text);
                }
            }
        };
        for (const auto& s : specification->contexts) check(s);
        for (const auto& sc : specification->scenarios) for (const auto& s : sc.steps) check(s);
        for (const auto& s : specification->teardowns) check(s);
    } else {
        auto check = [&](const Step& step) {
            for (const auto& a : step.args) {
                if (a.type == ArgType::Dynamic) {
                    error(step.lineNumber, "Dynamic parameter <" + a.value + "> used without a data table", step.text);
                }
            }
        };
        for (const auto& s : specification->contexts) check(s);
        for (const auto& sc : specification->scenarios) for (const auto& s : sc.steps) check(s);
        for (const auto& s : specification->teardowns) check(s);
    }

    result.specification = specification;
    return result;
}

} // namespace spec
} // namespace testhub
