// libworker 단위 시험: BoundedQueue, ThreadWatch, WorkerThread(스레드 이름), Publisher, Host·Module
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

#include "worker/host.hpp"
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

int main() {
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
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all libworker tests passed\n");
    return 0;
}
