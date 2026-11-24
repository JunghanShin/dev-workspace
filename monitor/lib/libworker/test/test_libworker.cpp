// libworker 단위 시험: BoundedQueue, ThreadWatch, WorkerThread(스레드 이름·우선순위), Publisher, Host·Module,
// 입력·출력 단계, Mutex, 기본 안전망(설정·상태), 명령 소켓(나중에 답하는 명령, 연결 시간 제한)
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

#include "worker/command_server.hpp"
#include "worker/guard.hpp"
#include "worker/inputs.hpp"
#include "worker/manager_link.hpp"
#include "worker/workspace.hpp"
#include "worker/host.hpp"
#include "worker/mutex.hpp"
#include "worker/stage.hpp"
#include "worker/peer.hpp"
#include "worker/publisher.hpp"
#include "worker/queue.hpp"
#include "worker/thread.hpp"

using namespace std::chrono_literals;
using worker::BoundedQueue;
using worker::Overflow;
using worker::ThreadWatch;
using worker::WorkerThread;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static bool readable(int fd, int timeout_ms) {
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN);
}

static void queue_drop_oldest() {
    BoundedQueue<int> q(3, Overflow::DropOldest);
    for (int i = 1; i <= 5; ++i) CHECK(q.push(i));
    auto s = q.stats();
    CHECK(s.size == 3);
    CHECK(s.dropped == 2);
    CHECK(s.high_water == 3);
    CHECK(q.try_pop().value() == 3);  // 1, 2는 버려짐
    CHECK(q.try_pop().value() == 4);
    CHECK(q.try_pop().value() == 5);
    CHECK(!q.try_pop());
}

static void queue_reject() {
    BoundedQueue<int> q(2, Overflow::Reject);
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK(!q.push(3));  // 잃으면 안 되는 것: 넣지 않고 실패를 알림
    CHECK(q.stats().dropped == 1);
    CHECK(q.try_pop().value() == 1);
    CHECK(q.push(3));
}

static void queue_wake_fd() {
    BoundedQueue<int> q(8, Overflow::Reject);
    CHECK(!readable(q.wake_fd(), 0));
    q.push(7);
    q.push(8);
    CHECK(readable(q.wake_fd(), 0));  // 비어 있지 않으면 poll이 깨어남
    std::vector<int> out;
    CHECK(q.drain(out) == 2);
    CHECK(out.size() == 2 && out[0] == 7 && out[1] == 8);
    CHECK(!readable(q.wake_fd(), 0));  // 다 꺼내면 다시 조용함
    q.push(9);
    CHECK(q.try_pop().value() == 9);
    CHECK(!readable(q.wake_fd(), 0));
}

static void queue_partial_drain() {
    BoundedQueue<int> q(8, Overflow::Reject);
    for (int i = 0; i < 5; ++i) q.push(i);
    std::vector<int> out;
    CHECK(q.drain(out, 2) == 2);
    CHECK(readable(q.wake_fd(), 0));  // 남은 것이 있으면 계속 깨어 있음
    CHECK(q.drain(out) == 3);
    CHECK(!readable(q.wake_fd(), 0));
}

static void queue_close() {
    BoundedQueue<int> q(4, Overflow::Reject);
    q.push(1);
    q.close();
    CHECK(!q.push(2));
    CHECK(q.pop_for(10ms).value() == 1);  // 남은 것은 꺼낼 수 있음
    auto t0 = std::chrono::steady_clock::now();
    CHECK(!q.pop_for(1000ms));            // 닫히고 비었으면 기다리지 않음
    CHECK(std::chrono::steady_clock::now() - t0 < 200ms);
    CHECK(readable(q.wake_fd(), 0));      // 닫히면 계속 깨어 있음(소비자가 정지를 알아챔)
}

static void queue_threads() {
    // 생산자 2 + 소비자 1, 순서·개수 확인(생산자별 순서 유지)
    BoundedQueue<int> q(64, Overflow::Reject);
    constexpr int kEach = 20000;
    std::atomic<int> rejected{0};
    auto producer = [&](int base) {
        for (int i = 0; i < kEach; ++i) {
            while (!q.push(base + i)) {
                ++rejected;
                std::this_thread::yield();
            }
        }
    };
    std::thread a(producer, 0), b(producer, 1000000);
    int got = 0, last_a = -1, last_b = 999999;
    bool ordered = true;
    while (got < 2 * kEach) {
        auto v = q.pop_for(1000ms);
        if (!v) break;
        if (*v < 1000000) { ordered &= *v > last_a; last_a = *v; }
        else { ordered &= *v > last_b; last_b = *v; }
        ++got;
    }
    a.join();
    b.join();
    CHECK(got == 2 * kEach);
    CHECK(ordered);
}

static void watch_and_threads() {
    ThreadWatch watch(150ms);
    std::atomic<bool> stall{false};
    WorkerThread healthy("healthy", watch, [](WorkerThread& t) {
        while (!t.stopping()) {
            std::this_thread::sleep_for(10ms);
            t.beat();
        }
    });
    WorkerThread stuck("stuck", watch, [&](WorkerThread& t) {
        while (!t.stopping()) {
            if (!stall.load()) t.beat();
            std::this_thread::sleep_for(10ms);
        }
    });
    WorkerThread boom("boom", watch, [](WorkerThread&) { throw std::runtime_error("device gone"); });
    healthy.start();
    stuck.start();
    boom.start();
    std::this_thread::sleep_for(60ms);
    std::string stalled;
    CHECK(watch.all_alive(ThreadWatch::Clock::now(), &stalled));
    CHECK(boom.failed() && boom.error() == "device gone" && !boom.running());

    stall.store(true);  // stuck이 beat를 멈춤
    std::this_thread::sleep_for(250ms);
    stalled.clear();
    CHECK(!watch.all_alive(ThreadWatch::Clock::now(), &stalled));
    CHECK(stalled == "stuck");
    auto ages = watch.ages_ms(ThreadWatch::Clock::now());
    CHECK(ages.contains("healthy") && ages["healthy"].get<int64_t>() < 100);
    CHECK(ages.contains("stuck") && ages["stuck"].get<int64_t>() >= 150);
    CHECK(!ages.contains("boom"));  // 실패로 끝난 스레드는 멈춤 검사에서 빠짐(Core가 failed()로 판단)

    healthy.request_stop();
    healthy.join();
    stuck.request_stop();
    stuck.join();
    CHECK(watch.all_alive(ThreadWatch::Clock::now()));  // 정상 종료한 스레드는 멈춤으로 보지 않음
}

static int connect_unix(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// 줄 하나 받기(시간 제한)
static std::string read_line(int fd, std::string& buf, int timeout_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const size_t nl = buf.find('\n');
        if (nl != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            return line;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        if (left.count() <= 0 || !readable(fd, static_cast<int>(left.count()))) return "";
        char tmp[4096];
        const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return "";
        buf.append(tmp, static_cast<size_t>(n));
    }
}

static bool wait_until(const std::function<bool()>& f, int timeout_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return f();
}

static void publisher_fanout_and_slow() {
    ThreadWatch watch(2000ms);
    worker::Publisher::Options opt;
    opt.max_backlog = 64 * 1024;
    worker::Publisher pub(watch, opt);
    const std::string path = "/tmp/libworker_test_pub_" + std::to_string(::getpid()) + ".sock";
    std::string err;
    CHECK(pub.open(path, &err));

    pub.publish("{\"lost\":1}");  // 구독자가 없으면 버린다
    CHECK(!pub.keyframe_requested());

    const int a = connect_unix(path);
    const int b = connect_unix(path);  // 읽지 않는 느린 구독자
    CHECK(a >= 0 && b >= 0);
    CHECK(wait_until([&] { return pub.subscribers() == 2; }, 1000));
    CHECK(pub.keyframe_requested());
    CHECK(!pub.keyframe_requested());  // 한 번만

    pub.publish("{\"n\":1}");
    pub.publish("{\"n\":2}");
    std::string buf;
    CHECK(read_line(a, buf, 1000) == "{\"n\":1}");
    CHECK(read_line(a, buf, 1000) == "{\"n\":2}");

    // b는 읽지 않는다 → 소켓 버퍼 + backlog를 넘기면 끊긴다. a는 계속 받는다
    const std::string big(1000, 'x');
    for (int i = 0; i < 2000; ++i) {
        pub.publish("{\"pad\":\"" + big + "\"}");
        if (i % 50 == 0) {
            // a는 따라 읽는다
            while (readable(a, 0)) {
                char tmp[65536];
                if (::recv(a, tmp, sizeof tmp, 0) <= 0) break;
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    CHECK(wait_until([&] { return pub.dropped_subscribers() == 1; }, 2000));
    CHECK(wait_until([&] { return pub.subscribers() == 1; }, 1000));

    ::close(a);
    CHECK(wait_until([&] { return pub.subscribers() == 0; }, 1000));
    ::close(b);
    pub.close();
    CHECK(::access(path.c_str(), F_OK) != 0);  // 닫으면 소켓 파일 정리
}


// ---- 호스트·모듈 ----------------------------------------------------------------------------

namespace {
struct Echo : worker::Module {
    std::string n;
    int calls = 0;
    explicit Echo(std::string name) : n(std::move(name)) {}
    std::string name() const override { return n; }
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override {
        if (cmd != "echo") return false;
        ++calls;
        reply(worker::ok_reply({{"module", n}, {"args", args}}));
        return true;
    }
    worker::Json status() const override { return {{"calls", calls}}; }
    void metrics(worker::Metrics& m) const override { m.set("calls", calls); m.set(n + "_link", 1); m.set("fps", 29.96, 1); }
};
}  // namespace

static void host_dispatch() {
    worker::Host host("test");
    auto& a = host.add<Echo>("alpha");
    host.add<Echo>("beta");
    host.alias("old_echo", "beta.echo");
    worker::Json r;
    auto call = [&](const std::string& line) { host.dispatch(line, [&r](const worker::Json& j) { r = j; }); return r; };
    CHECK(call(R"({"cmd":"alpha.echo","args":{"x":1}})")["data"]["module"] == "alpha");
    CHECK(call(R"({"cmd":"old_echo"})")["data"]["module"] == "beta");
    CHECK(call(R"({"cmd":"echo"})")["ok"] == false);            // 모듈이 둘이면 앞부분이 있어야 함
    CHECK(call(R"({"cmd":"alpha.nope"})")["error"]["code"] == "bad_request");
    CHECK(call(R"({"cmd":"gamma.echo"})")["ok"] == false);
    CHECK(call("not json")["ok"] == false);
    const auto st = call(R"({"cmd":"get_status"})");
    CHECK(st["ok"] == true && st["data"]["alpha"]["calls"] == 1 && st["data"]["beta"]["calls"] == 1);
    CHECK(a.calls == 1);
    // 지표: 모듈 이름을 앞에 붙이고, 이미 붙어 있으면 그대로
    const std::string m = host.metrics();
    CHECK(m.find("alpha_calls=1") != std::string::npos);
    CHECK(m.find("alpha_link=1") != std::string::npos && m.find("alpha_alpha_link") == std::string::npos);
    CHECK(m.find("beta_fps=30.0") != std::string::npos);

    worker::Host one("test1");
    one.add<Echo>("solo");
    one.dispatch(R"({"cmd":"echo"})", [&r](const worker::Json& j) { r = j; });
    CHECK(r["data"]["module"] == "solo");  // 모듈이 하나면 앞부분 없이
    CHECK(worker::call_module(a, "echo")["ok"] == true);
    // 모듈은 붙은 호스트를 안다(configure에서 워커 id 등)
    struct Peek : Echo {
        Peek() : Echo("peek") {}
        worker::Host* h() const { return host(); }
    };
    auto& pk = host.add<Peek>();
    CHECK(pk.h() == &host);
    CHECK(Peek().h() == nullptr);  // 호스트 없이 만든 모듈
    CHECK(worker::default_socket("w", "frames").find("w") != std::string::npos);
}

static void host_legacy_config() {
    struct Cfg : worker::Module {
        worker::Json got;
        std::string name() const override { return "fc"; }
        void configure(const worker::Json& c) override { got = c; }
    };
    char tmpl[] = "/tmp/hostcfgXXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string path = dir + "/w.json";
    std::FILE* f = std::fopen(path.c_str(), "w");
    std::fputs(R"({"id":"t","config":{"listen":"a:1","fc":{"sysid":2,"listen":"b:2"},"watchdog_sec":5}})", f);
    std::fclose(f);
    worker::Host host("test");
    auto& m = host.add<Cfg>();
    host.legacy_keys("fc", {"listen", "sysid"});
    std::string err;
    CHECK(host.configure_file(path, &err));
    CHECK(m.got["listen"] == "b:2" && m.got["sysid"] == 2);  // 칸에 있는 값이 이김, 맨 위 listen은 옮겨져 사라짐
    CHECK(host.watchdog_sec() == 5);
    worker::Host strict("test");
    strict.add<Cfg>();
    CHECK(!strict.configure_file(path, &err) && err.find("listen") != std::string::npos);  // 예전 키 등록 없으면 오류
    std::remove(path.c_str());
    ::rmdir(dir.c_str());
}

// 가짜 장치 모드(설정 맨 위 "sim"): 읽기, @EXEC_DIR@ 바꾸기, 상태 표시, 잘못된 형식은 설정 오류
static void host_sim_config() {
    char tmpl[] = "/tmp/hostsimXXXXXX";
    const std::string dir = mkdtemp(tmpl);
    auto write = [&](const char* text) {
        const std::string path = dir + "/w.json";
        std::FILE* f = std::fopen(path.c_str(), "w");
        std::fputs(text, f);
        std::fclose(f);
        return path;
    };
    std::string err;
    worker::Host plain("test");
    CHECK(plain.configure_file(write(R"({"id":"t","config":{}})"), &err) && !plain.sim());
    CHECK(!plain.status().contains("sim"));
    worker::Host host("test");
    CHECK(host.configure_file(write(R"({"id":"t","config":{"sim":{"cmd":"@EXEC_DIR@/fake-dev --port 1"}}})"), &err));
    CHECK(host.sim());
    const worker::Json s = host.status();
    CHECK(s.contains("sim") && s["sim"]["cmd"].size() == 3);
    CHECK(s["sim"]["cmd"][0] == worker::exec_dir() + "/fake-dev" && s["sim"]["cmd"][2] == "1");
    CHECK(worker::expand_exec_dir("a@EXEC_DIR@b") == "a" + worker::exec_dir() + "b");
    worker::Host bymark("test");
    CHECK(bymark.configure_file(write(R"({"id":"t","config":{"sim":{}}})"), &err) && bymark.sim());  // 설정으로만 가짜 장치
    worker::Host bad("test");
    CHECK(!bad.configure_file(write(R"({"id":"t","config":{"sim":{"cmd":5}}})"), &err));
    CHECK(!bad.configure_file(write(R"({"id":"t","config":{"sim":{"x":1}}})"), &err));
    std::remove((dir + "/w.json").c_str());
    ::rmdir(dir.c_str());
}

static void thread_names() {
    ThreadWatch watch(1000ms);
    std::string seen;
    WorkerThread t("mod.role-long-name", watch, [&](WorkerThread& self) {
        char buf[32] = {};
        pthread_getname_np(pthread_self(), buf, sizeof buf);
        seen = buf;
        while (!self.stopping()) { self.beat(); std::this_thread::sleep_for(5ms); }
    });
    t.start();
    std::this_thread::sleep_for(30ms);
    t.request_stop();
    t.join();
    CHECK(seen == "mod.role-long-n");  // 운영체제 한도 15자
}
// 워커 간: 상대 발행을 구독(끊기면 down, 다시 붙음), 없는 상대에게 비동기 명령은 connect 오류로 응답
static void peer_subscriber_and_command() {
    ThreadWatch watch(2000ms);
    const std::string path = "/tmp/libworker_test_peer_" + std::to_string(::getpid()) + ".sock";
    worker::Subscriber::Options so;
    so.socket = path;
    so.retry_sec = 0.2;
    so.thread_name = "t.sub";
    worker::Subscriber sub(watch, "peer", so);
    sub.start();
    CHECK(!sub.connected());  // 상대가 아직 없다: 기다린다
    {
        worker::Publisher pub(watch);
        std::string err;
        CHECK(pub.open(path, &err));
        CHECK(wait_until([&] { return sub.connected() && pub.subscribers() == 1; }, 3000));
        pub.publish("{\"seq\":1,\"kf\":true,\"topics\":{\"a\":{\"v\":1}},\"events\":[]}");
        std::optional<worker::Subscriber::Update> u;
        CHECK(wait_until([&] { u = sub.next(); return u.has_value(); }, 2000));
        CHECK(u && u->kf && u->topics["a"]["v"] == 1);
        pub.close();
    }
    CHECK(wait_until([&] { return !sub.connected(); }, 3000));
    std::optional<worker::Subscriber::Update> d;
    CHECK(wait_until([&] { d = sub.next(); return d && d->down; }, 2000));
    sub.stop();

    worker::CommandClient::Options co;
    co.socket = path + ".none";
    co.thread_name = "t.cmd";
    worker::CommandClient cmd(watch, "peer", co);
    cmd.start();
    const uint64_t id = cmd.send("x.ping");
    CHECK(id == 1);
    std::optional<worker::CommandClient::Reply> r;
    CHECK(wait_until([&] { r = cmd.next(); return r.has_value(); }, 3000));
    CHECK(r && r->id == 1 && r->response["error"]["code"] == "connect" && !cmd.reachable());
    cmd.stop();
}


static void queue_drop_newest() {
    BoundedQueue<int> q(2, Overflow::DropNewest);
    CHECK(q.push(1) && q.push(2));
    CHECK(q.push(3));  // 넣은 쪽은 성공으로 보고 새것만 버림
    CHECK(q.try_pop() == 1 && q.try_pop() == 2 && !q.try_pop());
    CHECK(q.stats().dropped == 1);
}

// 등록과 beat가 동시에 일어나도 안전(스레드를 실행 중에 더 만드는 워커). TSan 빌드에서 경합이 없어야 한다
static void watch_dynamic_registration() {
    ThreadWatch watch(1000ms);
    std::vector<std::unique_ptr<WorkerThread>> ts;
    for (int i = 0; i < 20; ++i) {
        ts.push_back(std::make_unique<WorkerThread>("t" + std::to_string(i), watch, [](WorkerThread& t) {
            while (!t.stopping()) {
                t.beat();
                std::this_thread::sleep_for(1ms);
            }
        }));
        ts.back()->start();
    }
    CHECK(watch.all_alive(ThreadWatch::Clock::now()));
    // 할 일을 다 하고 끝난 스레드는 멈춤으로 보지 않는다
    WorkerThread once("once", watch, [](WorkerThread&) {});
    once.start();
    once.join();
    std::this_thread::sleep_for(20ms);
    CHECK(!watch.ages_ms(ThreadWatch::Clock::now()).contains("once"));
    for (auto& t : ts) t->request_stop();
    ts.clear();
}

static void thread_options() {
    ThreadWatch watch(1000ms);
    worker::ThreadOptions o;
    o.nice = 5;          // 올리는 것(낮은 우선순위)은 권한 없이 됨
    o.cpus = {0};
    std::atomic<int> got_cpu{-1};
    WorkerThread t("opt", watch, [&](WorkerThread& th) {
        got_cpu.store(sched_getcpu());
        while (!th.stopping()) std::this_thread::sleep_for(5ms);
    });
    t.set_options(o);
    t.start();
    CHECK(wait_until([&] { return got_cpu.load() >= 0; }, 2000));
    std::this_thread::sleep_for(20ms);
    auto info = t.info();
    CHECK(info["requested"]["nice"] == 5 && info["requested"]["cpu"][0] == 0);
    CHECK(info["applied"] == true && got_cpu.load() == 0);
    t.request_stop();
    t.join();
    // 실시간: 권한이 없으면(보통 계정) 적용 실패로 보고하고 스레드는 계속 돈다
    worker::ThreadOptions rt;
    rt.priority = "realtime";
    rt.rt_priority = 10;
    std::atomic<bool> ran{false};
    WorkerThread r("rt", watch, [&](WorkerThread& th) {
        ran.store(true);
        while (!th.stopping()) std::this_thread::sleep_for(5ms);
    });
    r.set_options(rt);
    r.start();
    CHECK(wait_until([&] { return ran.load(); }, 2000));
    std::this_thread::sleep_for(20ms);
    auto ri = r.info();
    CHECK(ri["requested"]["priority"] == "realtime" && !r.failed());
    CHECK(ri["applied"] == true || ri.contains("error"));
    r.request_stop();
    r.join();
}

static void mutex_pi() {
    worker::Mutex m;
    int counter = 0;
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&] {
            for (int k = 0; k < 1000; ++k) {
                std::lock_guard<worker::Mutex> lock(m);
                ++counter;
            }
        });
    for (auto& t : ts) t.join();
    CHECK(counter == 4000);
    CHECK(m.try_lock());
    m.unlock();
}

static void stage_options_parse() {
    worker::StageOptions base;
    auto o = worker::parse_stage_options(worker::Json::parse(R"({"queue":8,"overflow":"reject","wait_ms":20,"priority":"realtime","rt_priority":40,"cpu":[1,2]})"),
                                         "m.rx", base);
    CHECK(o.queue == 8 && o.overflow == Overflow::Reject && o.wait_ms == 20 && o.thread.priority == "realtime" &&
          o.thread.rt_priority == 40 && o.thread.cpus.size() == 2);
    bool threw = false;
    try {
        worker::parse_stage_options(worker::Json::parse(R"({"overflow":"sometimes"})"), "m.rx", base);
    } catch (const worker::ConfigError&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        worker::parse_stage_options(worker::Json::parse(R"({"queue_size":8})"), "m.rx", base);
    } catch (const worker::ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

static void input_stage() {
    ThreadWatch watch(1000ms);
    worker::StageOptions o;
    o.queue = 4;
    o.wait_ms = 10;
    std::atomic<int> produced{0};
    std::atomic<bool> burst{false};
    int handled = 0, bad_seen = 0;
    worker::Input<int> in("m.rx", o, watch,
        [&](worker::Input<int>::Context& ctx) {
            if (burst.exchange(false))
                for (int i = 0; i < 10; ++i) ctx.push(100 + i);  // 큐 4개: 오래된 것 버림
            const int n = produced.load();
            if (n < 5) {
                ctx.push(n);
                produced.store(n + 1);
            }
            if (n == 5) {
                produced.store(6);
                throw std::runtime_error("device glitch");  // 세고 계속
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(ctx.wait_ms()));
        },
        [&](int& v) {
            if (v == 3) {
                ++bad_seen;
                throw std::runtime_error("bad item");  // 그 항목만 버림
            }
            ++handled;
        });
    CHECK(wait_until([&] { in.process(worker::Stage::Clock::now() + 50ms); return handled == 4 && bad_seen == 1; }, 3000));
    CHECK(wait_until([&] { return in.status()["errors"].get<uint64_t>() >= 2; }, 2000));  // read 1 + handle 1
    burst.store(true);
    CHECK(wait_until([&] { return in.status()["dropped"].get<uint64_t>() >= 6; }, 2000));
    const std::string m = in.metrics();
    CHECK(m.find("m_rx_q=") != std::string::npos && m.find("m_rx_drops=") != std::string::npos &&
          m.find("m_rx_lag_ms=") != std::string::npos && m.find("m_rx_errors=") != std::string::npos);
    CHECK(watch.all_alive(ThreadWatch::Clock::now()));
    in.stop();
    std::string why;
    CHECK(!in.failed(&why));
    // 주입된 멈춤: beat가 끊겨 멈춤으로 보이고, 풀리면 다시 살아남
    ThreadWatch fast(100ms);
    worker::Input<int> p("m.p", o, fast, [](worker::Input<int>::Context& ctx) { std::this_thread::sleep_for(std::chrono::milliseconds(ctx.wait_ms())); },
                         [](int&) {});
    p.fault_pause(400);
    CHECK(wait_until([&] { return !fast.all_alive(ThreadWatch::Clock::now()); }, 1000));
    CHECK(wait_until([&] { return fast.all_alive(ThreadWatch::Clock::now()); }, 2000));
    p.stop();
}

static void input_budget() {
    ThreadWatch watch(1000ms);
    worker::StageOptions o;
    o.queue = 1000;
    std::atomic<bool> done{false};
    worker::Input<int> in("m.in", o, watch,
        [&](worker::Input<int>::Context& ctx) {
            if (!done.exchange(true))
                for (int i = 0; i < 200; ++i) ctx.push(i);
            std::this_thread::sleep_for(5ms);
        },
        [&](int&) { std::this_thread::sleep_for(1ms); });  // 항목마다 1 ms
    CHECK(wait_until([&] { return in.status()["size"].get<size_t>() == 200; }, 2000));
    const auto t0 = worker::Stage::Clock::now();
    const bool more = in.process(t0 + 20ms);  // 예산 20 ms
    const auto spent = worker::Stage::Clock::now() - t0;
    CHECK(more && spent < 60ms);  // 예산에서 멈추고 남은 것을 알림
    in.stop();
}

static void output_stage() {
    ThreadWatch watch(1000ms);
    std::vector<int> written;
    std::mutex wm;
    std::atomic<bool> block{false};
    worker::Output<int> out("m.tx", worker::Host::output_defaults(), watch, [&](const int& v) {
        while (block.load()) std::this_thread::sleep_for(1ms);
        if (v == 7) throw std::runtime_error("write failed");  // 세고 계속
        std::lock_guard<std::mutex> lock(wm);
        written.push_back(v);
    });
    for (int i = 0; i < 10; ++i) CHECK(out.push(i));
    CHECK(wait_until([&] { std::lock_guard<std::mutex> lock(wm); return written.size() == 9; }, 2000));
    CHECK(out.status()["errors"] == 1 && out.status()["overflow"] == "reject");
    block.store(true);
    std::this_thread::sleep_for(20ms);
    int accepted = 0;
    for (int i = 0; i < 400; ++i) accepted += out.push(i) ? 1 : 0;
    CHECK(accepted <= 257 && out.status()["dropped"].get<uint64_t>() > 0);  // 출력 기본 256, 가득 차면 거절
    block.store(false);
    out.stop();
}

static void host_stage_config_and_runtime() {
    struct Cfg : worker::Module {
        std::string name() const override { return "m"; }
    };
    char tmpl[] = "/tmp/hoststageXXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string path = dir + "/w.json";
    auto write = [&](const char* txt) {
        std::FILE* f = std::fopen(path.c_str(), "w");
        std::fputs(txt, f);
        std::fclose(f);
    };
    std::string err;
    write(R"({"id":"t","config":{"stages":{"m.rx":{"queue":8,"priority":"realtime"}},"loop_budget_ms":20}})");
    worker::Host ok("test");
    ok.add<Cfg>();
    CHECK(ok.configure_file(path, &err));
    write(R"({"id":"t","config":{"stages":{"m.rx":{"overflow":"never"}}}})");
    worker::Host bad("test");
    bad.add<Cfg>();
    CHECK(!bad.configure_file(path, &err) && err.find("overflow") != std::string::npos);
    write(R"({"id":"t","config":{"loop_budget_ms":0}})");
    worker::Host bad2("test");
    bad2.add<Cfg>();
    CHECK(!bad2.configure_file(path, &err) && err.find("loop_budget_ms") != std::string::npos);
    // 상태에 runtime(루프·안전망), 장애 주입 명령은 꺼져 있으면 모르는 명령
    worker::Json r;
    ok.dispatch(R"({"cmd":"get_status"})", [&r](const worker::Json& j) { r = j; });
    CHECK(r["data"]["runtime"]["loop"]["budget_ms"] == 20 && r["data"]["runtime"]["errors"] == 0);
    ok.dispatch(R"({"cmd":"__fault","args":{"kind":"throw"}})", [&r](const worker::Json& j) { r = j; });
    CHECK(r["ok"] == false);
    std::remove(path.c_str());
    ::rmdir(dir.c_str());
}

// 명령 처리 중 예외: 그 명령에 오류로 응답하고, 셈(워커는 계속)
static void host_handle_exception() {
    struct Thrower : worker::Module {
        std::string name() const override { return "x"; }
        bool handle(const std::string& cmd, const worker::Json&, worker::Reply reply) override {
            if (cmd == "boom") throw std::runtime_error("oops");
            reply(worker::ok_reply());
            return true;
        }
    };
    worker::Host host("test");
    host.add<Thrower>();
    worker::Json r;
    host.dispatch(R"({"cmd":"boom"})", [&r](const worker::Json& j) { r = j; });
    CHECK(r["error"]["code"] == "internal");
    host.dispatch(R"({"cmd":"ok"})", [&r](const worker::Json& j) { r = j; });
    CHECK(r["ok"] == true);
    host.dispatch(R"({"cmd":"get_status"})", [&r](const worker::Json& j) { r = j; });
    CHECK(r["data"]["runtime"]["errors"] == 1 && r["data"]["runtime"]["warnings"].size() == 1);
}

// 명령 소켓: 요청을 받은 연결은 2 s가 지나도 끊지 않고 응답을 기다린다(0.9.1). 요청 없는 연결은 2 s에 끊고,
// 받은 뒤 60 s 안에 응답이 없으면 internal 오류로 답한다. 시각은 on_timer에 넘기는 값으로 흉내 낸다(기다리지 않음)
static void command_server_timeouts() {
    const std::string path = "/tmp/test_cmdsrv_" + std::to_string(getpid()) + ".sock";
    std::vector<int> got;  // 요청을 받은 연결(답하지 않고 둔다)
    worker::CommandServer srv([&](int conn, const std::string&) { got.push_back(conn); });
    std::string err;
    CHECK(srv.open(path, &err));
    auto connect_client = [&]() {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
        CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        return fd;
    };
    auto pump = [&]() {  // 서버 쪽 poll 한 바퀴씩 몇 번(accept, 읽기)
        for (int i = 0; i < 5; ++i) {
            std::vector<pollfd> fds;
            srv.add_pollfds(fds);
            ::poll(fds.data(), fds.size(), 20);
            srv.on_poll(fds);
        }
    };
    auto read_line = [](int fd) {
        std::string out;
        char b[512];
        while (readable(fd, 200)) {
            const ssize_t n = ::recv(fd, b, sizeof b, 0);
            if (n <= 0) break;
            out.append(b, static_cast<size_t>(n));
        }
        return out;
    };
    const auto t0 = worker::CommandServer::Clock::now();

    // 1) 요청을 받은 연결: 3 s 뒤에도 살아 있고, 나중에 보낸 응답이 도착한다
    const int a = connect_client();
    CHECK(::send(a, "{\"cmd\":\"slow\"}\n", 15, 0) == 15);
    pump();
    CHECK(got.size() == 1);
    srv.on_timer(t0 + 3s);
    if (!got.empty()) srv.reply(got[0], R"({"ok":true,"data":{"late":true}})");
    CHECK(read_line(a).find("\"late\":true") != std::string::npos);
    ::close(a);

    // 2) 요청 없이 머무는 연결: 2 s 넘으면 끊김(받는 쪽은 아무것도 못 받고 닫힘)
    const int b = connect_client();
    pump();
    srv.on_timer(t0 + 3s);
    CHECK(read_line(b).empty());
    char c1;
    CHECK(::recv(b, &c1, 1, MSG_DONTWAIT) == 0);  // 닫힘
    ::close(b);

    // 3) 요청을 받았는데 모듈이 끝내 답하지 않음: 60 s 넘으면 internal 오류로 답하고 닫음
    const int c = connect_client();
    CHECK(::send(c, "{\"cmd\":\"lost\"}\n", 15, 0) == 15);
    pump();
    CHECK(got.size() == 2);
    srv.on_timer(worker::CommandServer::Clock::now() + 30s);  // 아직
    CHECK(!readable(c, 50));
    srv.on_timer(worker::CommandServer::Clock::now() + 61s);
    CHECK(read_line(c).find("\"internal\"") != std::string::npos);
    if (got.size() == 2) srv.reply(got[1], R"({"ok":true})");  // 이미 닫힌 연결: 아무 일도 없음
    ::close(c);
    srv.close();
}

// ---- 명령하는 곳 검사(워크스페이스 docs/command-path.md 5장 ②) ----

static void write_text(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    std::fputs(text.c_str(), f);
    std::fclose(f);
    std::rename(tmp.c_str(), path.c_str());  // 새 파일: 바뀐 시각이 달라져 다시 읽힘
}

static void guard_table() {
    char dir[] = "/tmp/lw-guard-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    const std::string path = std::string(dir) + "/peers.json";
    const int me = static_cast<int>(getpid());
    const int pg = static_cast<int>(getpgid(0));
    // 단독 실행: 보지 않음
    {
        worker::CommandGuard g("w1", "");
        auto d = g.check(me, "alpha.echo", "screen");
        CHECK(d.run && !d.violation && d.from == "screen" && !d.read);
        CHECK(g.check(me, "get_status", "").read);
    }
    worker::CommandGuard g("w1", path);
    // 표가 아직 없음: 실행하고 기록
    auto d = g.check(me, "alpha.echo", "");
    CHECK(d.run && d.violation && d.reason == "no peers table yet");
    // 이 프로세스 = 게이트웨이(믿는 곳): 적힌 from을 씀, "*"
    write_text(path, R"({"guard": "warn", "peers": {"gateway": {"pid": )" + std::to_string(me) +
                         R"(, "trusted_from": true}}, "workers": {"w1": {"read": ["get_status", "x.get"],
                         "allow": {"gateway": "*", "ext": ["alpha.echo"]}}}})");
    d = g.check(me, "beta.echo", "remote:ops-ui");
    CHECK(d.run && !d.violation && d.from == "remote:ops-ui");
    CHECK(g.check(me, "beta.echo", "").from == "gateway");
    // 이 프로세스 그룹 = 연동 워커 ext(믿지 않는 곳): 적힌 from은 무시, 역할 명령만
    write_text(path, R"({"guard": "warn", "peers": {"gateway": {"pid": 1}, "ext": {"pgid": )" + std::to_string(pg) +
                         R"(}}, "workers": {"w1": {"read": ["get_status", "x.get"], "allow": {"gateway": "*", "ext": ["alpha.echo"]}}}})");
    d = g.check(me, "alpha.echo", "screen");
    CHECK(d.run && !d.violation && d.from == "ext");
    d = g.check(me, "beta.echo", "screen");
    CHECK(d.run && d.violation && d.from == "ext" && g.mode() == "warn");  // warn: 실행하고 기록
    d = g.check(me, "x.get", "");
    CHECK(d.run && d.read && !d.violation);  // 읽기는 늘
    write_text(path, R"({"guard": "enforce", "peers": {"ext": {"pgid": )" + std::to_string(pg) +
                         R"(}}, "workers": {"w1": {"read": ["get_status"], "allow": {"gateway": "*", "ext": ["alpha.echo"]}}}})");
    CHECK(!g.check(me, "beta.echo", "").run);
    CHECK(g.check(me, "alpha.echo", "").run);
    // 모르는 프로세스: enforce면 거절
    write_text(path, R"({"guard": "enforce", "peers": {}, "workers": {"w1": {"allow": {"gateway": "*"}}}})");
    d = g.check(me, "alpha.echo", "screen");
    CHECK(!d.run && d.violation && d.from == "unknown:" + std::to_string(me));
    CHECK(g.check(me, "get_status", "").run);
    // off: 보지 않음(읽기 표만)
    write_text(path, R"({"guard": "off", "peers": {}, "workers": {}})");
    CHECK(g.check(me, "alpha.echo", "").run && !g.check(me, "alpha.echo", "").violation);
    // 깨진 파일: 직전 표(off) 그대로
    write_text(path, "{broken");
    CHECK(g.mode() == "off" || g.check(me, "alpha.echo", "").run);
    std::remove(path.c_str());
    rmdir(dir);
}

static void host_guard_and_last_command() {
    char dir[] = "/tmp/lw-hostguard-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    const std::string cfg = std::string(dir) + "/w1.json", peers = std::string(dir) + "/peers.json";
    write_text(cfg, R"({"id": "w1", "config_version": 1, "config": {}})");
    worker::Host host("test");
    host.add<Echo>("alpha");
    host.add<Echo>("beta");
    std::string err;
    CHECK(host.configure_file(cfg, &err));
    worker::Json r;
    auto call = [&](const std::string& line, int pid) { host.dispatch(line, [&r](const worker::Json& j) { r = j; }, pid); return r; };
    // 검사 없음(단독): 마지막 명령에 보낸 곳·결과
    CHECK(call(R"({"cmd":"alpha.echo","from":"screen"})", 0)["ok"] == true);
    auto st = call(R"({"cmd":"get_status"})", 0)["data"];
    CHECK(st["last_command"]["cmd"] == "alpha.echo" && st["last_command"]["from"] == "screen" && st["last_command"]["ok"] == true);
    CHECK(st["last_command"]["t_mono_ns"].is_number_integer());
    call(R"({"cmd":"alpha.nope"})", 0);
    st = call(R"({"cmd":"get_status"})", 0)["data"];
    CHECK(st["last_command"]["cmd"] == "alpha.nope" && st["last_command"]["ok"] == false && st["last_command"]["from"].is_null());
    // enforce: 허용 목록 밖은 forbidden, 마지막 명령에 남지 않음
    write_text(peers, R"({"guard": "enforce", "peers": {}, "workers": {"w1": {"read": ["get_status"], "allow": {"gateway": "*"}}}})");
    host.set_peers_file(peers);
    r = call(R"({"cmd":"beta.echo"})", static_cast<int>(getpid()));
    CHECK(r["ok"] == false && r["error"]["code"] == "forbidden");
    st = call(R"({"cmd":"get_status"})", static_cast<int>(getpid()))["data"];
    CHECK(st["last_command"]["cmd"] == "alpha.nope");
    // origin(0.14): 믿는 곳(명령 입구 매니저)이면 from은 그 곳, origin은 봉투 것. 믿지 않는 곳의 origin은 버림
    const std::string me = std::to_string(getpid());
    write_text(peers, R"({"guard": "enforce", "peers": {"mission-manager": {"pid": )" + me + R"(, "trusted_from": true}},
                         "workers": {"w1": {"read": ["get_status"], "allow": {"gateway": "*", "mission-manager": "*"}}}})");
    host.set_peers_file(peers);
    CHECK(call(R"({"cmd":"alpha.echo","origin":"screen"})", static_cast<int>(getpid()))["ok"] == true);
    st = call(R"({"cmd":"get_status"})", static_cast<int>(getpid()))["data"];
    CHECK(st["last_command"]["from"] == "mission-manager" && st["last_command"]["origin"] == "screen");
    write_text(peers, R"({"guard": "warn", "peers": {"ext": {"pid": )" + me + R"(}},
                         "workers": {"w1": {"read": ["get_status"], "allow": {"gateway": "*", "ext": "*"}}}})");
    host.set_peers_file(peers);
    call(R"({"cmd":"alpha.echo","origin":"forged","from":"screen"})", static_cast<int>(getpid()));
    st = call(R"({"cmd":"get_status"})", static_cast<int>(getpid()))["data"];
    CHECK(st["last_command"]["from"] == "ext" && !st["last_command"].contains("origin"));
    std::remove(cfg.c_str());
    std::remove(peers.c_str());
    rmdir(dir);
}

// ---- 입력 주소(design.md 6.6.7.3·6.6.7.5) ----

static void inputs_resolve_and_host() {
    // 워커 id → 기본 소켓, 직접 준 경로는 그대로
    auto a = worker::resolve_input("up", {{"worker", "cam-a"}});
    CHECK(a.worker == "cam-a" && a.command == worker::default_command_socket("cam-a") &&
          a.stream == worker::default_stream_socket("cam-a") && a.data.at("frames") == worker::default_socket("cam-a", "frames"));
    auto b = worker::resolve_input("up", {{"command", "/x/c.sock"}, {"data", {{"raw", "/x/r.sock"}}}});
    CHECK(b.worker.empty() && b.command == "/x/c.sock" && b.stream.empty() && b.data.at("raw") == "/x/r.sock" && b.connected());
    CHECK(!worker::resolve_input("up", worker::Json::object()).connected());
    bool threw = false;
    try { worker::resolve_input("up", {{"wroker", "x"}}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { worker::resolve_input("up", {{"worker", "../x"}}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    char dir[] = "/tmp/lw-inputs-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    const std::string cfg = std::string(dir) + "/w1.json", file = std::string(dir) + "/inputs.json";
    write_text(cfg, R"({"id": "w1", "config": {"inputs": {"up": {"worker": "from-config"}, "aux": {"stream": "/s.sock"}}}})");
    write_text(file, R"({"inputs": {"up": {"worker": "from-ws"}}})");
    setenv("WORKSPACE_INPUTS_FILE", file.c_str(), 1);
    worker::Host host("test");
    host.add<Echo>("alpha");
    std::string err;
    CHECK(host.configure_file(cfg, &err));
    unsetenv("WORKSPACE_INPUTS_FILE");
    // 워크스페이스 파일이 설정을 이긴다(같은 입력), 다른 입력은 설정 그대로
    CHECK(host.uses("up") && host.uses("up")->worker == "from-ws" && host.uses("up")->from == "workspace");
    CHECK(host.uses("aux") && host.uses("aux")->stream == "/s.sock" && host.uses("aux")->from == "config");
    CHECK(host.uses("none") == nullptr);
    host.input_link("up", true);
    const auto st = host.status();
    CHECK(st["inputs"]["up"]["link"] == true && st["inputs"]["up"]["worker"] == "from-ws");
    CHECK(st["inputs"]["aux"]["link"] == false && st["inputs"]["aux"]["worker"].is_null());
    // 깨진 워크스페이스 파일: 무시하고 설정만으로
    write_text(file, "{broken");
    setenv("WORKSPACE_INPUTS_FILE", file.c_str(), 1);
    worker::Host h2("test");
    h2.add<Echo>("alpha");
    CHECK(h2.configure_file(cfg, &err));
    unsetenv("WORKSPACE_INPUTS_FILE");
    CHECK(h2.uses("up") && h2.uses("up")->worker == "from-config");
    std::remove(cfg.c_str());
    std::remove(file.c_str());
    rmdir(dir);
}

// 매니저 도구(0.13): 여러 워커 입력 목록 읽기, process-manager 구독(가짜 소켓)
static void manager_link_tools() {
    char tmpl[] = "/tmp/lw-many-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string file = dir + "/in.json";
    auto write = [&](const std::string& text) {
        const std::string tmp = file + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        std::fputs(text.c_str(), f);
        std::fclose(f);
        std::rename(tmp.c_str(), file.c_str());
    };
    worker::ManyInputs many(file, "me");
    CHECK(!many.refresh() && many.workers("streams").empty());  // 파일 없음
    write(R"({"inputs": {}, "many": {"streams": [{"worker": "b"}, {"worker": "me"}, {"worker": "a"}]}})");
    CHECK(many.refresh());
    CHECK((many.workers("streams") == std::vector<std::string>{"a", "b"}));
    CHECK(!many.refresh());  // 그대로
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write(R"({"many": {"streams": [{"worker": "a"}]}})");
    CHECK(many.refresh() && many.workers("streams").size() == 1);

    // 구독: 가짜 process-manager(첫 줄 ok, 그다음 줄들). 끊기면 Down
    const std::string sock = dir + "/pm.sock";
    const int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, sock.c_str(), sizeof(a.sun_path) - 1);
    CHECK(bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && listen(lfd, 2) == 0);
    worker::ThreadWatch watch(std::chrono::milliseconds(3000));
    worker::ManagerSubscription::Options o;
    o.socket = sock;
    o.retry_sec = 0.2;
    worker::ManagerSubscription sub(watch, o);
    sub.start();
    const int c = accept(lfd, nullptr, nullptr);
    char buf[512] = {0};
    CHECK(read(c, buf, sizeof buf - 1) > 0 && std::string(buf).find(R"("subscribe")") != std::string::npos);
    const std::string lines = R"({"ok":true})" "\n" R"({"event":"system","data":{"z":1,"a":2}})" "\n" "broken\n";
    CHECK(::write(c, lines.data(), lines.size()) == static_cast<ssize_t>(lines.size()));
    std::vector<worker::ManagerSubscription::Event> got;
    for (int i = 0; i < 50 && got.size() < 2; ++i) {
        while (auto e = sub.next()) got.push_back(*e);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(got.size() == 2 && got[0].kind == worker::ManagerSubscription::Event::Connected);
    CHECK(got.size() == 2 && got[1].msg.dump() == R"({"event":"system","data":{"z":1,"a":2}})");  // 키 순서 그대로
    CHECK(sub.connected());
    close(c);
    bool down = false;
    for (int i = 0; i < 50 && !down; ++i) {
        while (auto e = sub.next()) down |= e->kind == worker::ManagerSubscription::Event::Down;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(down);
    sub.stop();
    close(lfd);
    std::remove(file.c_str());
    std::remove(sock.c_str());
    rmdir(dir.c_str());
}


// 명령 임대(0.15, docs/command-lease.md): ok인 임대 명령만 기한, 갱신(같은 명령·lease.renew), 기한 지남 → safe_state가 모듈로·이벤트,
// safe_state가 오면 끝, 실패한 명령은 임대 없음, 표시 없는 명령은 그대로
namespace {
struct Mount : worker::Module {
    std::vector<std::string> got;
    std::string name() const override { return "mount"; }
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override {
        got.push_back(cmd);
        if (cmd == "rate" && args.value("fail", false)) {
            reply(worker::error_reply("unavailable", "device busy"));
            return true;
        }
        if (cmd == "rate" || cmd == "stop" || cmd == "angle") {
            reply(worker::ok_reply());
            return true;
        }
        return false;
    }
};
}  // namespace

static void command_lease() {
    char dir[] = "/tmp/lw-lease-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    const std::string cfg = std::string(dir) + "/w.json";
    write_text(cfg, R"({"id": "w1", "config_version": 1, "config": {}})");
    worker::Host host("test");
    auto& m = host.add<Mount>();
    std::string err;
    CHECK(host.configure_file(cfg, &err));
    host.load_leases(worker::Json::parse(R"({"mount": {"name": "mount", "safe_state": {"cmd": "mount.stop", "args": {}},
        "commands": {"mount.rate": {"lease_ms": 200}, "mount.angle": {}, "mount.stop": {}}}})"));
    worker::Json r;
    auto call = [&](const std::string& line) { host.dispatch(line, [&r](const worker::Json& j) { r = j; }); return r; };
    using Clock = std::chrono::steady_clock;
    CHECK(host.next_lease_deadline() == worker::TimePoint::max());
    call(R"({"cmd": "mount.angle"})");  // 표시 없음: 임대 없음
    CHECK(host.status()["leases"].empty());
    call(R"({"cmd": "mount.rate", "args": {"fail": true}})");  // 실패: 임대 없음
    CHECK(host.status()["leases"].empty());
    CHECK(call(R"({"cmd": "mount.rate", "origin": "screen"})")["ok"] == true);
    auto st = host.status();
    CHECK(st["leases"].contains("mount") && st["leases"]["mount"]["cmd"] == "mount.rate" && st["leases"]["mount"]["origin"] == "screen");
    const auto d1 = host.next_lease_deadline();
    CHECK(d1 != worker::TimePoint::max());
    // 기한 안: 아무것도 안 함
    host.lease_tick(Clock::now());
    CHECK(m.got.back() == "rate");
    // 갱신(lease.renew): 기한이 늘어남
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(call(R"({"cmd": "lease.renew", "args": {"role": "mount"}})")["ok"] == true);
    CHECK(host.next_lease_deadline() > d1);
    CHECK(call(R"({"cmd": "lease.renew", "args": {"role": "nope"}})")["error"]["code"] == "bad_request");
    // 기한 지남 → safe_state가 모듈로, 이벤트, 임대 끝, last_command from lease
    host.lease_tick(Clock::now() + std::chrono::milliseconds(300));
    CHECK(m.got.back() == "stop");
    CHECK(host.status()["leases"].empty());
    CHECK(host.status()["last_command"]["from"] == "lease" && host.status()["last_command"]["ok"] == true);
    auto ev = host.take_events();
    CHECK(ev.size() == 1 && ev[0]["code"] == "lease_expired" && ev[0]["detail"]["role"] == "mount" && ev[0]["detail"]["origin"] == "screen");
    CHECK(ev.size() == 1 && ev[0]["source"] == "w1.host" && ev[0]["severity"] == "warning");
    CHECK(host.take_events().empty());
    // safe_state 명령이 오면(사람이 멈춤) 임대 끝, 그 뒤 기한이 지나도 다시 멈추지 않음
    call(R"({"cmd": "mount.rate"})");
    CHECK(!host.status()["leases"].empty());
    call(R"({"cmd": "mount.stop"})");
    CHECK(host.status()["leases"].empty());
    const size_t n = m.got.size();
    host.lease_tick(Clock::now() + std::chrono::seconds(1));
    CHECK(m.got.size() == n && host.take_events().empty());
    std::remove(cfg.c_str());
    rmdir(dir);
}

int main() {
    command_lease();
    manager_link_tools();
    inputs_resolve_and_host();
    guard_table();
    host_guard_and_last_command();
    queue_drop_oldest();
    queue_reject();
    queue_wake_fd();
    queue_partial_drain();
    queue_close();
    queue_threads();
    watch_and_threads();
    publisher_fanout_and_slow();
    host_dispatch();
    host_legacy_config();
    host_sim_config();
    thread_names();
    peer_subscriber_and_command();
    queue_drop_newest();
    watch_dynamic_registration();
    thread_options();
    mutex_pi();
    stage_options_parse();
    input_stage();
    input_budget();
    output_stage();
    host_stage_config_and_runtime();
    host_handle_exception();
    command_server_timeouts();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all libworker tests passed\n");
    return 0;
}
