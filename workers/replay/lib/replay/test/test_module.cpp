// 모듈 시험: 호스트 없이 설정 칸 읽기·검사, 명령 인자 검사, 시작 전 상태(녹화·재생 없음)
#include <cstdio>
#include <string>

#include "replay/module.hpp"

static int failures = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                              \
        }                                                            \
    } while (0)

int main() {
    using worker::Json;
    // 설정
    const auto c = replay::Module::parse(Json{{"dir", "/tmp/x"}, {"max_total_mb", 10}}, "rp");
    CHECK(c.dir == "/tmp/x" && c.max_total_mb == 10 && !c.bus_socket.empty());
    bool threw = false;
    try {
        replay::Module::parse(Json{{"bus_slots", 1}}, "rp");
    } catch (const worker::ConfigError&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        replay::Module::parse(Json{{"nope", 1}}, "rp");
    } catch (const worker::ConfigError&) {
        threw = true;
    }
    CHECK(threw);

    // 명령 인자 검사(호스트 없이: 스레드를 띄우기 전에 거르는 것만)
    replay::Module m;
    m.configure(Json{{"dir", "/tmp/replay_module_test_none"}});
    CHECK(worker::call_module(m, "record_start", {{"name", "a b"}, {"source", "x"}})["error"]["code"] == "bad_request");
    CHECK(worker::call_module(m, "record_start", {{"name", "ok"}})["error"]["code"] == "bad_request");  // source 없음
    CHECK(worker::call_module(m, "play_start", {{"name", "none"}})["error"]["code"] == "bad_request");  // 녹화 없음
    CHECK(worker::call_module(m, "play_start", {{"name", "x"}, {"speed", 0}})["error"]["code"] == "bad_request");
    const Json l = worker::call_module(m, "list");
    CHECK(l.value("ok", false) && l["data"]["recordings"].empty());
    CHECK(worker::call_module(m, "nope").value("ok", true) == false);
    const Json s = m.status();
    CHECK(!s["recording"]["active"].get<bool>() && !s["playing"]["active"].get<bool>());

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all replay module tests passed\n");
    return 0;
}
