// mission 코어 단위 시험: 계약 → 역할, (대상, 역할)마다 한 번에 하나·대기·busy·timeout
#include <iostream>
#include <string>

#include "mission/dispatch.hpp"

using mission::Json;

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " #cond "\n"; \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const int64_t MS = 1000000;

static mission::Request req(uint64_t id, const std::string& target, const std::string& role, int64_t t) {
    mission::Request r;
    r.id = id;
    r.target = target;
    r.role = role;
    r.cmd = role + ".x";
    r.enq_ns = t;
    return r;
}

static void test_roles() {
    const Json io = Json::parse(R"({"provides": {"mount": {"commands": {"mount.angle": {}, "mount.get": {"read": true}}},
                                                  "camera": {"commands": {"camera.zoom": {}}}},
                                     "self": {"commands": {"diag.dump": {"read": true}, "diag.reset": {}}}})");
    const auto c = mission::contract_from_io(io);
    CHECK(mission::role_of(&c, "mount.angle") == "mount");
    CHECK(mission::role_of(&c, "camera.zoom") == "camera");
    CHECK(mission::role_of(&c, "diag.reset") == "self");  // 계약의 역할에 없음: 워커 전용
    CHECK(c.read.count("mount.get") && c.read.count("diag.dump") && !c.read.count("mount.angle"));
    CHECK(mission::role_of(nullptr, "gimbal.angle") == "gimbal");  // 계약을 아직 모름: 이름 앞부분
    CHECK(mission::role_of(nullptr, "reset") == "self");
    const auto empty = mission::contract_from_io(Json());
    CHECK(mission::role_of(&empty, "mount.angle") == "self");
}

static void test_queue() {
    mission::Dispatcher d(2, 1000 * MS);
    using A = mission::Dispatcher::Admit;
    CHECK(d.submit(req(1, "cam", "mount", 0)) == A::Send);
    CHECK(d.submit(req(2, "cam", "mount", 1)) == A::Queued);
    CHECK(d.submit(req(3, "cam", "camera", 2)) == A::Send);   // 다른 역할: 기다리지 않음
    CHECK(d.submit(req(4, "loc", "mount", 3)) == A::Send);    // 다른 워커: 기다리지 않음
    CHECK(d.submit(req(5, "cam", "mount", 4)) == A::Queued);
    CHECK(d.submit(req(6, "cam", "mount", 5)) == A::Busy);    // 대기 2개 꽉 참
    CHECK(d.waiting() == 2);
    auto snap = d.snapshot(10 * MS);
    CHECK(snap["cam.mount"]["waiting"] == 2 && snap["cam.mount"]["inflight"]["cmd"] == "mount.x");
    // 끝나면 다음(온 순서)
    auto n = d.done("cam.mount");
    CHECK(n && n->id == 2);
    n = d.done("cam.mount");
    CHECK(n && n->id == 5);
    CHECK(!d.done("cam.mount"));
    CHECK(d.idle("cam.mount") && !d.idle("cam.camera"));
    // 큐에서 오래 기다리면 timeout
    mission::Dispatcher t(4, 100 * MS);
    t.submit(req(1, "a", "r", 0));
    t.submit(req(2, "a", "r", 10 * MS));
    t.submit(req(3, "a", "r", 90 * MS));
    auto ex = t.expire(150 * MS);
    CHECK(ex.size() == 1 && ex[0].id == 2);
    ex = t.expire(200 * MS);
    CHECK(ex.size() == 1 && ex[0].id == 3);
    // 대기 0: 보낸 것이 있으면 바로 busy
    mission::Dispatcher z(0, 100 * MS);
    CHECK(z.submit(req(1, "a", "r", 0)) == A::Send);
    CHECK(z.submit(req(2, "a", "r", 0)) == A::Busy);
}


static void test_access() {
    const Json peers = Json::parse(R"({"workers": {"cam": {"allow": {}, "via": {"ext": ["mount.rate", "mount.stop"], "dev": "*"}},
                                                   "pos": {"allow": {}}}})");
    const mission::Access a = mission::access_from_peers(peers);
    using V = mission::Verdict;
    CHECK(mission::check_access(a, "screen", "cam", "mount.rate", Json::object(), nullptr) == V::Pass);  // via에 없음(게이트웨이·매니저)
    CHECK(mission::check_access(a, "ext", "cam", "mount.rate", Json::object(), nullptr) == V::Allow);
    CHECK(mission::check_access(a, "ext", "cam", "mount.angle", Json::object(), nullptr) == V::Deny);
    CHECK(mission::check_access(a, "ext", "pos", "loc.reset", Json::object(), nullptr) == V::Deny);    // 그 대상엔 허락 없음
    CHECK(mission::check_access(a, "dev", "cam", "anything", Json::object(), nullptr) == V::Allow);    // "*"
    CHECK(mission::check_access(a, "ext", "cam", "lease.renew", Json{{"role", "mount"}}, nullptr) == V::Allow);
    CHECK(mission::check_access(a, "ext", "cam", "lease.renew", Json{{"role", "other"}}, nullptr) == V::Deny);
    CHECK(mission::fast_command(nullptr, "get_status") && mission::fast_command(nullptr, "lease.renew"));
    mission::Contract c;
    c.read.insert("mount.get");
    CHECK(mission::fast_command(&c, "mount.get") && !mission::fast_command(&c, "mount.rate"));
    CHECK(mission::access_from_peers(Json()).listed.empty());
}

static mission::Request preq(uint64_t id, const std::string& origin, int prio, int64_t t) {
    mission::Request r = req(id, "cam", "mount", t);
    r.origin = origin;
    r.priority = prio;
    return r;
}

static void test_priority() {
    using A = mission::Dispatcher::Admit;
    // 규칙 읽기: 정확히·앞부분·기본
    const auto rules = mission::priority_from_config(Json::parse(
        R"({"default": 50, "origins": {"fault-manager": 100, "remote:*": 30, "remote:ops": 60}, "hold_ms": 0, "roles": {"mount": {"hold_ms": 1000}}})"));
    CHECK(rules.priority_of("fault-manager") == 100 && rules.priority_of("remote:x") == 30 && rules.priority_of("remote:ops") == 60);
    CHECK(rules.priority_of("screen") == 50 && rules.hold_of("mount") == 1000 * MS && rules.hold_of("other") == 0);
    bool threw = false;
    try { mission::priority_from_config(Json::parse(R"({"nope": 1})")); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    // 큐 순서: 높은 것이 먼저, 같으면 온 순서
    mission::Dispatcher d(3);
    CHECK(d.submit(preq(1, "screen", 50, 0)) == A::Send);
    CHECK(d.submit(preq(2, "ext", 20, 1)) == A::Queued);
    CHECK(d.submit(preq(3, "screen", 50, 2)) == A::Queued);
    CHECK(d.submit(preq(4, "fault-manager", 100, 3)) == A::Queued);
    // 큐가 참: 더 낮거나 같으면 busy, 더 높으면 가장 낮은 것(ext)을 밀어냄
    CHECK(d.submit(preq(5, "ext2", 20, 4)) == A::Busy);
    CHECK(d.submit(preq(6, "screen", 50, 5)) == A::Queued);
    auto ref = d.refused();
    CHECK(ref.size() == 1 && ref[0].first.id == 2 && ref[0].second.find("preempted") != std::string::npos);
    auto n = d.done("cam.mount");
    CHECK(n && n->id == 4);
    n = d.done("cam.mount");
    CHECK(n && n->id == 3);
    n = d.done("cam.mount");
    CHECK(n && n->id == 6);
    // 규칙이 없으면 예전과 같음(모두 0, 온 순서)
    mission::Dispatcher e(4);
    CHECK(e.submit(req(1, "a", "r", 0)) == A::Send && e.submit(req(2, "a", "r", 1)) == A::Queued && e.submit(req(3, "a", "r", 2)) == A::Queued);
    CHECK(e.done("a.r")->id == 2 && e.done("a.r")->id == 3);
}

static void test_hold() {
    using A = mission::Dispatcher::Admit;
    mission::Dispatcher d(4);
    d.set_rules(mission::priority_from_config(Json::parse(R"({"roles": {"mount": {"hold_ms": 1000}}})")));
    CHECK(d.submit(preq(1, "screen", 50, 0)) == A::Send);
    CHECK(d.submit(preq(2, "ext", 20, 10 * MS)) == A::Queued);
    CHECK(!d.done("cam.mount", 100 * MS));  // screen이 끝남 → 1 s 잡아 둠, 큐의 ext(낮음)는 held로 빠짐
    auto ref = d.refused();
    CHECK(ref.size() == 1 && ref[0].first.id == 2 && ref[0].second.find("held by screen") != std::string::npos);
    CHECK(d.submit(preq(3, "ext", 20, 500 * MS)) == A::Held);       // 낮은 곳: 거절
    CHECK(d.submit(preq(4, "screen", 50, 600 * MS)) == A::Send);    // 잡은 곳 자신: 됨
    CHECK(!d.done("cam.mount", 700 * MS));                          // 다시 1 s(1700 ms까지)
    CHECK(d.snapshot(800 * MS)["cam.mount"]["holder"]["origin"] == "screen");
    CHECK(d.submit(preq(5, "fault-manager", 100, 900 * MS)) == A::Send);  // 더 높은 곳: 됨
    CHECK(!d.done("cam.mount", 950 * MS));                          // 이제 fault가 잡음
    CHECK(d.submit(preq(6, "screen", 50, 1000 * MS)) == A::Held);
    CHECK(d.submit(preq(7, "ext", 20, 2000 * MS)) == A::Send);      // 기한 지남
}

static void test_conditions() {
    const auto cs = mission::conditions_from_config(Json::parse(R"([
        {"name": "arm-mode", "target": "fc", "cmd": "flight.set_mode", "except": ["fault-manager"],
         "require": [{"metric": "armed", "op": "==", "value": 0}, {"worker": "gps", "state": "running"}], "message": "disarm first"},
        {"name": "dev-only", "cmd": "debug.*", "require": [{"mode": "development"}]},
        {"name": "soft", "target": "cam", "cmd": "*", "unknown": "allow", "require": [{"metric": "temp_c", "op": "<", "value": 80}]}])"));
    CHECK(cs.size() == 3);
    mission::World w;
    w.state = {{"fc", "running"}, {"gps", "running"}};
    w.metrics["fc"] = Json{{"armed", 1}};
    w.mode = "operation";
    mission::Request r;
    r.target = "fc";
    r.cmd = "flight.set_mode";
    r.origin = "screen";
    auto why = mission::check_conditions(cs, w, r);
    CHECK(why && why->find("arm-mode") != std::string::npos && why->find("fc.armed is 1") != std::string::npos && why->find("disarm first") != std::string::npos);
    r.origin = "fault-manager";
    CHECK(!mission::check_conditions(cs, w, r));  // except
    r.origin = "screen";
    w.metrics["fc"]["armed"] = 0;
    CHECK(!mission::check_conditions(cs, w, r));
    w.state["gps"] = "stopped";
    why = mission::check_conditions(cs, w, r);
    CHECK(why && why->find("gps state is stopped") != std::string::npos);
    w.state.erase("gps");
    why = mission::check_conditions(cs, w, r);
    CHECK(why && why->find("unknown") != std::string::npos);  // 모르면 막음(기본)
    r.target = "x";
    r.cmd = "debug.dump";
    CHECK(mission::check_conditions(cs, w, r).has_value());
    w.mode = "development";
    CHECK(!mission::check_conditions(cs, w, r));
    r.target = "cam";
    r.cmd = "cam.zoom";
    CHECK(!mission::check_conditions(cs, w, r));  // 지표 없음 + unknown allow
    w.metrics["cam"] = Json{{"temp_c", 85}};
    CHECK(mission::check_conditions(cs, w, r).has_value());
    bool threw = false;
    try { mission::conditions_from_config(Json::parse(R"([{"require": [{"metric": "a", "op": "~", "value": 1}]}])")); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(mission::conditions_from_config(Json()).empty());
}

int main() {
    test_priority();
    test_hold();
    test_conditions();
    test_access();
    test_roles();
    test_queue();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mission core: all tests passed\n";
    return 0;
}
