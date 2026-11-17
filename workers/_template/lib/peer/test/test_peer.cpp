// peer 시험: 유닛(SeqTracker) + 모듈(호스트 없이 설정 칸·꺼진 상태·명령). 실제 워커 간 통신은 워커 시험(test/test_worker.cpp)
#include <cstdio>

#include "peer/module.hpp"
#include "peer/seq_tracker.hpp"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static void unit_tracker() {
    peer::SeqTracker t;
    CHECK(t.on(1) == 0 && t.on(2) == 0);
    CHECK(t.on(5) == 2);  // 3, 4가 빠짐
    CHECK(t.received() == 3 && t.lost() == 2 && t.last() == 5);
    t.on(1);  // 상대가 다시 시작
    CHECK(t.restarts() == 1 && t.lost() == 2);
}

static void module_without_peer() {
    peer::Module m;
    CHECK(m.name() == "peer");
    m.configure({{"peer", ""}});
    CHECK(m.status()["peer"] == "" && m.status()["received"] == 0);
    CHECK(worker::call_module(m, "set_rate", {{"hz", 5}})["error"]["code"] == "unavailable");
    worker::Metrics mx(m.name());
    m.metrics(mx);
    CHECK(mx.str().empty());  // 상대가 없으면 지표도 없음
    bool threw = false;
    try {
        m.configure({{"bad", 1}});
    } catch (const worker::ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    unit_tracker();
    module_without_peer();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all peer tests passed\n");
    return 0;
}
