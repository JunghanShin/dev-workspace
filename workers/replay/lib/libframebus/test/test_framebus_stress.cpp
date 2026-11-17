// libframebus 동시성 스트레스 시험. 생산자 하나와 소비자 여럿을 스레드로 동시에 세게 돌린다.
// ThreadSanitizer 빌드(dev/check.sh)로 돌리면 생산자 장부(mutex)·소비자 쪽의 데이터 경쟁도 잡는다.
//
//   - 내용 검사: 생산자는 슬롯마다 "프레임 번호 + 번호에서 만든 무늬"를 쓴다. 소비자는 들고 있는 동안
//     처음과 끝에 두 번 검사한다 → 읽는 중에 생산자가 그 슬롯을 덮어쓰면(소유권 규칙 위반) 무늬가 깨져 잡힌다
//   - InOrder 소비자: 번호가 항상 늘어난다(중복·역순 없음)
//   - Latest 소비자: 번호가 늘어난다(건너뛰기는 허용)
//   - 들고 있다가 끊는 소비자: 반납하지 않고 닫아도(죽은 소비자 흉내) 생산자가 회수한다
//   - 끝나면 모든 소비자가 떠난 뒤 슬롯 K개를 모두 다시 얻을 수 있어야 한다(참조가 새지 않음)
//
//   두 단계로 돈다: ① 빠른 소비자만(처리량 최대) ② 느린 InOrder 소비자가 낌(그 소비자가 슬롯을 붙잡아
//   생산자가 버리기 시작 — 버려도 깨짐·역순은 없어야 한다)
//
//   test_framebus_stress [단계당 초]   (기본 2초)
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "framebus/framebus.hpp"

using namespace framebus;
using Clock = std::chrono::steady_clock;

static std::atomic<int> failures{0};
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

constexpr int kSlots = 4;
constexpr uint32_t kBytes = 64 * 1024;  // 슬롯 하나 64 KiB(작은 영상 프레임 크기)

static uint8_t pattern(uint64_t id, size_t i) { return static_cast<uint8_t>((id * 131u + i * 7u) & 0xff); }

static void fill(uint8_t* p, size_t n, uint64_t id) {
    std::memcpy(p, &id, sizeof id);
    for (size_t i = sizeof id; i < n; ++i) p[i] = pattern(id, i);
}

// 들고 있는 프레임이 생산자가 쓴 그대로인지(번호 일치 + 무늬 전부)
static bool intact(const Frame& f) {
    if (f.bytes != kBytes) return false;
    uint64_t id = 0;
    std::memcpy(&id, f.data, sizeof id);
    if (id != f.frame_id) return false;
    for (size_t i = sizeof id; i < f.bytes; ++i) {
        if (f.data[i] != pattern(id, i)) return false;
    }
    return true;
}

struct Counts {
    std::atomic<uint64_t> frames{0}, corrupt{0}, order{0}, reconnects{0};
};

// 소비자 하나: 받고, 검사하고, 잠깐 들고 있다가(그동안 생산자는 계속 씀), 다시 검사하고, 반납
static void consume(const std::string& path, Consumer::Mode mode, int hold_us_max, const std::atomic<bool>& stop,
                    Counts& c, unsigned seed) {
    std::mt19937 rng(seed);
    Consumer con(path, mode);
    std::string err;
    uint64_t last = 0;
    while (!stop) {
        if (!con.connected()) {
            if (!con.connect(&err)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            ++c.reconnects;
            last = 0;
        }
        Frame f;
        const auto r = con.next(50, &f);
        if (r != Consumer::Result::Frame) continue;
        ++c.frames;
        if (!intact(f)) ++c.corrupt;
        if (last && f.frame_id <= last) ++c.order;
        last = f.frame_id;
        if (hold_us_max > 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % hold_us_max));
        if (!intact(f)) ++c.corrupt;  // 들고 있는 동안 덮어쓰였는지
        con.release(f);
    }
}

// 붙었다 떨어지기를 반복하고, 절반은 반납하지 않고 닫는다(죽은 소비자 흉내)
static void churn(const std::string& path, const std::atomic<bool>& stop, Counts& c) {
    std::mt19937 rng(7);
    std::string err;
    while (!stop) {
        Consumer con(path, rng() % 2 ? Consumer::Mode::InOrder : Consumer::Mode::Latest);
        if (!con.connect(&err)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        ++c.reconnects;
        const int take = 1 + static_cast<int>(rng() % 4);
        std::vector<Frame> held;
        for (int i = 0; i < take && !stop; ++i) {
            Frame f;
            if (con.next(50, &f) != Consumer::Result::Frame) continue;
            ++c.frames;
            if (!intact(f)) ++c.corrupt;
            held.push_back(f);
        }
        if (rng() % 2) {
            for (auto& f : held) {
                if (!intact(f)) ++c.corrupt;
                con.release(f);
            }
        }
        con.close();  // 반납 안 한 슬롯은 생산자가 끊김을 보고 회수해야 한다
    }
}

static void run(const char* title, double seconds, bool slow_inorder) {
    const std::string path = "/tmp/test_framebus_stress_" + std::to_string(getpid()) + ".sock";
    Producer p(path, Format::raw(kBytes, "stress"), kSlots);
    std::string err;
    if (!p.start(&err)) {
        std::fprintf(stderr, "start: %s\n", err.c_str());
        ++failures;
        return;
    }
    std::atomic<bool> stop{false};
    Counts inorder, latest, slow, churner;
    std::vector<std::thread> threads;
    threads.emplace_back(consume, path, Consumer::Mode::InOrder, 50, std::cref(stop), std::ref(inorder), 1u);
    threads.emplace_back(consume, path, Consumer::Mode::Latest, 300, std::cref(stop), std::ref(latest), 2u);
    threads.emplace_back(consume, path, slow_inorder ? Consumer::Mode::InOrder : Consumer::Mode::Latest, 3000,
                         std::cref(stop), std::ref(slow), 3u);  // 느린 소비자
    threads.emplace_back(churn, path, std::cref(stop), std::ref(churner));

    // 생산자: 쉬지 않고 쓴다(빈 슬롯이 없으면 버린다 — 기다리지 않음)
    uint64_t id = 0;
    const auto end = Clock::now() + std::chrono::duration<double>(seconds);
    while (Clock::now() < end) {
        auto s = p.acquire();
        if (!s) {
            std::this_thread::yield();
            continue;
        }
        ++id;
        fill(s->data, kBytes, id);
        p.publish(*s, id, 0);
    }
    stop = true;
    for (auto& t : threads) t.join();

    // 모든 소비자가 떠났다: 생산자가 끊김을 처리한 뒤 슬롯 K개를 다시 다 얻을 수 있어야 한다(참조가 새지 않음)
    for (int i = 0; i < 100 && p.stats().consumers > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(p.stats().consumers == 0);
    std::vector<Producer::Slot> all;
    for (int i = 0; i < kSlots; ++i) {
        auto s = p.acquire();
        CHECK(s.has_value());
        if (s) all.push_back(*s);
    }
    for (auto& s : all) p.abandon(s);

    const Stats st = p.stats();
    std::printf("%s: published %llu, dropped %llu, skipped %llu\n", title, (unsigned long long)st.published,
                (unsigned long long)st.dropped, (unsigned long long)st.skipped);
    for (auto [name, c] : {std::pair<const char*, Counts*>{"inorder", &inorder}, {"latest", &latest},
                           {slow_inorder ? "slow-io" : "slow-lt", &slow}, {"churn", &churner}}) {
        std::printf("  %-8s frames %llu, corrupt %llu, order %llu, connects %llu\n", name,
                    (unsigned long long)c->frames.load(), (unsigned long long)c->corrupt.load(),
                    (unsigned long long)c->order.load(), (unsigned long long)c->reconnects.load());
        CHECK(c->corrupt == 0);  // 읽는 중 덮어쓰기 없음
        CHECK(c->order == 0);    // 번호는 늘어나기만
        CHECK(c->frames > 0);
    }
    CHECK(churner.reconnects > 10);  // 붙었다 떨어지기를 충분히 반복
    CHECK(st.published > 300);  // ThreadSanitizer 빌드는 몇 배 느리다
    p.stop();
}

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 2.0;
    run("fast", seconds, false);
    run("slow-inorder", seconds, true);
    if (failures) {
        std::printf("%d check(s) failed\n", failures.load());
        return 1;
    }
    std::printf("framebus stress passed\n");
    return 0;
}
