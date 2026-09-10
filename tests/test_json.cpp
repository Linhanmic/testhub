#include "test_framework.h"
#include "util/json.h"

using testhub::Json;

TEST_CASE("json: parse scalars") {
    CHECK(Json::parse("null").isNull());
    CHECK_EQ(Json::parse("true").asBool(), true);
    CHECK_EQ(Json::parse("false").asBool(), false);
    CHECK_EQ(Json::parse("42").asInt(), 42);
    CHECK_EQ(Json::parse("-3.5").asNumber(), -3.5);
    CHECK_EQ(Json::parse("1e3").asNumber(), 1000.0);
    CHECK_EQ(Json::parse("\"hi\"").asString(), std::string("hi"));
}

TEST_CASE("json: parse nested structures and whitespace") {
    Json j = Json::parse(" { \"a\" : [1, 2, {\"b\": null}], \"c\": {\"d\": \"e\"} } ");
    REQUIRE(j.isObject());
    CHECK_EQ(j["a"].size(), static_cast<size_t>(3));
    CHECK_EQ(j["a"][1].asInt(), 2);
    CHECK(j["a"][2]["b"].isNull());
    CHECK_EQ(j["c"]["d"].asString(), std::string("e"));
    CHECK(j["missing"].isNull());
    CHECK(j.contains("c"));
    CHECK(!j.contains("zzz"));
}

TEST_CASE("json: string escapes and unicode") {
    Json j = Json::parse(R"("line\nbreak \"quoted\" \\ \u4e2d\u6587 \ud83d\ude00")");
    CHECK_EQ(j.asString(), std::string("line\nbreak \"quoted\" \\ 中文 \xF0\x9F\x98\x80"));
    std::string dumped = Json("tab\there\x01").dump();
    CHECK_EQ(dumped, std::string("\"tab\\there\\u0001\""));
    // 非 ASCII 原样输出
    CHECK_EQ(Json("中文").dump(), std::string("\"中文\""));
}

TEST_CASE("json: parse errors are reported") {
    std::string err;
    Json j = Json::tryParse("{bad", &err);
    CHECK(!err.empty());
    CHECK(j.isNull());
    CHECK_THROWS(Json::parse("[1,2"), Json::ParseError);
    CHECK_THROWS(Json::parse("{\"a\":1} trailing"), Json::ParseError);
    CHECK_THROWS(Json::parse(""), Json::ParseError);
    CHECK_THROWS(Json::parse("\"unterminated"), Json::ParseError);
}

TEST_CASE("json: build and dump") {
    Json j = Json::object();
    j["name"] = "TestHub";
    j["version"] = 1;
    j["ok"] = true;
    j["ratio"] = 0.5;
    j["list"] = Json::array();
    j["list"].push(1).push("two").push(nullptr);
    j["nested"]["deep"] = "value";
    std::string s = j.dump();
    CHECK_EQ(s, std::string(R"({"list":[1,"two",null],"name":"TestHub","nested":{"deep":"value"},"ok":true,"ratio":0.5,"version":1})"));
    // 往返
    Json back = Json::parse(s);
    CHECK_EQ(back.dump(), s);
}

TEST_CASE("json: pretty print") {
    Json j = Json::object();
    j["a"] = 1;
    j["b"] = Json::array();
    j["b"].push(true);
    std::string pretty = j.dump(2);
    CHECK_EQ(pretty, std::string("{\n  \"a\": 1,\n  \"b\": [\n    true\n  ]\n}"));
}

TEST_CASE("json: numbers serialize compactly") {
    CHECK_EQ(Json(3).dump(), std::string("3"));
    CHECK_EQ(Json(3.0).dump(), std::string("3"));
    CHECK_EQ(Json(2.5).dump(), std::string("2.5"));
    CHECK_EQ(Json(1e21).dump(), std::string("1e+21"));
    CHECK_EQ(Json(-0.25).dump(), std::string("-0.25"));
    CHECK_EQ(Json(123456789012LL).dump(), std::string("123456789012"));
}

TEST_CASE("json: copy-on-write semantics") {
    Json a = Json::object();
    a["x"] = 1;
    Json b = a;
    b["x"] = 2;
    CHECK_EQ(a["x"].asInt(), 1);
    CHECK_EQ(b["x"].asInt(), 2);

    Json arr = Json::array();
    arr.push(1);
    Json arr2 = arr;
    arr2.push(2);
    CHECK_EQ(arr.size(), static_cast<size_t>(1));
    CHECK_EQ(arr2.size(), static_cast<size_t>(2));
}

TEST_CASE("json: integer literal indexing on mutable values") {
    Json j = Json::parse(R"({"list":[10,20]})");
    // 非 const 对象上 j["list"][0] 必须走数组索引而不是 const char* 重载
    CHECK_EQ(j["list"][0].asInt(), 10);
    CHECK_EQ(j["list"][1].asInt(), 20);
    CHECK(j["list"][5].isNull());  // 可写索引会扩容，新元素为 null
    CHECK_EQ(j["list"].size(), static_cast<size_t>(6));
    const Json c = Json::parse("[1,2]");
    CHECK_EQ(c[1].asInt(), 2);
    CHECK(c[7].isNull());
    CHECK(c[-1].isNull());
    Json arr;
    arr[2] = "x";
    CHECK(arr.isArray());
    CHECK(arr[0].isNull());
    CHECK_EQ(arr[2].asString(), std::string("x"));
}

TEST_CASE("json: erase and conversions from containers") {
    std::vector<std::string> v = {"a", "b"};
    Json j(v);
    CHECK(j.isArray());
    CHECK_EQ(j[1].asString(), std::string("b"));

    std::map<std::string, int> m = {{"k", 7}};
    Json o(m);
    CHECK_EQ(o["k"].asInt(), 7);
    o.erase("k");
    CHECK(!o.contains("k"));
    CHECK(o.empty());
}
