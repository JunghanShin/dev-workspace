// fault 코어 단위 시험: 조건별 올림·내림, for_s·hold_s, 워커마다, 대응 한 번, 개발 모드 건너뜀, status_lost, ack, 규칙 검사
#include <iostream>
#include <stdexcept>
#include <string>

#include "fault/engine.hpp"

using fault::Json;

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " #cond "\n"; \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const int64_t S = 1000000000;

static Json sys(const std::string& health, const std::string& cam1, const std::string& cam2 = "ok") {
    return {{"health", health}, {"workers", {{"cam1", {{"health", cam1}}}, {"cam2", {{"health", cam2}}}, {"x", {{"health", "fault"}}}}}};
}

static void test_health_rule() {
    fault::Engine e(fault::parse_rules(Json::parse(R"([
      {"id": "cam_lost", "when": {"health": {"worker": "cam*", "is": ["fault", "off"]}}, "for_s": 3, "severity": "error",
       "text": "camera lost", "do": [{"target": "payload-manager", "cmd": "payload.power", "args": {"module": "cam1", "on": true}}]}])")));
    e.set_system(sys("ok", "ok"), 0);
    CHECK(e.tick(0, true).events.empty());
    e.set_system(sys("degraded", "fault"), 1 * S);
    auto o = e.tick(1 * S, true);
    CHECK(o.events.empty() && e.active() == 0);  // 아직 3 s 안 됨
    o = e.tick(4 * S, true);
    CHECK(o.events.size() == 1 && o.events[0]["code"] == "fault_raised" && o.events[0]["severity"] == "error");
    CHECK(o.events[0]["detail"]["fault"] == "cam_lost:cam1" && o.events[0]["text"] == "camera lost (cam1)");
    CHECK(o.sends.size() == 1 && o.sends[0].action.cmd == "payload.power" && o.sends[0].fault == "cam_lost:cam1");
    CHECK(e.tick(5 * S, true).sends.empty());  // 올라가 있는 동안 다시 하지 않음
    e.action_result("cam_lost:cam1", 0, true, Json());
    auto snap = e.snapshot(5 * S);
    CHECK(snap["active"]["cam_lost:cam1"]["actions"][0]["ok"] == true && snap["worst"] == "error");
    CHECK(snap["counts"]["error"] == 1);
    CHECK(e.ack("cam_lost:cam1") && !e.ack("nope"));
    CHECK(e.snapshot(5 * S)["active"]["cam_lost:cam1"]["acked"] == true);
    e.set_system(sys("ok", "ok"), 6 * S);
    o = e.tick(6 * S, true);
    CHECK(o.events.size() == 1 && o.events[0]["code"] == "fault_cleared" && e.active() == 0);
    // 다시 올라가면 대응도 다시
    e.set_system(sys("degraded", "off"), 7 * S);
    e.tick(7 * S, true);
    CHECK(e.tick(10 * S, true).sends.size() == 1);
}

static void test_dev_mode_and_many() {
    fault::Engine e(fault::parse_rules(Json::parse(R"([
      {"id": "any", "when": {"health": {"worker": "cam*", "is": "fault"}}, "do": [{"target": "t", "cmd": "c"}]}])")));
    e.set_system(sys("degraded", "fault", "fault"), 0);
    auto o = e.tick(0, false);  // 개발 모드
    CHECK(o.events.size() == 2 && o.sends.empty());  // 워커마다 따로, 대응은 건너뜀
    CHECK(e.snapshot(0)["active"]["any:cam2"]["actions"][0]["skipped"] == "development mode");
}

static void test_metric_event_system() {
    fault::Engine e(fault::parse_rules(Json::parse(R"([
      {"id": "hot", "when": {"metric": {"worker": "sys*", "name": "temp_max_c", "above": 85}}, "for_s": 2, "severity": "warning"},
      {"id": "drop", "when": {"event": {"code": "link_*"}}, "hold_s": 5},
      {"id": "sysbad", "when": {"system": {"is": ["fault"]}}, "severity": "critical"}])")));
    e.set_system(sys("ok", "ok"), 0);
    e.set_metrics("sysmon", {{"temp_max_c", 90}});
    e.tick(0, true);
    CHECK(e.active() == 0);
    e.tick(2 * S, true);
    CHECK(e.active() == 1 && e.worst() == "warning");
    e.set_metrics("sysmon", {{"temp_max_c", 50}});
    e.tick(3 * S, true);
    CHECK(e.active() == 0);
    e.on_event("cam1", {{"code", "link_down"}}, 4 * S);
    e.on_event("cam1", {{"code", "other"}}, 4 * S);
    auto o = e.tick(4 * S, true);
    CHECK(o.events.size() == 1 && o.events[0]["detail"]["fault"] == "drop:cam1");
    e.tick(8 * S, true);
    CHECK(e.active() == 1);  // hold 5 s 안
    o = e.tick(10 * S, true);
    CHECK(o.events.size() == 1 && o.events[0]["code"] == "fault_cleared" && e.active() == 0);
    e.set_system(sys("fault", "ok"), 11 * S);
    e.tick(11 * S, true);
    CHECK(e.active() == 1 && e.worst() == "critical");
    e.remove_worker("sysmon");
}

static void test_status_lost() {
    fault::Engine e(fault::parse_rules(Json::parse(R"([{"id": "h", "when": {"health": {"is": "fault"}}}])")));
    e.set_system(sys("degraded", "fault"), 0);
    e.tick(0, true);
    const size_t before = e.active();  // cam1·x fault
    CHECK(before == 2);
    e.set_system(Json(), 1 * S);  // 끊김: 건강 조건은 판정 못 함, 올라간 이상은 그대로
    e.tick(2 * S, true);
    CHECK(e.active() == 2);
    auto o = e.tick(4 * S + 1, true);  // 3 s 넘음
    CHECK(o.events.size() == 1 && o.events[0]["detail"]["fault"] == "status_lost" && e.active() == 3);
    e.set_system(sys("ok", "ok"), 5 * S);
    e.tick(5 * S, true);
    CHECK(e.active() == 1);  // status_lost·cam1 내림, x는 여전히 fault
}

static void test_rules() {
    auto bad = [](const char* text) {
        try {
            fault::parse_rules(Json::parse(text));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(bad(R"({"id": "a"})"));
    CHECK(bad(R"([{"when": {"system": {"is": "ok"}}}])"));
    CHECK(bad(R"([{"id": "a", "when": {}}])"));
    CHECK(bad(R"([{"id": "a", "when": {"health": {"is": "x"}, "system": {"is": "y"}}}])"));
    CHECK(bad(R"([{"id": "a", "when": {"metric": {"name": "t"}}}])"));
    CHECK(bad(R"([{"id": "a", "when": {"system": {"is": "ok"}}, "severity": "panic"}])"));
    CHECK(bad(R"([{"id": "a", "when": {"system": {"is": "ok"}}, "do": [{"cmd": "x"}]}])"));
    CHECK(bad(R"([{"id": "a", "when": {"system": {"is": "ok"}}}, {"id": "a", "when": {"system": {"is": "ok"}}}])"));
    CHECK(bad(R"([{"id": "a", "when": {"event": {"code": "x"}}, "nope": 1}])"));
    CHECK(!bad(R"([{"id": "a", "when": {"event": {"code": "x", "worker": "c*"}}, "hold_s": 3, "severity": "critical", "text": "t",
                    "do": [{"target": "w", "cmd": "c", "args": {"k": 1}}]}])"));
    CHECK(fault::parse_rules(Json()).empty());
}


// 조합(all/any, 중첩): 시스템 전체로 한 번, 이름 = 규칙 id, status 없으면 모름(그대로 둠), 조합 안 이벤트는 hold_s
static void test_combined() {
    fault::Engine e(fault::parse_rules(Json::parse(R"([
      {"id": "both", "when": {"all": [{"health": {"worker": "cam*", "is": "fault"}},
                                      {"any": [{"metric": {"worker": "sys", "name": "temp", "above": 80}},
                                               {"event": {"code": "link_down"}}]}]},
       "for_s": 1, "hold_s": 5, "severity": "error"}])")));
    e.set_system(sys("degraded", "fault"), 0);
    e.set_metrics("sys", {{"temp", 50}});
    CHECK(e.tick(0, true).events.empty());                   // 건강만 참
    e.set_metrics("sys", {{"temp", 90}});
    CHECK(e.tick(1 * S, true).events.empty());               // 둘 다 참, for_s 1 s 기다림
    auto o = e.tick(2 * S, true);
    CHECK(o.events.size() == 1 && o.events[0]["detail"]["fault"] == "both" && e.active() == 1);
    CHECK(e.snapshot(2 * S)["active"]["both"]["worker"].is_null());
    e.set_system(Json(), 3 * S);                             // status 끊김: 모름 → 그대로
    o = e.tick(3 * S, true);
    CHECK(e.snapshot(3 * S)["active"].contains("both"));
    e.set_system(sys("degraded", "fault"), 4 * S);
    e.set_metrics("sys", {{"temp", 50}});
    o = e.tick(4 * S, true);
    CHECK(!e.snapshot(4 * S)["active"].contains("both"));   // any가 거짓 → 내림
    e.on_event("radio", {{"code", "link_down"}}, 4 * S);     // 이벤트로 any 참(5 s 유지)
    e.tick(4 * S, true);
    e.tick(5 * S, true);
    CHECK(e.snapshot(5 * S)["active"].contains("both"));
    e.tick(10 * S, true);                                    // hold_s 지남 → 내림
    CHECK(!e.snapshot(10 * S)["active"].contains("both"));
}

// 단계: after_s마다 심각도·대응을 올림(단계마다 한 번), 풀리면 끝, 다시 올라가면 처음부터, 개발 모드는 건너뜀
static void test_stages() {
    fault::Engine e(fault::parse_rules(Json::parse(R"([
      {"id": "cam", "when": {"health": {"worker": "cam1", "is": "fault"}}, "severity": "warning",
       "do": [{"target": "payload-manager", "cmd": "payload.restart", "args": {"module": "cam1"}}],
       "stages": [{"after_s": 5, "severity": "error", "text": "cam still lost",
                   "do": [{"target": "payload-manager", "cmd": "payload.power", "args": {"module": "cam1", "on": false}}]},
                  {"after_s": 20, "severity": "critical"}]}])")));
    e.set_system(sys("degraded", "fault"), 0);
    auto o = e.tick(0, true);
    CHECK(o.sends.size() == 1 && o.sends[0].action.cmd == "payload.restart" && o.sends[0].index == 0);
    CHECK(e.tick(4 * S, true).sends.empty());
    o = e.tick(5 * S, true);
    CHECK(o.sends.size() == 1 && o.sends[0].action.cmd == "payload.power" && o.sends[0].index == 1);
    CHECK(o.events.size() == 1 && o.events[0]["code"] == "fault_escalated" && o.events[0]["severity"] == "error");
    CHECK(o.events[0]["detail"]["stage"] == 1 && o.events[0]["text"] == "cam still lost (cam1)");
    auto snap = e.snapshot(5 * S);
    CHECK(snap["active"]["cam:cam1"]["stage"] == 1 && snap["active"]["cam:cam1"]["severity"] == "error" && snap["worst"] == "error");
    CHECK(snap["active"]["cam:cam1"]["actions"][1]["stage"] == 1);
    CHECK(e.tick(6 * S, true).sends.empty());              // 같은 단계는 다시 안 함
    o = e.tick(30 * S, true);
    CHECK(o.sends.empty() && o.events.size() == 1 && e.worst() == "critical");
    e.action_result("cam:cam1", 1, true, Json());
    CHECK(e.snapshot(30 * S)["active"]["cam:cam1"]["actions"][1]["ok"] == true);
    e.set_system(sys("ok", "ok"), 31 * S);
    o = e.tick(31 * S, true);
    CHECK(o.events.size() == 1 && o.events[0]["code"] == "fault_cleared" && e.active() == 0);
    // 다시: 처음 단계부터, 개발 모드면 건너뜀 표시
    e.set_system(sys("degraded", "fault"), 40 * S);
    e.tick(40 * S, false);
    o = e.tick(46 * S, false);
    CHECK(o.sends.empty() && e.snapshot(46 * S)["active"]["cam:cam1"]["stage"] == 1);
    CHECK(e.snapshot(46 * S)["active"]["cam:cam1"]["actions"][1]["skipped"] == "development mode");
}

static void test_rules_v2() {
    auto bad = [](const char* text) {
        try {
            fault::parse_rules(Json::parse(text));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(bad(R"([{"id": "a", "when": {"all": []}}])"));
    CHECK(bad(R"([{"id": "a", "when": {"any": [{"nope": {}}]}}])"));
    CHECK(bad(R"([{"id": "a", "when": {"all": [{"all": [{"all": [{"all": [{"all": [{"system": {"is": "ok"}}]}]}]}]}]}}])"));
    CHECK(bad(R"([{"id": "a", "when": {"system": {"is": "ok"}}, "stages": []}])"));
    CHECK(bad(R"([{"id": "a", "when": {"system": {"is": "ok"}}, "stages": [{"after_s": 0, "severity": "error"}]}])"));
    CHECK(bad(R"([{"id": "a", "when": {"system": {"is": "ok"}}, "stages": [{"after_s": 5, "severity": "error"}, {"after_s": 5, "severity": "error"}]}])"));
    CHECK(bad(R"([{"id": "a", "when": {"system": {"is": "ok"}}, "severity": "error", "stages": [{"after_s": 5, "severity": "warning"}]}])"));
    CHECK(bad(R"([{"id": "a", "when": {"system": {"is": "ok"}}, "stages": [{"after_s": 5, "severity": "error", "x": 1}]}])"));
    CHECK(!bad(R"([{"id": "a", "when": {"any": [{"system": {"is": "fault"}}, {"all": [{"event": {"code": "x"}}, {"metric": {"name": "t", "below": 1}}]}]},
                    "stages": [{"after_s": 2, "severity": "error", "do": [{"target": "w", "cmd": "c"}]}, {"after_s": 9, "severity": "critical"}]}])"));
}

int main() {
    test_combined();
    test_stages();
    test_rules_v2();
    test_health_rule();
    test_dev_mode_and_many();
    test_metric_event_system();
    test_status_lost();
    test_rules();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "fault core: all tests passed\n";
    return 0;
}
