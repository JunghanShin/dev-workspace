// source 시험: 유닛(Generator, libworker 없이) + 모듈(호스트 없이 설정 칸·바로 적용·명령)
#include <cstdio>

#include "source/generator.hpp"
#include "source/module.hpp"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

template <class E, class F>
static bool throws(F f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}

// ① 유닛: 시각을 직접 넘긴다(가짜 시계)
static void unit_generator() {
    source::Generator g({10, 4, 1.0});  // 10 Hz → 100 ms
    CHECK(!g.due(1000));                // 첫 호출은 기준만
    CHECK(!g.due(1099));
    CHECK(g.due(1100));
    CHECK(g.make().size() == 4 && g.seq() == 1);
    CHECK(!g.due(1150) && g.due(1200));
    CHECK(g.due(5000) && g.next_due_ms() == 5100);  // 많이 밀리면 한 번만 만들고 다시 맞춤
    g.reset();
    CHECK(g.seq() == 0);
    CHECK(throws<source::ConfigError>([&] { g.set_config({0, 4, 1}); }));
    CHECK(throws<source::ConfigError>([&] { g.set_config({10, 0, 1}); }));
}

// ② 모듈: 설정 칸, 바로 적용, 명령(호스트·소켓 없이)
static void module_config_and_commands() {
    source::Module m;
    CHECK(m.name() == "source");
    m.configure({{"rate_hz", 20}, {"size", 8}});
    CHECK(m.status()["rate_hz"] == 20 && m.status()["size"] == 8);
    CHECK(m.reconfigure({{"rate_hz", 50}, {"size", 8}}));    // 주기는 바로
    CHECK(!m.reconfigure({{"rate_hz", 50}, {"size", 16}}));  // 크기는 데이터 버스 슬롯이 바뀌므로 다시 시작
    CHECK(worker::call_module(m, "set_rate", {{"hz", 5}})["data"]["rate_hz"] == 5);
    CHECK(worker::call_module(m, "set_rate", {{"hz", 0}})["error"]["code"] == "bad_request");
    CHECK(worker::call_module(m, "reset")["data"]["seq"] == 0);
    CHECK(worker::call_module(m, "nope")["ok"] == false);
    CHECK(throws<worker::ConfigError>([&] { m.configure({{"rate_hz", 5000}}); }));
    CHECK(throws<worker::ConfigError>([&] { m.configure({{"unknown", 1}}); }));
}

int main() {
    unit_generator();
    module_config_and_commands();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all source tests passed\n");
    return 0;
}
