#include "test_framework.h"
#include "spec/spec_parser.h"
#include "spec/spec_repository.h"

using namespace testhub::spec;

namespace {
const char* kLoginSpec = R"(# 用户登录
tags: smoke, auth

一些描述文字。

* 打开登录页面

## 使用正确的用户名和密码登录
tags: positive

* 输入用户名 "admin"
* 输入密码 "secret"
* 点击登录按钮

## 空用户名
* 输入用户名 ""

___
* 清理会话
)";
}

TEST_CASE("spec: parse heading, tags, contexts, scenarios and teardown") {
    SpecParser parser;
    ParseResult r = parser.parse(kLoginSpec, "login.spec");
    REQUIRE(r.ok());
    const Specification& s = *r.specification;
    CHECK_EQ(s.heading, std::string("用户登录"));
    CHECK_EQ(s.tags.size(), static_cast<size_t>(2));
    CHECK(s.hasTag("smoke"));
    CHECK(s.hasTag("auth"));
    CHECK_EQ(s.contexts.size(), static_cast<size_t>(1));
    CHECK_EQ(s.contexts[0].text, std::string("打开登录页面"));
    REQUIRE_EQ(s.scenarios.size(), static_cast<size_t>(2));
    CHECK_EQ(s.scenarios[0].name, std::string("使用正确的用户名和密码登录"));
    CHECK(s.scenarios[0].hasTag("positive"));
    CHECK_EQ(s.scenarios[0].steps.size(), static_cast<size_t>(3));
    CHECK_EQ(s.scenarios[0].lineNumber, 8);
    CHECK_EQ(s.teardowns.size(), static_cast<size_t>(1));
    CHECK_EQ(s.teardowns[0].text, std::string("清理会话"));
}

TEST_CASE("spec: step parameters are extracted") {
    Step step;
    std::string err;
    REQUIRE(SpecParser::parseStepText(R"( 输入用户名 "admin" 与密码 <pwd> 文件 <file:data.txt>)", step, &err));
    CHECK_EQ(step.text, std::string(R"(输入用户名 "admin" 与密码 <pwd> 文件 <file:data.txt>)"));
    CHECK_EQ(step.parameterizedText, std::string("输入用户名 {} 与密码 {} 文件 {}"));
    REQUIRE_EQ(step.args.size(), static_cast<size_t>(3));
    CHECK(step.args[0].type == ArgType::Static);
    CHECK_EQ(step.args[0].value, std::string("admin"));
    CHECK(step.args[1].type == ArgType::Dynamic);
    CHECK_EQ(step.args[1].value, std::string("pwd"));
    CHECK(step.args[2].type == ArgType::SpecialString);
    CHECK_EQ(step.args[2].value, std::string("data.txt"));
}

TEST_CASE("spec: underline style headings") {
    const char* text = "用户登录\n=======\n\n登录成功\n--------\n* 步骤一\n";
    SpecParser parser;
    ParseResult r = parser.parse(text, "x.spec");
    REQUIRE(r.ok());
    CHECK_EQ(r.specification->heading, std::string("用户登录"));
    REQUIRE_EQ(r.specification->scenarios.size(), static_cast<size_t>(1));
    CHECK_EQ(r.specification->scenarios[0].name, std::string("登录成功"));
}

TEST_CASE("spec: data table and dynamic parameter validation") {
    const char* text = R"(# 计算
|a|b|
|-|-|
|1|2|
|3|4|

## 相加
* 输入 <a> 和 <b>
* 结果 <c>
)";
    SpecParser parser;
    ParseResult r = parser.parse(text, "calc.spec");
    REQUIRE(r.specification != nullptr);
    CHECK(r.specification->isDataDriven());
    CHECK_EQ(r.specification->dataTable.rowCount(), static_cast<size_t>(2));
    CHECK_EQ(r.specification->dataTable.headers[1], std::string("b"));
    // <c> 不在数据表列中 -> 错误
    REQUIRE_EQ(r.errors.size(), static_cast<size_t>(1));
    CHECK(r.errors[0].message.find("c") != std::string::npos);
    CHECK_EQ(r.errors[0].lineNumber, 9);
}

TEST_CASE("spec: inline table argument") {
    const char* text = R"(# 购物
## 批量
* 加入商品
   |名称|数量|
   |----|----|
   |鼠标|2|
   |键盘|1|
* 结束
)";
    SpecParser parser;
    ParseResult r = parser.parse(text, "shop.spec");
    REQUIRE(r.ok());
    const Scenario& sc = r.specification->scenarios[0];
    REQUIRE_EQ(sc.steps.size(), static_cast<size_t>(2));
    CHECK(sc.steps[0].hasInlineTable());
    CHECK_EQ(sc.steps[0].parameterizedText, std::string("加入商品 {}"));
    REQUIRE_EQ(sc.steps[0].args.size(), static_cast<size_t>(1));
    CHECK_EQ(sc.steps[0].args[0].table.rowCount(), static_cast<size_t>(2));
    CHECK_EQ(sc.steps[0].args[0].table.rows[1][0], std::string("键盘"));
}

TEST_CASE("spec: errors for missing heading and steps outside scenario") {
    SpecParser parser;
    ParseResult r = parser.parse("* 孤立步骤\n", "bad.spec");
    CHECK(!r.ok());
    CHECK(!r.errors.empty());

    ParseResult r2 = parser.parse("# 只有标题\n", "empty.spec");
    // 无场景视为警告而非错误
    CHECK(r2.ok());
    CHECK(!r2.warnings.empty());
}

TEST_CASE("spec: concept expansion binds parameters") {
    ConceptDictionary dict;
    auto errs = dict.parse("# 以 <user> 身份登录\n* 输入用户名 <user>\n* 输入密码 \"pw\"\n", "auth.cpt");
    REQUIRE(errs.empty());
    CHECK_EQ(dict.size(), static_cast<size_t>(1));
    REQUIRE(dict.find("以 {} 身份登录") != nullptr);

    SpecParser parser;
    parser.setConceptDictionary(&dict);
    ParseResult r = parser.parse("# S\n## sc\n* 以 \"alice\" 身份登录\n", "s.spec");
    REQUIRE(r.ok());
    const Step& st = r.specification->scenarios[0].steps[0];
    CHECK(st.isConcept);
    REQUIRE_EQ(st.conceptSteps.size(), static_cast<size_t>(2));
    CHECK(st.conceptSteps[0].args[0].type == ArgType::Static);
    CHECK_EQ(st.conceptSteps[0].args[0].value, std::string("alice"));
    CHECK_EQ(st.conceptSteps[1].args[0].value, std::string("pw"));
}

TEST_CASE("spec: table row parsing helpers") {
    auto cells = SpecParser::parseTableRow("| a | b c |  |");
    REQUIRE_EQ(cells.size(), static_cast<size_t>(3));
    CHECK_EQ(cells[0], std::string("a"));
    CHECK_EQ(cells[1], std::string("b c"));
    CHECK_EQ(cells[2], std::string(""));
    CHECK(SpecParser::isTableSeparator("|---|:--:|"));
    CHECK(!SpecParser::isTableSeparator("|a|b|"));
}

TEST_CASE("spec: repository lists and resolves example specs") {
    SpecRepository repo;
    repo.configure(std::string(TESTHUB_SOURCE_DIR) + "/specs", "");
    auto conceptErrors = repo.reloadConcepts();
    CHECK(conceptErrors.empty());
    CHECK(repo.concepts().size() >= 1);

    auto summaries = repo.list();
    REQUIRE(summaries.size() >= 3);
    for (const auto& s : summaries) CHECK_MSG(s.valid, s.file);

    std::vector<std::string> missing;
    auto resolved = repo.resolve({"login.spec", "specs/calculator.spec", "nope.spec"}, &missing);
    CHECK_EQ(resolved.size(), static_cast<size_t>(2));
    REQUIRE_EQ(missing.size(), static_cast<size_t>(1));
    CHECK_EQ(missing[0], std::string("nope.spec"));

    // 目录解析
    auto all = repo.resolve({"."}, &missing);
    CHECK(all.size() >= 3);

    // 路径穿越被拒绝
    std::string content;
    CHECK(!repo.readRaw("../CMakeLists.txt", content));
    CHECK(repo.readRaw("login.spec", content));
    CHECK(content.find("# 用户登录") != std::string::npos);
    CHECK_EQ(repo.toRelative(resolved[0]), std::string("login.spec"));
}
