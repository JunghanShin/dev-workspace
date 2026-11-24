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

int main() {
    test_roles();
    test_queue();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mission core: all tests passed\n";
    return 0;
}
