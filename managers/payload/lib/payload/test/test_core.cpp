// payload 코어 단위 시험: 모듈 목록(매니저 빼기, 역할·건강 합치기), 묶음 글롭, 규칙 검사
#include <iostream>
#include <stdexcept>
#include <string>

#include "payload/modules.hpp"

using payload::Json;

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " #cond "\n"; \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static Json w(const std::string& id, const std::string& kind, const std::string& state = "running") {
    return {{"id", id}, {"kind", kind}, {"actual_state", state}, {"desired_state", "running"}, {"autostart", true}};
}

static void test_modules() {
    payload::Modules m(payload::parse_groups(Json::parse(R"({"cams": ["cam*"], "all": ["*"], "none": ["zzz*"]})")));
    m.on_worker(w("cam1", "worker"));
    m.on_worker(w("cam2", "worker", "stopped"));
    m.on_worker(w("sensor", "worker"));
    m.on_worker(w("mission-manager", "manager"));
    m.set_roles("cam1", {"camera", "mount"});
    m.set_health(Json::parse(R"({"workers": {"cam1": {"health": "degraded", "why": "link down"}}})"));
    CHECK(m.count() == 3 && m.running() == 2);
    CHECK(m.is_module("cam1") && !m.is_module("mission-manager") && m.is_manager("mission-manager") && !m.known("x"));
    const Json s = m.snapshot();
    CHECK(!s["modules"].contains("mission-manager"));
    CHECK(s["modules"]["cam1"]["roles"] == Json::parse(R"(["camera","mount"])"));
    CHECK(s["modules"]["cam1"]["health"] == "degraded" && s["modules"]["cam1"]["why"] == "link down");
    CHECK(s["modules"]["sensor"]["health"].is_null());  // status에 없음
    CHECK(s["modules"]["cam2"]["state"] == "stopped");
    CHECK(s["modules"]["cam1"]["groups"] == Json::parse(R"(["all","cams"])"));
    CHECK(s["groups"]["cams"] == Json::parse(R"(["cam1","cam2"])"));
    CHECK(s["groups"]["all"] == Json::parse(R"(["cam1","cam2","sensor"])"));  // 매니저는 빠짐
    CHECK(s["groups"]["none"] == Json::array());
    CHECK(m.has_group("cams") && !m.has_group("x"));
    m.set_health(Json());
    CHECK(m.snapshot()["modules"]["cam1"]["health"].is_null());
    m.on_removed("cam2");
    CHECK(m.members("cams") == std::vector<std::string>{"cam1"});
}

static void test_rules() {
    auto bad = [](const char* text) {
        try {
            payload::parse_groups(Json::parse(text));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(bad(R"(["a"])"));
    CHECK(bad(R"({"g": "cam*"})"));
    CHECK(bad(R"({"g": [1]})"));
    CHECK(bad(R"({"": ["a"]})"));
    CHECK(!bad(R"({"g": ["cam*", "sensor"]})"));
    CHECK(payload::parse_groups(Json()).empty());
}

int main() {
    test_modules();
    test_rules();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "payload core: all tests passed\n";
    return 0;
}
