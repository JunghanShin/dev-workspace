// processor 시험: 유닛(Window) + 모듈(가짜 SourcePort로, 호스트 없이). 포트 덕분에 source 없이 따로 시험한다
#include <cstdio>
#include <vector>

#include "processor/module.hpp"
#include "processor/window.hpp"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

// 가짜 포트: 시험이 표본을 직접 넣는다
struct FakeSource : source::SourcePort {
    std::vector<Listener> ls;
    void subscribe(Listener fn) override { ls.push_back(std::move(fn)); }
    void emit(uint64_t seq, std::vector<float> v) {
        for (auto& fn : ls) fn(source::Sample{seq, 0, &v});
    }
};

static void unit_window() {
    processor::Window w(3);
    w.add(1);
    w.add(2);
    w.add(3);
    CHECK(w.avg() == 2 && w.min() == 1 && w.max() == 3);
    w.add(6);  // 1이 빠진다
    CHECK(w.count() == 3 && w.avg() == 11.0 / 3 && w.min() == 2 && w.total() == 4);
    bool threw = false;
    try {
        w.set_size(1);
    } catch (const processor::ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

static void module_with_fake_port() {
    FakeSource src;
    processor::Module m(src);
    m.configure({{"window", 2}});
    m.attach();  // start가 하는 일(포트 구독)을 호스트 없이
    src.emit(1, {1, 3});  // 평균 2
    src.emit(2, {4, 4});  // 평균 4
    CHECK(m.status()["avg"] == 3.0 && m.status()["last_seq"] == 2);
    CHECK(m.reconfigure({{"window", 5}}));
    CHECK(worker::call_module(m, "reset")["data"]["count"] == 0);
    CHECK(m.status()["count"] == 0);
}

int main() {
    unit_window();
    module_with_fake_port();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all processor tests passed\n");
    return 0;
}
