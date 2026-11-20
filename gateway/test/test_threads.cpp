// 스레드 시험(TSan으로도 돌림): 가짜 워커 스트림 소켓·가짜 Manager 소켓으로
//   - Hub/Upstream/Subscriber: 기준 상태, 합치기, 위쪽 연결 하나, 워커 재시작(down → up), 화면이 떠나면 정리
//   - SseSession: hz 간격, ping, 쓰기 실패면 끝(구독 해제)
//   - Gateway(HTTP 끝까지): 여러 스레드가 동시에 요청·SSE, 동시 요청 상한 → 503, 상한 안 끝내기
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "httplib/httplib.h"
#include "hub.hpp"
#include "server.hpp"
#include "sys.hpp"

using namespace gw;

namespace {

int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_fail;                                                              \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        }                                                                          \
    } while (0)

bool wait_for(const std::function<bool()>& cond, double timeout_s) {
    const double end = mono_now() + timeout_s;
    while (mono_now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

std::string tmpdir() {
    char tmpl[] = "/tmp/gw-threads-XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    return d ? d : "/tmp";
}

// Unix 소켓 서버(연결마다 스레드, 멈출 때 모두 join)
class FakeServer {
public:
    using Handler = std::function<void(int fd)>;
    FakeServer(std::string path, Handler h) : path_(std::move(path)), h_(std::move(h)) {
        ::unlink(path_.c_str());
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path_.c_str());
        ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(fd_, 16);
        t_ = std::thread([this] {
            while (!stop_.load()) {
                pollfd p{fd_, POLLIN, 0};
                if (::poll(&p, 1, 50) <= 0) continue;
                const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
                if (c < 0) continue;
                std::lock_guard<std::mutex> lock(mu_);
                conns_.push_back(c);
                threads_.emplace_back([this, c] { h_(c); });
            }
        });
    }
    ~FakeServer() { close(); }
    void close() {
        if (stop_.exchange(true)) return;
        t_.join();
        std::vector<std::thread> ts;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (int c : conns_) ::shutdown(c, SHUT_RDWR);
            ts.swap(threads_);
        }
        for (auto& t : ts) t.join();
        for (int c : conns_) ::close(c);
        ::close(fd_);
        ::unlink(path_.c_str());
    }
    bool stopping() const { return stop_.load(); }
    size_t accepted() {
        std::lock_guard<std::mutex> lock(mu_);
        return conns_.size();
    }

private:
    std::string path_;
    Handler h_;
    int fd_;
    std::atomic<bool> stop_{false};
    std::thread t_;
    std::mutex mu_;
    std::vector<int> conns_;
    std::vector<std::thread> threads_;
};

void send_line(int fd, const Json& j) {
    const std::string s = compact(j) + "\n";
    (void)!::send(fd, s.data(), s.size(), MSG_NOSIGNAL);
}

// 가짜 워커: 붙으면 기준 상태, 그 뒤 broadcast()로 변경분
class FakeWorker {
public:
    explicit FakeWorker(const std::string& path)
        : srv_(path, [this](int fd) {
              {
                  std::lock_guard<std::mutex> lock(mu_);
                  send_line(fd, Json{{"seq", 1}, {"kf", true}, {"topics", {{"v", {{"r", 0}}}}},
                                     {"events", Json::array({Json{{"seq", 1}, {"text", "found"}}})}, {"events_next", 2}});
                  fds_.push_back(fd);
              }
              char b[64];
              while (!srv_.stopping()) {  // 구독자가 닫을 때까지
                  pollfd p{fd, POLLIN, 0};
                  if (::poll(&p, 1, 50) > 0 && ::recv(fd, b, sizeof b, 0) <= 0) break;
              }
              std::lock_guard<std::mutex> lock(mu_);
              fds_.erase(std::remove(fds_.begin(), fds_.end(), fd), fds_.end());
          }) {}
    void broadcast(const Json& j) {
        std::lock_guard<std::mutex> lock(mu_);
        for (int fd : fds_) send_line(fd, j);
    }
    size_t open_conns() {
        std::lock_guard<std::mutex> lock(mu_);
        return fds_.size();
    }
    size_t accepted() { return srv_.accepted(); }
    void close() { srv_.close(); }

private:
    std::mutex mu_;
    std::vector<int> fds_;
    FakeServer srv_;
};

SourceOpener opener(const std::string& path) {
    return [path](const std::string&, std::string& why) {
        int err;
        bool to;
        const int fd = connect_unix(path, 1.0, err, to);
        why = fd < 0 ? errno_text(err) : path;
        return fd;
    };
}

std::optional<Json> take_until(Subscriber& s, const std::function<bool(const Json&)>& pred, double timeout_s) {
    const double end = mono_now() + timeout_s;
    while (mono_now() < end) {
        auto m = s.take(0.2);
        if (m && pred(*m)) return m;
    }
    return std::nullopt;
}

void test_hub() {
    const std::string dir = tmpdir(), path = dir + "/w.sock";
    StreamTiming t;
    t.retry_s = 0.1;
    t.idle_close_s = 0.3;
    Hub hub(opener(path), t);
    {
        // 워커 없음: 기준 상태 down
        auto p = hub.subscribe("w", 10);
        auto m = p.second->take(1.0);
        CHECK(m && (*m)["kf"].get<bool>() && (*m)["stream"] == "down");
        p.first->unsubscribe(p.second);
    }
    CHECK(wait_for([&] { return hub.stats().empty(); }, 3));
    auto w = std::make_unique<FakeWorker>(path);
    auto a = hub.subscribe("w", 50);
    auto up = take_until(*a.second, [](const Json& m) { return m.value("stream", "") == "up"; }, 3);
    CHECK(up && (*up)["topics"]["v"]["r"] == 0 && (*up)["events"].size() == 1);
    w->broadcast(Json{{"kf", false}, {"topics", {{"v", {{"r", 1}}}}}});
    w->broadcast(Json{{"kf", false}, {"topics", {{"v", {{"r", 2}}}}}, {"events", Json::array({Json{{"seq", 2}}})}});
    auto d = take_until(*a.second, [](const Json& m) { return m["topics"].contains("v") && m["topics"]["v"]["r"] == 2; }, 3);
    CHECK(d.has_value());
    auto b = hub.subscribe("w", 50);
    CHECK(a.first == b.first);
    auto kb = b.second->take(1.0);
    CHECK(kb && (*kb)["kf"].get<bool>() && (*kb)["topics"]["v"]["r"] == 2 && (*kb)["events"].size() == 2);
    CHECK(w->accepted() == 1);
    // 워커 재시작
    w->close();
    CHECK(take_until(*a.second, [](const Json& m) { return m.value("stream", "") == "down"; }, 3).has_value());
    w = std::make_unique<FakeWorker>(path);
    auto up2 = take_until(*a.second, [](const Json& m) { return m.value("stream", "") == "up"; }, 3);
    CHECK(up2 && (*up2)["events"].size() == 1);
    // 여러 스레드가 동시에 구독·해제(경합 시험)
    std::vector<std::thread> ts;
    std::atomic<int> got{0};
    for (int i = 0; i < 8; ++i)
        ts.emplace_back([&] {
            for (int k = 0; k < 20; ++k) {
                auto s = hub.subscribe("w", 50);
                if (s.second->take(0.5)) got.fetch_add(1);
                s.first->unsubscribe(s.second);
            }
        });
    for (int k = 0; k < 50; ++k) w->broadcast(Json{{"kf", false}, {"topics", {{"x", k}}}});
    for (auto& th : ts) th.join();
    CHECK(got.load() == 160);
    a.first->unsubscribe(a.second);
    b.first->unsubscribe(b.second);
    CHECK(wait_for([&] { return hub.stats().empty(); }, 3));
    CHECK(wait_for([&] { return w->open_conns() == 0; }, 3));
    hub.reap();
    w->close();
    hub.stop_all();
}

void test_sse_session() {
    const std::string dir = tmpdir(), path = dir + "/w.sock";
    FakeWorker w(path);
    Hub hub(opener(path));
    auto p = hub.subscribe("w", 5);
    SseSession s(p.first, p.second, 0.5);
    std::atomic<bool> stopping{false};
    std::vector<std::pair<double, std::string>> writes;
    auto write = [&](const std::string& x) {
        writes.push_back({mono_now(), x});
        return true;
    };
    CHECK(s.step(write, stopping) && writes.back().second == "retry: 2000\n\n");
    const double end = mono_now() + 2.0;
    int k = 0;
    while (mono_now() < end) {
        w.broadcast(Json{{"kf", false}, {"topics", {{"x", ++k}}}});
        CHECK(s.step(write, stopping));
    }
    double min_gap = 1e9;
    double prev = -1;
    int data = 0;
    for (auto& wr : writes) {
        if (wr.second.rfind("data: ", 0) != 0) continue;
        if (prev > 0) min_gap = std::min(min_gap, wr.first - prev);
        prev = wr.first;
        ++data;
    }
    CHECK(data >= 3 && data <= 12);
    CHECK(min_gap >= 0.19);  // hz 5 → 0.2 s
    // 아무것도 오지 않으면 ping
    writes.clear();
    for (int i = 0; i < 3; ++i) s.step(write, stopping);
    bool ping = false;
    for (auto& wr : writes) ping = ping || wr.second == ": ping\n\n";
    CHECK(ping);
    // 쓰기 실패 → 끝, 구독 해제
    w.broadcast(Json{{"kf", false}, {"topics", {{"y", 1}}}});
    bool more = true;
    for (int i = 0; i < 5 && more; ++i) more = s.step([](const std::string&) { return false; }, stopping);
    CHECK(!more);
    s.finish();
    CHECK(p.first->subscribers() == 0);
    stopping.store(true);
    CHECK(!s.step(write, stopping));
    hub.stop_all();
}

void test_gateway() {
    const std::string dir = tmpdir();
    const std::string data = dir + "/d";
    ::mkdir(data.c_str(), 0755);
    ::mkdir((data + "/run").c_str(), 0755);
    ::mkdir((data + "/run/stream").c_str(), 0755);
    ::mkdir((data + "/run/cmd").c_str(), 0755);
    // 가짜 Manager: ping·get·get_config·list
    FakeServer mgr(data + "/run/manager-rest.sock", [](int fd) {
        std::string buf;
        int err;
        if (read_line(fd, buf, 65536, mono_now() + 2, err) != Io::Ok) return;
        auto req = Json::parse(buf.substr(0, buf.find('\n')));
        const std::string op = req.value("op", "");
        Json reply = op == "list" ? Json{{"ok", true}, {"data", {{"workers", {"w"}}}}}
                     : op == "get_config" ? Json{{"ok", true}, {"data", {{"config", Json::object()}}}}
                                          : Json{{"ok", true}, {"data", {{"op", op}}}};
        send_line(fd, reply);
    });
    FakeWorker w(data + "/run/stream/w.sock");
    FakeServer cmd(data + "/run/cmd/w.sock", [](int fd) {
        std::string buf;
        int err;
        if (read_line(fd, buf, 65536, mono_now() + 2, err) != Io::Ok) return;
        send_line(fd, Json{{"ok", true}, {"data", Json::parse(buf.substr(0, buf.find('\n')))}});
    });
    GatewayConfig cfg;
    cfg.data_dir = data;
    cfg.workspace = dir;
    cfg.manager_socket = data + "/run/manager-rest.sock";
    cfg.bind = "127.0.0.1";
    cfg.port = 0;
    cfg.threads = 6;
    cfg.unix_socket = dir + "/gw.sock";
    cfg.ui.web = dir + "/web";
    cfg.ui.workers = dir + "/workers";
    cfg.ui.media_url = "http://{host}:8889/{path}";
    auto gwp = std::make_unique<Gateway>(cfg);
    std::string err;
    CHECK(gwp->start(err));
    const int port = gwp->port();

    // 동시 요청(Manager op·명령·읽기·화면) + SSE 2개
    std::atomic<int> ok{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> sse;
    std::atomic<int> sse_msgs{0};
    for (int i = 0; i < 2; ++i)
        sse.emplace_back([&] {
            httplib::Client c("127.0.0.1", port);
            c.set_read_timeout(5, 0);
            c.Get("/workers/w/stream?hz=50", [&](const char* d, size_t n) {
                if (std::string(d, n).find("data: ") != std::string::npos) sse_msgs.fetch_add(1);
                return !stop.load();
            });
        });
    std::thread pump([&] {
        int k = 0;
        while (!stop.load()) {
            w.broadcast(Json{{"kf", false}, {"topics", {{"k", ++k}}}});
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    CHECK(wait_for([&] { return sse_msgs.load() >= 4; }, 5));
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&, i] {
            httplib::Client c("127.0.0.1", port);
            for (int k = 0; k < 15; ++k) {
                httplib::Result r;
                switch ((i + k) % 4) {
                    case 0: r = c.Get("/workers"); break;
                    case 1: r = c.Post("/workers/w/command", "{\"cmd\": \"x\"}", "application/json"); break;
                    case 2: r = c.Get("/api/board/health"); break;
                    default: r = c.Get("/api/ui"); break;
                }
                if (r && r->status == 200) ok.fetch_add(1);
            }
        });
    for (auto& t : ts) t.join();
    CHECK(ok.load() == 60);
    // Unix 소켓으로도
    httplib::Client uc(cfg.unix_socket);
    uc.set_address_family(AF_UNIX);
    auto ur = uc.Get("/health");
    CHECK(ur && ur->status == 200 && ur->body.find("\"manager\": \"ok\"") != std::string::npos);
    // 상한 6 중 SSE 2개가 쓰는 중. 4개를 더 열면 그다음은 503
    std::vector<std::unique_ptr<httplib::Client>> held;
    std::vector<std::thread> holders;
    std::atomic<int> opened{0};
    for (int i = 0; i < 4; ++i)
        holders.emplace_back([&] {
            httplib::Client c("127.0.0.1", port);
            c.set_read_timeout(5, 0);
            c.Get("/workers/w/stream?hz=1", [&](const char*, size_t) {
                opened.fetch_add(1);
                return !stop.load();
            });
        });
    CHECK(wait_for([&] { return opened.load() >= 4; }, 5));
    httplib::Client c("127.0.0.1", port);
    auto r = c.Get("/workers");
    CHECK(r && r->status == 503);
    stop.store(true);
    for (auto& t : holders) t.join();
    for (auto& t : sse) t.join();
    pump.join();
    CHECK(wait_for([&] {
        auto x = c.Get("/workers");
        return x && x->status == 200;
    }, 5));
    CHECK(gwp->stalled().empty());
    CHECK(gwp->stop(5.0));
    gwp.reset();
    w.close();
    mgr.close();
    cmd.close();
}

}  // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    test_hub();
    test_sse_session();
    test_gateway();
    std::cout << (g_fail ? "FAILED " : "OK ") << g_checks - g_fail << "/" << g_checks << " checks\n";
    return g_fail ? 1 : 0;
}
