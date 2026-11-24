// status 코어 단위 시험: 워커 건강 표, 시스템 건강(필수 규칙·autostart 기본·process-manager 끊김), 바뀔 때만 이벤트, 규칙 검사
#include <iostream>
#include <stdexcept>
#include <string>

#include "status/health.hpp"

using status::Json;

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " #cond "\n"; \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const int64_t S = 1000000000;

static Json w(const std::string& id, const std::string& actual, bool autostart = true, Json metrics = Json::object(),
              double age_ms = 100, const std::string& desired = "running") {
    return {{"id", id}, {"actual_state", actual}, {"desired_state", desired}, {"autostart", autostart},
            {"metrics", metrics}, {"last_heartbeat_ms_ago", age_ms}, {"kind", "worker"}};
}

static std::string health_of(status::Health& h, const std::string& id, int64_t now) {
    return h.evaluate(now).system["workers"][id]["health"].get<std::string>();
}

static void test_worker_table() {
    status::Health h(status::parse_rules(Json::parse(R"({"metrics":[{"worker":"cam*","metric":"camera_fps","below":10}]})")));
    h.set_pm_link(true);
    h.on_worker(w("a", "running"), 0);
    CHECK(health_of(h, "a", 0) == "ok");
    h.on_worker(w("a", "running", true, {{"link_link", 0}, {"inputs_up_link", 1}}), 0);
    auto r = h.evaluate(0);
    CHECK(r.system["workers"]["a"]["health"] == "degraded");
    CHECK(r.system["workers"]["a"]["links"]["link_link"] == 0);
    CHECK(r.system["workers"]["a"]["why"] == "link down (link_link=0)");
    h.on_worker(w("a", "running", true, Json::object(), 5000), 0);
    CHECK(health_of(h, "a", 0) == "fault");                        // heartbeat 끊김(3 s 넘음)
    h.on_worker(w("a", "running", true, Json::object(), 100), 0);
    CHECK(health_of(h, "a", 4 * S) == "fault");                    // 받은 뒤 시간이 지나도 늘어남
    h.on_worker(w("a", "starting"), 0);
    CHECK(health_of(h, "a", 0) == "starting");
    h.on_worker(w("a", "exited"), 0);
    CHECK(health_of(h, "a", 0) == "fault");
    Json cl = w("a", "starting");
    cl["crash_loop"] = true;
    h.on_worker(cl, 0);
    CHECK(health_of(h, "a", 0) == "fault");
    h.on_worker(w("a", "stopped", true, Json::object(), 0, "stopped"), 0);
    CHECK(health_of(h, "a", 0) == "off");
    h.on_worker(w("a", "disabled"), 0);
    CHECK(health_of(h, "a", 0) == "off");
    // 규칙 지표 조건: cam*의 camera_fps < 10 → degraded
    h.on_worker(w("cam1", "running", true, {{"camera_fps", 5}}), 0);
    CHECK(health_of(h, "cam1", 0) == "degraded");
    h.on_worker(w("cam1", "running", true, {{"camera_fps", 30}}), 0);
    CHECK(health_of(h, "cam1", 0) == "ok");
}

static void test_system() {
    status::Health h;  // required 비움: autostart 워커가 필수
    h.set_pm_link(true);
    h.on_worker(w("req", "running", true), 0);
    h.on_worker(w("opt", "running", false), 0);
    auto r = h.evaluate(0);
    CHECK(r.system["health"] == "ok" && r.system["counts"]["ok"] == 2);
    CHECK(r.system["workers"]["req"]["required"] == true && r.system["workers"]["opt"]["required"] == false);
    h.on_worker(w("opt", "exited", false), 0);
    CHECK(h.evaluate(0).system["health"] == "degraded");             // 필수 아닌 워커 fault → degraded
    h.on_worker(w("opt", "stopped", false, Json::object(), 0, "stopped"), 0);
    CHECK(h.evaluate(0).system["health"] == "ok");                   // 필수 아닌 워커 off는 괜찮음
    h.on_worker(w("req", "running", true, {{"x_link", 0}}), 0);
    CHECK(h.evaluate(0).system["health"] == "degraded");
    h.on_worker(w("req", "stopped", true, Json::object(), 0, "stopped"), 0);
    r = h.evaluate(0);
    CHECK(r.system["health"] == "fault");                             // 필수 워커 off → fault
    CHECK(r.system["reasons"][0].get<std::string>().rfind("req: off", 0) == 0);
    h.on_worker(w("req", "running"), 0);
    h.set_pm_link(false);
    r = h.evaluate(0);
    CHECK(r.system["health"] == "degraded" && r.system["pm_link"] == false);
    // required 규칙: 글롭
    status::Health g(status::parse_rules(Json::parse(R"({"required":["cam*"]})")));
    g.set_pm_link(true);
    g.on_worker(w("cam1", "exited", false), 0);
    g.on_worker(w("other", "exited", true), 0);
    r = g.evaluate(0);
    CHECK(r.system["workers"]["cam1"]["required"] == true && r.system["workers"]["other"]["required"] == false);
    CHECK(r.system["health"] == "fault");
    // 빠짐
    g.on_removed("cam1");
    CHECK(!g.evaluate(0).system["workers"].contains("cam1"));
}

static void test_events() {
    status::Health h;
    h.set_pm_link(true);
    h.on_worker(w("a", "running"), 0);
    auto r = h.evaluate(1);
    CHECK(r.events.size() == 1 && r.events[0]["code"] == "health_changed" && r.events[0]["detail"]["to"] == "ok");
    CHECK(h.evaluate(2).events.empty());  // 그대로면 없음
    h.on_worker(w("a", "running", true, {{"l_link", 0}}), 0);
    r = h.evaluate(3);
    CHECK(r.events.size() == 2);
    CHECK(r.events[0]["code"] == "worker_health_changed" && r.events[0]["severity"] == "warning" && r.events[0]["detail"]["to"] == "degraded");
    CHECK(r.events[1]["code"] == "health_changed" && r.events[1]["detail"]["from"] == "ok");
    for (const auto& e : r.events)
        CHECK(e.contains("seq") && e["t_mono_ns"] == 3 && e["source"] == "status-manager.status" && e.contains("text"));
}

static void test_rules() {
    auto bad = [](const char* text) {
        try {
            status::parse_rules(Json::parse(text));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(bad(R"({"nope":1})"));
    CHECK(bad(R"({"required":"cam1"})"));
    CHECK(bad(R"({"stale_s":0})"));
    CHECK(bad(R"({"metrics":[{"metric":"x"}]})"));
    CHECK(bad(R"({"metrics":[{"metric":"x","below":1,"then":"panic"}]})"));
    CHECK(!bad(R"({"required":["a*"],"stale_s":2,"publish_ms":500,"metrics":[{"metric":"x","above":1,"then":"fault"}]})"));
}

int main() {
    test_worker_table();
    test_system();
    test_events();
    test_rules();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "status core: all tests passed\n";
    return 0;
}
