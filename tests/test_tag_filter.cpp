#include "test_framework.h"
#include "engine/tag_filter.h"

using testhub::TagFilter;

TEST_CASE("tags: empty filter matches everything") {
    TagFilter f = TagFilter::parse("");
    CHECK(f.empty());
    CHECK(f.valid());
    CHECK(f.matches({}));
    CHECK(f.matches({"a"}));
}

TEST_CASE("tags: single tag and negation") {
    CHECK(TagFilter::parse("smoke").matches({"smoke", "auth"}));
    CHECK(!TagFilter::parse("smoke").matches({"auth"}));
    CHECK(TagFilter::parse("!slow").matches({"fast"}));
    CHECK(!TagFilter::parse("!slow").matches({"slow"}));
    CHECK(TagFilter::parse("not slow").matches({"fast"}));
}

TEST_CASE("tags: and / or / parentheses") {
    TagFilter f = TagFilter::parse("(login | search) & regression");
    CHECK(f.valid());
    CHECK(f.matches({"login", "regression"}));
    CHECK(f.matches({"search", "regression"}));
    CHECK(!f.matches({"login"}));
    CHECK(!f.matches({"regression"}));

    CHECK(TagFilter::parse("a and b").matches({"a", "b"}));
    CHECK(!TagFilter::parse("a and b").matches({"a"}));
    CHECK(TagFilter::parse("a or b").matches({"b"}));
    CHECK(TagFilter::parse("a && !b").matches({"a"}));
    CHECK(!TagFilter::parse("a || b").matches({"c"}));
}

TEST_CASE("tags: comma means AND and matching is case-insensitive") {
    TagFilter f = TagFilter::parse("smoke, auth");
    CHECK(f.matches({"smoke", "auth"}));
    CHECK(!f.matches({"smoke"}));
    CHECK(TagFilter::parse("Smoke").matches({"smoke"}));
}

TEST_CASE("tags: invalid expressions are reported") {
    std::string err;
    TagFilter f = TagFilter::parse("(a & ", &err);
    CHECK(!f.valid());
    CHECK(!err.empty());
    err.clear();
    TagFilter g = TagFilter::parse("a b", &err);
    CHECK(!g.valid());
    CHECK(!err.empty());
}

TEST_CASE("tags: all() combines multiple expressions") {
    TagFilter f = TagFilter::all({"smoke", "!slow", ""});
    CHECK(f.matches({"smoke", "fast"}));
    CHECK(!f.matches({"smoke", "slow"}));
    CHECK(!f.matches({"fast"}));
}
