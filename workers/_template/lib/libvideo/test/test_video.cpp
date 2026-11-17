// libvideo 시험. 가짜 ffmpeg·gst-launch(셸 스크립트)로 송출 묶음을 띄워 확인한다.
// - 설정 읽기(기본값 위에 덮어쓰기, 검증)
// - relay·encode 명령, 진행(progress)으로 링크 판단, 끊김 뒤 다시 시작
// - 송출 둘의 독립(한쪽이 끝나도 다른 쪽은 그대로), stream_stop/stream_start, autostart false
// - 입력 framebus: 가짜 생산자 → Feeder → gst 표준 입력, 생산자가 없어지면 끊김·돌아오면 다시 송출
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#include "framebus/framebus.hpp"
#include "video/pipeline.hpp"
#include "video/stream.hpp"
#include "worker/config.hpp"

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

std::string slurp(const std::string& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// 가짜 ffmpeg: 인자를 남기고 fd 3에 progress. dir/exit 있으면 종료
std::string fake_ffmpeg(const std::string& dir) {
    mkdir(dir.c_str(), 0755);
    const std::string p = dir + "/ffmpeg";
    std::ofstream(p) << "#!/bin/sh\n"
                        "echo \"$@\" > " << dir << "/ffmpeg.args\n"
                        "n=0\n"
                        "while :; do\n"
                        "  [ -e " << dir << "/exit ] && exit 1\n"
                        "  n=$((n+6))\n"
                        "  printf 'frame=%d\\nfps=30.0\\nprogress=continue\\n' $n >&3\n"
                        "  sleep 0.2\n"
                        "done\n";
    chmod(p.c_str(), 0755);
    return p;
}

// 가짜 gst: 인자를 남기고 stdout에 계속 쓴다. 표준 입력이 파이프면(framebus) 끝까지 읽고, 끝나면(EOF) 종료
std::string fake_gst(const std::string& dir, bool read_stdin) {
    mkdir(dir.c_str(), 0755);
    const std::string p = dir + "/gst";
    std::ofstream f(p);
    f << "#!/bin/sh\necho \"$@\" > " << dir << "/gst.args\n";
    if (read_stdin) {
        f << "exec 3<&0\n( cat <&3 > " << dir << "/in.raw ) &\nr=$!\n"
             "while kill -0 $r 2>/dev/null; do printf 'x'; sleep 0.2; done\nexit 1\n";
    } else {
        f << "while :; do printf 'x'; sleep 0.2; done\n";
    }
    f.close();
    chmod(p.c_str(), 0755);
    return p;
}

// streams를 poll 루프로 돌리면서 cond가 참이 될 때까지(최대 sec)
bool run_until(video::Streams& s, double sec, const std::function<bool()>& cond) {
    const auto end = Clock::now() + std::chrono::milliseconds(static_cast<long>(sec * 1000));
    std::vector<pollfd> fds;
    while (Clock::now() < end) {
        fds.clear();
        s.add_pollfds(fds);
        poll(fds.data(), fds.size(), 50);
        s.on_poll(fds, Clock::now());
        s.on_timer(Clock::now());
        if (cond()) return true;
    }
    return cond();
}

Json find(const Json& status, const std::string& name) {
    for (const auto& x : status) if (x.value("name", "") == name) return x;
    return Json::object();
}

bool config_error(const Json& streams, const video::StreamConfig& d) {
    try {
        video::parse_streams(streams, d);
    } catch (const worker::ConfigError&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/libvideo-test-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    video::StreamConfig d;
    d.source = "rtsp://cam/main.264";
    d.retry_sec = 0.5;
    d.stall_sec = 1;

    // 1. 설정 읽기
    {
        auto v = video::parse_streams(Json::array({{{"name", "main"}, {"url", "rtsp://srv:8554/a"}},
                                                   {{"name", "proc"}, {"url", "rtsp://srv:8554/b"}, {"mode", "encode"},
                                                    {"overlay_time", true}, {"autostart", false}}}),
                                      d);
        CHECK(v.size() == 2 && v[0].source == "rtsp://cam/main.264" && v[0].mode == "relay");
        CHECK(v[1].encode() && v[1].overlay_time && !v[1].autostart);
        CHECK(config_error(Json::array({{{"name", "a"}, {"url", "x"}}, {{"name", "a"}, {"url", "y"}}}), d));  // 이름 중복
        CHECK(config_error(Json::array({{{"url", "x"}, {"nope", 1}}}), d));                               // 모르는 키
        CHECK(config_error(Json::array({{{"url", "x"}, {"input", "framebus"}}}), d));                     // framebus는 encode만
        CHECK(config_error(Json::array({{{"url", "x"}, {"overlay_time", true}}}), d));                    // relay 시각 표시
        CHECK(config_error(Json::array({{{"name", "m"}}}), d));                                          // url 없음
        CHECK(video::parse_streams(nullptr, d).empty());
    }

    // 2. relay + encode 둘: 명령, 링크, 독립, stream_stop/start, autostart false
    {
        video::StreamConfig a = d, b = d, c = d;
        a.name = "main";
        a.url = "rtsp://srv:8554/cam";
        a.transport = "rtsp_tcp";
        a.ffmpeg = fake_ffmpeg(dir + "/a");
        b.name = "proc";
        b.url = "rtsp://srv:8554/cam-proc";
        b.mode = "encode";
        b.overlay_time = true;
        b.ffmpeg = fake_ffmpeg(dir + "/b");
        b.gst_launch = fake_gst(dir + "/b", false);
        c.name = "spare";
        c.url = "rtsp://srv:8554/spare";
        c.autostart = false;
        c.ffmpeg = fake_ffmpeg(dir + "/c");
        video::Streams s;
        s.configure({a, b, c});
        auto st = [&](const char* n) { return find(s.status(), n); };
        CHECK(run_until(s, 8, [&] { return st("main").value("link", "") == "up" && st("proc").value("link", "") == "up"; }));
        CHECK(has(slurp(dir + "/a/ffmpeg.args"), "-rtsp_transport tcp -i rtsp://cam/main.264"));
        CHECK(has(slurp(dir + "/b/gst.args"), "rtspsrc location=rtsp://cam/main.264"));
        CHECK(has(slurp(dir + "/b/gst.args"), "clockoverlay"));
        CHECK(st("spare").value("link", "") == "down" && !st("spare").value("enabled", true));
        CHECK(has(s.metrics(), "stream_main_link=1") && has(s.metrics(), "stream_spare_link=0"));
        // 한쪽(main ffmpeg)이 끝나도 다른 쪽은 그대로, main은 다시 시작
        std::ofstream(dir + "/a/exit") << "";
        CHECK(run_until(s, 4, [&] { return st("main").value("restarts", 0) >= 1; }));
        CHECK(st("proc").value("link", "") == "up" && st("proc").value("restarts", 0) == 0);
        unlink((dir + "/a/exit").c_str());
        CHECK(run_until(s, 6, [&] { return st("main").value("link", "") == "up"; }));
        // 명령: 끄기·켜기
        Json reply;
        CHECK(s.handle("stream_stop", {{"name", "proc"}}, &reply) && reply.value("ok", false));
        CHECK(run_until(s, 2, [&] { return st("proc").value("link", "") == "down" && !st("proc").value("enabled", true); }));
        CHECK(s.handle("stream_start", {{"name", "spare"}}, &reply) && reply.value("ok", false));
        CHECK(run_until(s, 6, [&] { return st("spare").value("link", "") == "up"; }));
        CHECK(s.handle("stream_start", {{"name", "nope"}}, &reply) && !reply.value("ok", true));
        CHECK(!s.handle("get_status", Json::object(), &reply));
        s.stop();
    }

    // 3. 입력 framebus
    {
        const std::string fb = dir + "/frames.sock";
        framebus::Format f;
        f.width = 32;
        f.height = 8;
        f.stride = 32;
        auto producer = std::make_unique<framebus::Producer>(fb, f, 4);
        std::string err;
        CHECK(producer->start(&err));
        std::atomic<framebus::Producer*> prod{producer.get()};
        std::atomic<bool> run{true};
        std::thread pt([&] {
            uint64_t id = 0;
            while (run.load()) {
                if (auto* p = prod.load()) {
                    if (auto sl = p->acquire()) {
                        std::memset(sl->data, static_cast<int>(++id & 0xff), sl->bytes);
                        p->publish(*sl, id, 0);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        video::StreamConfig x = d;
        x.name = "main";
        x.input = "framebus";
        x.source = fb;
        x.mode = "encode";
        x.url = "rtsp://srv:8554/ximea";
        x.ffmpeg = fake_ffmpeg(dir + "/x");
        x.gst_launch = fake_gst(dir + "/x", true);
        video::Streams s;
        s.configure({x});
        auto st = [&] { return find(s.status(), "main"); };
        CHECK(run_until(s, 8, [&] { return st().value("link", "") == "up"; }));
        CHECK(has(slurp(dir + "/x/gst.args"),
                  "fdsrc fd=0 blocksize=256 ! rawvideoparse width=32 height=8 format=gray8 framerate=30/1 ! nvvidconv"));
        run_until(s, 1.5, [] { return false; });
        const auto fed = st()["framebus"].value("fed", 0ul);
        CHECK(fed >= 20 && fed <= 80);  // 약 30 fps로 줄여 보냄
        prod.store(nullptr);
        producer->stop();
        CHECK(run_until(s, 8, [&] { return st().value("link", "") == "down"; }));
        producer = std::make_unique<framebus::Producer>(fb, f, 4);
        CHECK(producer->start(&err));
        prod.store(producer.get());
        CHECK(run_until(s, 10, [&] { return st().value("link", "") == "up"; }));
        s.stop();
        run.store(false);
        pt.join();
    }

    (void)!std::system(("rm -rf " + dir).c_str());
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("libvideo: all tests passed\n");
    return 0;
}
