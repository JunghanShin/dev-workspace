// 유닛 시험: 녹화 파일(코어만, libworker 없이). 쓰기 → 읽기 같은 내용·순서, 목록·삭제, 이름 검사, 잘린 파일
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "replay/recording.hpp"

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

int main() {
    char tmpl[] = "/tmp/replay_test_XXXXXX";
    const std::string root = std::string(mkdtemp(tmpl)) + "/rec";

    CHECK(replay::valid_name("cam-01_a"));
    CHECK(!replay::valid_name("") && !replay::valid_name("../x") && !replay::valid_name("a b"));

    replay::Meta m;
    m.width = 16;
    m.height = 1;
    m.stride = 16;
    m.slot_bytes = 16;
    m.pixel = "u8x16";
    m.source = "src";
    std::string err;
    {
        replay::Writer w;
        CHECK(w.open(root, "t1", m, &err));
        for (uint64_t i = 0; i < 50; ++i) {
            uint8_t d[16];
            for (int k = 0; k < 16; ++k) d[k] = static_cast<uint8_t>(i * 3 + k);
            CHECK(w.write(100 + i, 1000000000ll + static_cast<int64_t>(i) * 20000000, d, 10 + i % 7));  // 크기가 바뀌는 레코드
        }
        CHECK(w.close(&err));
        CHECK(w.meta().frames == 50);
        replay::Writer dup;
        CHECK(!dup.open(root, "t1", m, &err));  // 같은 이름은 실패
    }
    replay::Reader r;
    CHECK(r.open(root, "t1", &err));
    CHECK(r.meta().frames == 50 && r.meta().pixel == "u8x16" && r.meta().source == "src");
    CHECK(r.meta().duration_ns == 49ll * 20000000);
    replay::Record rec;
    for (uint64_t i = 0; i < 50; ++i) {
        CHECK(r.next(&rec));
        CHECK(rec.frame_id == 100 + i);
        CHECK(rec.ts_ns == 1000000000ll + static_cast<int64_t>(i) * 20000000);
        CHECK(rec.data.size() == 10 + i % 7);
        CHECK(!rec.data.empty() && rec.data[0] == static_cast<uint8_t>(i * 3));
    }
    CHECK(!r.next(&rec));  // 끝
    r.rewind();
    CHECK(r.next(&rec) && rec.frame_id == 100);
    r.close();

    // 잘린 파일: 앞부분은 읽히고 잘린 레코드에서 멈춘다
    {
        const std::string fb = root + "/t1/frames.bin";
        std::FILE* f = std::fopen(fb.c_str(), "rb+");
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fclose(f);
        CHECK(::truncate(fb.c_str(), size - 5) == 0);
        replay::Reader t;
        CHECK(t.open(root, "t1", &err));
        int n = 0;
        while (t.next(&rec)) ++n;
        CHECK(n == 49);
    }

    const auto l = replay::list(root);
    CHECK(l.size() == 1 && l[0].name == "t1" && l[0].meta.frames == 50 && l[0].file_bytes > 0);
    CHECK(replay::total_bytes(root) == l[0].file_bytes);
    CHECK(replay::remove(root, "t1", &err));
    CHECK(replay::list(root).empty());
    CHECK(!replay::remove(root, "../etc", &err));

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all recording unit tests passed\n");
    return 0;
}
