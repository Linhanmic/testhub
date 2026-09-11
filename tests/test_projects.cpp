#include "test_framework.h"
#include "testhub.h"
#include "util/json.h"

using namespace testhub;

TEST_CASE("projects: empty config synthesizes default project") {
    TestHubConfig c;
    c.finalizeProjects();
    REQUIRE_EQ(c.projects.size(), static_cast<size_t>(1));
    CHECK_EQ(c.projects[0].id, std::string("default"));
    CHECK_EQ(c.projects[0].name, std::string("默认"));
    CHECK_EQ(c.currentProjectId, std::string("default"));
    CHECK_EQ(c.specsDir, std::string("specs"));
}

TEST_CASE("projects: JSON current selects matching dir") {
    Json j = Json::parse(R"({
      "specs": {
        "dir": "specs",
        "current": "alt",
        "projects": [
          {"id":"main","name":"主规范","dir":"specs"},
          {"id":"alt","name":"备用","dir":"examples/alt-specs","concepts_dir":""}
        ]
      }
    })");
    TestHubConfig c;
    c.applyJson(j);
    c.finalizeProjects();
    CHECK_EQ(c.projects.size(), static_cast<size_t>(2));
    CHECK_EQ(c.currentProjectId, std::string("alt"));
    CHECK_EQ(c.specsDir, std::string("examples/alt-specs"));
    CHECK_EQ(c.projects[1].name, std::string("备用"));
}

TEST_CASE("projects: missing ids are slugged and uniquified") {
    Json j = Json::parse(R"({
      "specs": {
        "projects": [
          {"name":"Alpha Suite","dir":"a"},
          {"name":"Alpha Suite","dir":"b"},
          {"dir":"c"}
        ]
      }
    })");
    TestHubConfig c;
    c.applyJson(j);
    c.finalizeProjects();
    REQUIRE_EQ(c.projects.size(), static_cast<size_t>(3));
    CHECK_EQ(c.projects[0].id, std::string("alpha-suite"));
    CHECK_EQ(c.projects[1].id, std::string("alpha-suite-2"));
    CHECK_EQ(c.projects[2].id, std::string("project-3"));
    CHECK_EQ(c.currentProjectId, std::string("alpha-suite"));
    CHECK_EQ(c.specsDir, std::string("a"));
}

TEST_CASE("projects: specs dir override selects or rewrites") {
    TestHubConfig c;
    c.applyJson(Json::parse(R"({
      "specs": {
        "projects": [
          {"id":"main","dir":"specs"},
          {"id":"alt","dir":"examples/alt-specs"}
        ]
      }
    })"));
    c.finalizeProjects();
    CHECK_EQ(c.currentProjectId, std::string("main"));

    c.applySpecsDirOverride("examples/alt-specs");
    CHECK_EQ(c.currentProjectId, std::string("alt"));
    CHECK_EQ(c.specsDir, std::string("examples/alt-specs"));

    c.applySpecsDirOverride("/tmp/custom-specs");
    CHECK_EQ(c.currentProjectId, std::string("alt"));
    CHECK_EQ(c.specsDir, std::string("/tmp/custom-specs"));
    CHECK_EQ(c.findProject("alt")->dir, std::string("/tmp/custom-specs"));
}

TEST_CASE("projects: toJson roundtrip keeps list and current") {
    TestHubConfig c;
    c.applyJson(Json::parse(R"({"specs":{"current":"p2","projects":[{"id":"p1","name":"One","dir":"d1"},{"id":"p2","name":"Two","dir":"d2"}]}})"));
    c.finalizeProjects();
    Json out = c.toJson();
    CHECK_EQ(out["specs"]["current"].asString(), std::string("p2"));
    CHECK_EQ(out["specs"]["projects"].size(), static_cast<size_t>(2));
    CHECK_EQ(out["specs"]["dir"].asString(), std::string("d2"));

    TestHubConfig again;
    again.applyJson(out);
    again.finalizeProjects();
    CHECK_EQ(again.currentProjectId, std::string("p2"));
    CHECK_EQ(again.specsDir, std::string("d2"));
}
