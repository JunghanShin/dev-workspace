// libframebus 단위 시험: 전달, 최신 우선, 빈 슬롯 없을 때 버림, 소비자 끊김 시 참조 해제, 생산자 종료 알림,
// 받는 스레드가 도는 동안 다른 스레드에서 skipped() 읽기(ThreadSanitizer로 돌릴 때 경합이 없어야 한다)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

#include "framebus/framebus.hpp"

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

using namespace framebus;

static void put(Producer& p, uint64_t id) {
    auto s = p.acquire();
    if (!s) return;
    std::memset(s->data, static_cast<int>(id & 0xff), s->bytes);
    p.publish(*s, id, static_cast<int64_t>(id) * 1000);
}

static void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }

int main() {
    const std::string path = "/tmp/test_framebus_" + std::to_string(getpid()) + ".sock";
    Format fmt;
    fmt.width = 64;
    fmt.height = 4;
    fmt.stride = 64;
    Producer p(path, fmt, 3);
    std::string err;
    CHECK(p.start(&err));

    // 소비자가 없으면 슬롯은 바로 빈다
    for (int i = 0; i < 10; ++i) put(p, i);
    CHECK(p.stats().dropped == 0);

    Consumer c(path);
    CHECK(c.connect(&err));
    CHECK(c.format().width == 64 && c.format().height == 4 && c.format().pixel == "GRAY8");
    settle();
    CHECK(p.stats().consumers == 1);

    // 전달
    put(p, 21);
    Frame f;
    CHECK(c.next(1000, &f) == Consumer::Result::Frame);
    CHECK(f.frame_id == 21 && f.bytes == 256 && f.data[0] == 21 && f.data[255] == 21);
    c.release(f);
    settle();

    // 최신 우선 + 빈 슬롯 없음: 소비자가 읽지 않는 동안 3개를 채우면 4번째는 버림
    put(p, 31);
    put(p, 32);
    put(p, 33);
    put(p, 34);  // 슬롯 3개를 소비자가 모두 들고 있다
    CHECK(p.stats().dropped == 1);
    CHECK(c.next(1000, &f) == Consumer::Result::Frame);
    CHECK(f.frame_id == 33 && f.data[0] == 33);
    CHECK(c.skipped() == 2);
    c.release(f);
    settle();
    put(p, 35);  // 모두 반납했으니 다시 쓸 수 있다
    CHECK(p.stats().dropped == 1);
    CHECK(c.next(1000, &f) == Consumer::Result::Frame && f.frame_id == 35);
    // 이 프레임은 반납하지 않고 끊는다 → 생산자가 참조를 푼다
    c.close();
    settle();
    CHECK(p.stats().consumers == 0);
    put(p, 41);
    put(p, 42);
    put(p, 43);
    put(p, 44);
    CHECK(p.stats().dropped == 1);

    // 다시 붙고, 생산자가 끝나면 소비자는 끊김을 안다
    CHECK(c.connect(&err));
    settle();
    p.stop();
    CHECK(c.next(1000, &f) == Consumer::Result::Disconnected);

    // 데이터(영상이 아님) + 순서대로 전부: 크기가 바뀌는 레코드를 빠짐없이 순서대로 받는다
    {
        const std::string dpath = path + ".data";
        Producer dp(dpath, Format::raw(1024, "f32x256"), 4);
        CHECK(dp.start(&err));
        Consumer dc(dpath, Consumer::Mode::InOrder);
        CHECK(dc.connect(&err));
        CHECK(dc.format().pixel == "f32x256" && dc.format().frame_bytes() == 1024);
        settle();
        for (uint64_t id = 1; id <= 3; ++id) {
            auto s = dp.acquire();
            CHECK(s.has_value());
            if (!s) break;
            std::memset(s->data, static_cast<int>(id), 100 * id);
            dp.publish(*s, id, 0, 100 * id);  // 레코드마다 크기가 다르다
        }
        for (uint64_t id = 1; id <= 3; ++id) {
            Frame df;
            CHECK(dc.next(1000, &df) == Consumer::Result::Frame);
            CHECK(df.frame_id == id && df.bytes == 100 * id && df.data[0] == id);
            dc.release(df);
        }
        CHECK(dc.skipped() == 0);
        dp.stop();
    }

    // 받는 스레드가 next()를 도는 동안 다른 스레드(상태 보고)가 skipped()를 읽는다
    {
        const std::string tpath = path + ".thr";
        Producer tp(tpath, fmt, 3);
        CHECK(tp.start(&err));
        Consumer tc(tpath);
        CHECK(tc.connect(&err));
        settle();
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> got{0};
        std::thread reader([&] {
            Frame tf;
            while (!stop.load()) {
                if (tc.next(50, &tf) != Consumer::Result::Frame) continue;
                ++got;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));  // 일부러 늦게 읽어 건너뛰게 한다
                tc.release(tf);
            }
        });
        uint64_t seen = 0;
        for (uint64_t id = 1; id <= 300; ++id) {
            put(tp, id);
            seen = tc.skipped();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        settle();
        stop = true;
        reader.join();
        CHECK(got.load() > 0);
        CHECK(tc.skipped() >= seen);
        CHECK(seen > 0);  // 실제로 건너뛰기가 일어나야 이 시험이 뜻이 있다
        tp.stop();
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("framebus: all tests passed\n");
    return 0;
}
