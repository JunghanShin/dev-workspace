// record 코어 단위 시험: 플랫폼 규칙(예전 workspace-logger와 같은 줄), 기록 파일(이어 쓰기·나누기·지우기), 찾기
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "record/platform.hpp"
#include "record/store.hpp"
#include "record/topics.hpp"

using record::Json;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " #cond "\n";   \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static std::string tmpdir() {
    char tmpl[] = "/tmp/rec-test-XXXXXX";
    return mkdtemp(tmpl);
}

static std::vector<std::string> names(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (std::string(e->d_name).size() > 6 && std::string(e->d_name).find(".jsonl") != std::string::npos)
                out.push_back(e->d_name);
        closedir(d);
    }
    return out;
}

static Json worker_msg(const std::string& id, const std::string& actual, int cpu) {
    return Json::parse(R"({"event":"worker_status","data":{"id":")" + id + R"(","type":"cam","desired_state":"running","actual_state":")" +
                       actual + R"(","pid":10,"metrics":{"cpu":)" + std::to_string(cpu) + "}}}");
}

static void test_platform() {
    record::PlatformRecorder p(1.0);
    // 처음: 바뀐 것으로 남김. type은 빠지고 끝에 worker_type, 키 순서는 type, changed, data...
    auto r = p.handle(worker_msg("cam1", "running", 1), 10.0);
    CHECK(r.size() == 1);
    CHECK(r[0].dump() ==
          R"({"type":"worker","changed":true,"id":"cam1","desired_state":"running","actual_state":"running","pid":10,"metrics":{"cpu":1},"worker_type":"cam"})");
    // 상태 같고 간격 안: 안 남김
    CHECK(p.handle(worker_msg("cam1", "running", 2), 10.5).empty());
    // 간격 지남: changed false로 남김
    r = p.handle(worker_msg("cam1", "running", 3), 11.0);
    CHECK(r.size() == 1 && r[0]["changed"] == false);
    // 상태 바뀜: 간격 안이라도 바로
    r = p.handle(worker_msg("cam1", "stopping", 3), 11.1);
    CHECK(r.size() == 1 && r[0]["changed"] == true);
    // 빠짐
    r = p.handle(Json::parse(R"({"event":"worker_removed","data":{"id":"cam1"}})"), 12);
    CHECK(r.size() == 1 && r[0].dump() == R"({"type":"removed","id":"cam1"})");
    // 명령: 모두
    r = p.handle(Json::parse(R"({"event":"command","data":{"op":"start","id":"x","ok":true}})"), 12);
    CHECK(r.size() == 1 && r[0].dump() == R"({"type":"command","op":"start","id":"x","ok":true})");
    // system: 바뀔 때만
    const Json sys = Json::parse(R"({"event":"system","data":{"clients":{"rest":true}}})");
    CHECK(p.handle(sys, 13).size() == 1);
    CHECK(p.handle(sys, 14).empty());
    // 모르는 event·깨진 모양: 무시
    CHECK(p.handle(Json::parse(R"({"event":"other","data":{}})"), 15).empty());
    CHECK(p.handle(Json::parse(R"([1,2])"), 15).empty());
    // 끊김: 붙어 있지 않으면 안 남김, 붙어 있었으면 하나. 그 뒤 모두 새로
    CHECK(p.disconnected("x").empty());
    CHECK(p.connected().dump() == R"({"type":"logger","event":"connected"})");
    auto d = p.disconnected("gone");
    CHECK(d.size() == 1 && d[0].dump() == R"({"type":"logger","event":"disconnected","reason":"gone"})");
    CHECK(p.handle(sys, 16).size() == 1);  // 잊었으니 다시 남김
}

static void test_line() {
    Json rec = Json::parse(R"({"type":"logger","event":"start","pid":5,"text":"한글"})");
    const std::string line = record::make_line(rec, "2026-11-24T10:00:00.123+09:00", 42);
    CHECK(line == "{\"t\":\"2026-11-24T10:00:00.123+09:00\",\"t_mono_ns\":42,\"type\":\"logger\",\"event\":\"start\",\"pid\":5,\"text\":\"한글\"}\n");
    const std::string iso = record::now_iso();
    CHECK(iso.size() == 29 && iso[10] == 'T' && iso[19] == '.' && (iso[23] == '+' || iso[23] == '-'));
}

static void test_file() {
    const std::string dir = tmpdir();
    {
        record::RecordFile f(dir, 200, 450, 0, "bootaaaa");
        CHECK(names(dir).size() == 1);
        CHECK(f.path().find("_bootaaaa.jsonl") != std::string::npos);
        for (int i = 0; i < 3; ++i) CHECK(f.write(std::string(90, 'a' + i) + "\n"));  // 273 B → 넘어서 새 파일
        CHECK(names(dir).size() == 2);
    }
    // 같은 부팅이면 마지막 파일에 이어 씀
    {
        record::RecordFile f(dir, 200, 450, 0, "bootaaaa");
        CHECK(names(dir).size() == 2);
        // 많이 쓰면 오래된 파일부터 지움(전체 450 B 상한)
        for (int i = 0; i < 12; ++i) {
            CHECK(f.write(std::string(99, 'x') + "\n"));
            sleep(i == 5 ? 1 : 0);  // 파일 이름(초)이 겹치지 않게 한 번
        }
        CHECK(f.removed() > 0);
        uint64_t total = 0;
        for (const auto& n : names(dir)) {
            struct stat st {};
            stat((dir + "/" + n).c_str(), &st);
            total += st.st_size;
        }
        CHECK(total <= 450 + 200);
    }
    // 다른 부팅: 새 파일
    {
        record::RecordFile f(dir, 200, 100000, 0, "bootbbbb");
        CHECK(f.path().find("_bootbbbb.jsonl") != std::string::npos);
    }
    (void)!std::system(("rm -rf " + dir).c_str());
}

static void test_find() {
    const std::string dir = tmpdir();
    {
        std::ofstream a(dir + "/20261124-100000_bootold0.jsonl");
        a << R"({"t":"2026-11-24T10:00:00.000+09:00","type":"worker","id":"cam1"})" "\n";
        a << R"({"t":"2026-11-24T10:00:05.000+09:00","type":"command","id":"cam2"})" "\n";
        std::ofstream b(dir + "/20261124-110000_bootnew0.jsonl");
        b << R"({"t":"2026-11-24T11:00:00.000+09:00","t_mono_ns":100,"type":"event","src":"cam1","topic":"camera"})" "\n";
        b << "not json\n";
        b << R"({"t":"2026-11-24T11:00:01.000+09:00","t_mono_ns":200,"type":"topic","src":"cam2","topic":"camera"})" "\n";
    }
    auto q = [&](const char* text) { return record::find(dir, Json::parse(text), "bootnew0"); };
    CHECK(q("{}")["lines"].size() == 4);
    CHECK(q(R"({"types":["worker","command"]})")["lines"].size() == 2);
    CHECK(q(R"({"src":"cam1"})")["lines"].size() == 2);   // id 또는 src
    CHECK(q(R"({"src":"cam*","topic":"camera"})")["lines"].size() == 2);
    CHECK(q(R"({"since":"2026-11-24T10:00:01"})")["lines"].size() == 3);
    CHECK(q(R"({"until":"2026-11-24T10"})")["lines"].size() == 2);  // 앞부분 포함
    auto m = q(R"({"since_ns":150})");
    CHECK(m["lines"].size() == 1 && m["files"] == 1);  // 이 부팅 파일만
    auto l = q(R"({"limit":3})");
    CHECK(l["lines"].size() == 3 && l["more"] == true);
    CHECK(q(R"({"limit":100000})")["more"] == false);
    (void)!std::system(("rm -rf " + dir).c_str());
}

static void test_topics() {
    record::TopicRules r;  // 기본: 모든 주제 1 s에 하나
    const int64_t s = 1000000000;
    auto t = Json::parse(R"({"camera":{"fps":30,"t_mono_ns":777},"mount":{"yaw":1}})");
    auto ev = Json::parse(R"([{"t_mono_ns":555,"source":"cam1.link","severity":"warning","code":"link_down","text":"x"}])");
    auto out = r.records("cam1", t, ev, false, 10 * s);
    CHECK(out.size() == 3);
    CHECK(out[0].rec.dump() == R"({"type":"topic","src":"cam1","topic":"camera","v":{"fps":30,"t_mono_ns":777}})");
    CHECK(out[0].t_mono_ns == 777);        // 잰 시각
    CHECK(out[1].t_mono_ns == 10 * s);     // 없으면 받은 시각
    CHECK(out[2].rec["type"] == "event" && out[2].t_mono_ns == 555);
    // 간격 안: 주제는 안 남김. 같은 이벤트(kf에 다시 실려 옴)도 안 남김, 새 이벤트는 남김
    out = r.records("cam1", t, ev, false, 10 * s + s / 2);
    CHECK(out.empty());
    auto ev2 = Json::parse(R"([{"t_mono_ns":555,"source":"cam1.link","code":"link_down"},{"t_mono_ns":556,"source":"cam1.link","code":"link_up"},{"code":"no_time"}])");
    out = r.records("cam1", Json(), ev2, false, 10 * s + s / 2);
    CHECK(out.size() == 2 && out[0].rec["v"]["code"] == "link_up" && out[1].rec["v"]["code"] == "no_time");
    r.forget("cam1");  // 끊겨도 이벤트는 잊지 않음
    CHECK(r.records("cam1", Json(), ev, false, 10 * s + s / 2).empty());
    // kf는 늘(표시 kf)
    out = r.records("cam1", t, Json::array(), true, 10 * s + s / 2 + 1);
    CHECK(out.size() == 2 && out[0].rec["kf"] == true);
    // 간격 지남
    CHECK(r.records("cam1", t, Json(), false, 12 * s).size() == 2);
    // 다른 보낸 곳은 따로
    CHECK(r.records("cam2", t, Json(), false, 12 * s).size() == 2);
    // forget: 다시 붙으면 바로
    r.forget("cam1");
    CHECK(r.records("cam1", t, Json(), false, 12 * s + 1).size() == 2);
    // 규칙: 위에서부터 처음 맞는 것, 0이면 모두, exclude
    r.configure(Json::parse(R"([{"match":"*.camera","interval_ms":0},{"match":"cam1.*","interval_ms":2000}])"),
                Json::parse(R"(["*.secret"])"));
    CHECK(r.take("cam1", "camera", 1, false) && r.take("cam1", "camera", 2, false));
    CHECK(r.take("cam1", "mount", s, false) && !r.take("cam1", "mount", 2 * s, false) && r.take("cam1", "mount", 3 * s, false));
    CHECK(!r.take("cam1", "secret", 5 * s, true));
    CHECK(!r.take("cam9", "mount", 5 * s, false));  // 어느 규칙에도 안 맞음
    bool threw = false;
    try {
        r.configure(Json::parse(R"([{"match":1}])"), Json());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    test_topics();
    test_platform();
    test_line();
    test_file();
    test_find();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "record core: all tests passed\n";
    return 0;
}
