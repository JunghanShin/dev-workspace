// 스레드 시험(TSan으로도 돌림): 가짜 워커 스트림 소켓·가짜 Manager 소켓으로
//   - Hub/Upstream/Subscriber: 기준 상태, 합치기, 위쪽 연결 하나, 워커 재시작(down → up), 화면이 떠나면 정리
//   - SseSession: hz 간격, ping, 쓰기 실패면 끝(구독 해제)
//   - MuxSession(/streams): 스레드 하나로 워커 여럿, 이름표, 워커마다 hz, 하나가 꺼져도 다른 것은 계속
//   - Gateway(HTTP 끝까지): 여러 스레드가 동시에 요청·SSE, 동시 요청 상한 → 503, 상한 안 끝내기
//   - FileRecorder(gw.log): 큐 상한·이어 쓰기·여러 스레드·파일 회전과 전체 상한
//   - UsersFile: 읽는 중 바꿔 써도 깨진 값 없음, 깨진 파일은 마지막 값, 지우면 사용자 없음
//   - ManagerCache(gw.mgr): list 순서, 구독 값, Manager가 멈추면 새롭지 않음 → 직접 묻게
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

#include "auth.hpp"
#include "httplib/httplib.h"
#include "hub.hpp"
#include "mgr_cache.hpp"
#include "recorder.hpp"
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

// /streams 본문: 스레드 하나가 워커 여럿을 돈다. 이름표, 없는 워커 down 한 번, 하나가 꺼져도 다른 하나는 계속, 다시 옴
void test_mux_session() {
    const std::string dir = tmpdir();
    StreamTiming t;
    t.retry_s = 0.1;
    Hub hub([dir](const std::string& id, std::string& why) {
        int err;
        bool to;
        const int fd = connect_unix(dir + "/" + id + ".sock", 1.0, err, to);
        why = fd < 0 ? errno_text(err) : id;
        return fd;
    }, t);
    auto w1 = std::make_unique<FakeWorker>(dir + "/a.sock");
    FakeWorker w2(dir + "/b.sock");
    auto waker = std::make_shared<Waker>();
    std::vector<MuxSession::Feed> feeds;
    for (auto [id, hz] : {std::pair<const char*, double>{"a", 50}, {"b", 5}, {"nobody", 10}}) {
        auto p = hub.subscribe(id, hz, waker);
        feeds.push_back({id, p.first, p.second});
    }
    auto ups = feeds;  // 끝난 뒤 구독자 수를 보려고
    MuxSession s(std::move(feeds), waker, 0.5);
    std::atomic<bool> stopping{false};
    std::vector<Json> got;
    std::vector<double> b_at;
    auto write = [&](const std::string& x) {
        size_t pos = 0;
        while ((pos = x.find("data: ", pos)) != std::string::npos) {
            const size_t end = x.find("\n\n", pos);
            got.push_back(Json::parse(x.substr(pos + 6, end - pos - 6)));
            if (got.back().value("worker", "") == "b") b_at.push_back(mono_now());
            pos = end;
        }
        return true;
    };
    auto until = [&](const std::function<bool(const Json&)>& pred, double timeout_s) {
        const double end = mono_now() + timeout_s;
        size_t seen = 0;
        while (mono_now() < end) {
            CHECK(s.step(write, stopping));
            for (; seen < got.size(); ++seen)
                if (pred(got[seen])) return true;
        }
        return false;
    };
    auto is = [](const char* id, const char* stream) {
        return [id, stream](const Json& m) { return m.value("worker", "") == id && m.value("stream", "") == stream; };
    };
    CHECK(until(is("a", "up"), 3));
    CHECK(until(is("b", "up"), 3));
    CHECK(until([](const Json& m) { return m.value("worker", "") == "nobody" && m.value("down", false); }, 3));
    // 모든 줄에 이름표, 없는 워커의 down은 한 번만
    int nobody = 0;
    for (const auto& m : got) {
        CHECK(m.contains("worker"));
        if (m["worker"] == "nobody") ++nobody;
    }
    // 둘이 함께 흐름. b는 5 Hz
    got.clear();
    b_at.clear();
    const double end = mono_now() + 1.5;
    int k = 0;
    while (mono_now() < end) {
        w1->broadcast(Json{{"kf", false}, {"topics", {{"x", ++k}}}});
        w2.broadcast(Json{{"kf", false}, {"topics", {{"y", k}}}});
        CHECK(s.step(write, stopping));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    int na = 0, nb = 0;
    for (const auto& m : got) {
        if (m["worker"] == "a") ++na;
        if (m["worker"] == "b") ++nb;
    }
    CHECK(na > nb && nb >= 4 && nb <= 10);
    for (size_t i = 1; i < b_at.size(); ++i) CHECK(b_at[i] - b_at[i - 1] >= 0.19);
    // a가 꺼짐 → a만 down, b는 계속
    w1->close();
    CHECK(until([](const Json& m) { return m.value("worker", "") == "a" && m.value("down", false); }, 3));
    got.clear();
    w2.broadcast(Json{{"kf", false}, {"topics", {{"after", 1}}}});
    CHECK(until([](const Json& m) { return m.value("worker", "") == "b" && m["topics"].contains("after"); }, 3));
    // a가 다시 옴 → 기준 상태부터
    w1 = std::make_unique<FakeWorker>(dir + "/a.sock");
    CHECK(until([](const Json& m) { return m.value("worker", "") == "a" && m.value("kf", false) && m.value("stream", "") == "up"; }, 3));
    for (int i = 0; i < 3; ++i) s.step(write, stopping);  // ping 시간이 와도 끝나지 않음
    CHECK(nobody == 1);
    // 쓰기 실패 → 끝, 모두 구독 해제
    w2.broadcast(Json{{"kf", false}, {"topics", {{"z", 1}}}});
    bool more = true;
    for (int i = 0; i < 10 && more; ++i) more = s.step([](const std::string&) { return false; }, stopping);
    CHECK(!more);
    s.finish();
    for (auto& f : ups) CHECK(f.up->subscribers() == 0);
    stopping.store(true);
    CHECK(!s.step(write, stopping));
    w1->close();
    w2.close();
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
                    case 1: r = c.Post("/workers/w/command", "{\"cmd\": \"get_status\"}", "application/json"); break;  // 읽기(운용 모드에서도)
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

// ---- 4단계: 명령 기록(gw.log)·사용자 파일 다시 읽기·Manager 캐시(gw.mgr) ----

RecordFields sample(int i) {
    RecordFields f;
    f.t = iso_now();
    f.user = "u" + std::to_string(i % 3);
    f.role = "operator";
    f.mode = "operation";
    f.remote = "127.0.0.1";
    f.method = "POST";
    f.path = "/workers/w" + std::to_string(i) + "/command";
    f.cmd = "c" + std::to_string(i);
    f.code = 200;
    f.ms = 1.5;
    return f;
}

std::vector<std::string> log_files(const std::string& dir) {
    std::vector<std::string> out;
    for (const auto& n : list_dir(dir))
        if (starts_with(n, "gateway-") && ends_with(n, ".jsonl")) out.push_back(dir + "/" + n);
    return out;
}

void test_recorder() {
    const std::string base = tmpdir();
    {
        // 큐 상한: 스레드가 돌기 전에 6개 → 4개만 들어가고 2개는 버리고 셈
        FileRecorder r(base + "/a", "boot0001", RecorderLimits{1 << 20, 1 << 22, 4});
        for (int i = 0; i < 6; ++i) r.record(sample(i));
        CHECK(r.stats()["dropped"] == 2);
        r.start();
        CHECK(wait_for([&] { return r.stats()["written"] == 4; }, 5));
        r.stop(2.0);
        auto text = read_file(base + "/a/gateway-boot0001.jsonl");
        CHECK(text && std::count(text->begin(), text->end(), '\n') == 4);
        // 같은 부팅에서 다시 뜨면 이어 쓴다
        FileRecorder r2(base + "/a", "boot0001");
        r2.start();
        r2.record(sample(9));
        r2.stop(2.0);
        text = read_file(base + "/a/gateway-boot0001.jsonl");
        CHECK(text && std::count(text->begin(), text->end(), '\n') == 5);
    }
    {
        // 여러 요청 스레드 + 파일 상한(2000 B)·전체 상한(6000 B): 넘기면 이름을 바꾸고 오래된 것부터 지움
        const std::string dir = base + "/b";
        FileRecorder r(dir, "boot0002", RecorderLimits{2000, 6000, 1024});
        r.start();
        std::vector<std::thread> ts;
        for (int k = 0; k < 4; ++k)
            ts.emplace_back([&r, k] {
                for (int i = 0; i < 100; ++i) r.record(sample(k * 100 + i));
            });
        for (auto& t : ts) t.join();
        CHECK(wait_for([&] {
            const Json s = r.stats();
            return s["written"].get<unsigned long long>() + s["dropped"].get<unsigned long long>() == 400;
        }, 5));
        r.stop(2.0);
        long long total = 0;
        size_t lines = 0;
        bool parsed = true;
        for (const auto& f : log_files(dir)) {
            total += size_of(f);
            for_each_line(f, [&](const std::string& l) {
                auto j = parse_json(l);
                parsed = parsed && j && j->size() == 11;
                ++lines;
                return true;
            });
        }
        CHECK(log_files(dir).size() >= 2);
        CHECK(total <= 6000 + 2000 + 400);  // 전체 상한 + 쓰는 중인 파일 하나
        CHECK(parsed && lines > 0);
    }
}

void test_users_reload() {
    const std::string dir = tmpdir();
    const std::string path = dir + "/users.json";
    UsersFile uf(path, 0.02);
    CHECK(uf.get()->users.empty());  // 없는 파일 = 사용자 없음
    auto make = [](int n) {
        UsersDb db;
        for (int i = 0; i < n; ++i) {
            User u;
            u.name = "u" + std::to_string(i);
            u.role = Role::Viewer;
            db.users.push_back(u);
        }
        return dump_users(db);
    };
    std::string err;
    CHECK(write_file_atomic(path, make(1), 0600, err));
    struct stat st{};
    CHECK(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(wait_for([&] { return uf.get()->users.size() == 1; }, 3));
    // 여러 스레드가 읽는 중에 파일을 계속 바꿔 써도(원자적 바꿔 끼우기) 깨진 값을 보지 않는다
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int k = 0; k < 4; ++k)
        ts.emplace_back([&] {
            while (!stop.load()) {
                const size_t n = uf.get()->users.size();
                if (n < 1 || n > 3) bad.fetch_add(1);
            }
        });
    for (int i = 0; i < 12; ++i) {
        CHECK(write_file_atomic(path, make(1 + i % 3), 0600, err));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(write_file_atomic(path, make(3), 0600, err));
    CHECK(wait_for([&] { return uf.get()->users.size() == 3; }, 3));
    stop.store(true);
    for (auto& t : ts) t.join();
    CHECK(bad.load() == 0);
    // 깨진 파일: 마지막으로 읽은 목록을 그대로
    CHECK(write_file_atomic(path, "{broken", 0600, err));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(uf.get()->users.size() == 3);
    CHECK(::unlink(path.c_str()) == 0);
    CHECK(wait_for([&] { return uf.get()->users.empty(); }, 3));  // 지우면 사용자 없음
}

// 구독을 아는 가짜 Manager: 주기마다 워커 전체 + system, list는 w2·w1 순서(구독 순서와 다르게)
struct FakeSubManager {
    std::atomic<bool> stall{false};
    std::atomic<int> lists{0}, modes{0};
    std::mutex mu;
    std::string last_mode;
    std::unique_ptr<FakeServer> srv;
    static Json status(const std::string& id, int n) { return Json{{"id", id}, {"actual_state", "running"}, {"n", n}}; }
    explicit FakeSubManager(const std::string& path) {
        srv.reset(new FakeServer(path, [this](int fd) {
            std::string buf;
            int err;
            if (read_line(fd, buf, 65536, mono_now() + 2, err) != Io::Ok) return;
            const Json req = Json::parse(buf.substr(0, buf.find('\n')));
            const std::string op = req.value("op", "");
            if (op == "list") {
                lists.fetch_add(1);
                send_line(fd, Json{{"ok", true}, {"data", Json::array({status("w2", 0), status("w1", 0)})}});
                return;
            }
            if (op == "set_access_mode") {
                modes.fetch_add(1);
                std::lock_guard<std::mutex> lock(mu);
                last_mode = req.value("mode", "");
                send_line(fd, Json{{"ok", true}, {"data", {{"access_mode", last_mode}}}});
                return;
            }
            if (op != "subscribe") {
                send_line(fd, Json{{"ok", true}, {"data", {{"op", op}}}});
                return;
            }
            send_line(fd, Json{{"ok", true}, {"data", {{"subscribed", {"system", "workers"}}}}});
            int n = 0;
            while (!srv->stopping()) {
                if (!stall.load()) {
                    ++n;
                    send_line(fd, Json{{"event", "worker_status"}, {"data", status("w1", n)}});
                    send_line(fd, Json{{"event", "worker_status"}, {"data", status("w2", n)}});
                    send_line(fd, Json{{"event", "system"}, {"data", {{"clients", Json::object()}, {"access_mode", "operation"}}}});
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }));
    }
};

void test_manager_cache() {
    const std::string dir = tmpdir();
    FakeSubManager fake(dir + "/mgr.sock");
    ManagerCache c(dir + "/mgr.sock", 1.0, 2.0, 0.5, 100);
    std::atomic<int> systems{0};
    c.on_system = [&](const Json& s) {
        if (s.value("access_mode", "") == "operation") systems.fetch_add(1);
    };
    worker::ThreadWatch watch(std::chrono::milliseconds(3000));
    Json out;
    CHECK(!c.workers(out));  // 시작 전: 새롭지 않음 → 부르는 쪽이 직접
    c.start(&watch);
    CHECK(wait_for([&] { return c.workers(out); }, 5));
    CHECK(out.is_array() && out.size() == 2 && out[0]["id"] == "w2" && out[1]["id"] == "w1");  // list 순서 그대로
    CHECK(fake.lists.load() == 1);  // 순서는 처음 한 번만 배움
    // 요청 스레드 여럿이 읽는 동안 값이 계속 바뀜
    std::vector<std::thread> ts;
    std::atomic<int> hits{0};
    for (int k = 0; k < 4; ++k)
        ts.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                Json a, b, s;
                if (c.workers(a) && c.worker("w1", b) && c.system(s)) hits.fetch_add(1);
            }
        });
    for (auto& t : ts) t.join();
    CHECK(hits.load() > 700);
    CHECK(!c.worker("nope", out));  // 없는 워커는 Manager가 not_found로
    CHECK(wait_for([&] { return c.worker("w1", out) && out["n"].get<int>() > 2; }, 3));  // 구독으로 계속 새 값
    CHECK(systems.load() > 0);
    CHECK(fake.lists.load() == 1);
    // Manager가 멈추면(구독이 조용함) fresh_s(0.5 s) 뒤에는 캐시를 쓰지 않는다
    fake.stall.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(!c.workers(out) && !c.system(out));
    CHECK(c.state()["fresh"] == false && c.state()["connected"] == true);
    fake.stall.store(false);
    CHECK(wait_for([&] { return c.workers(out); }, 3));
    CHECK(watch.all_alive(worker::ThreadWatch::Clock::now()));
    c.stop();
    // Manager가 없으면 연결만 다시 시도(멈추지 않음)
    ManagerCache none(dir + "/nothing.sock", 0.2, 0.2, 0.5, 100);
    none.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!none.workers(out) && none.state()["connected"] == false);
    none.stop();
    fake.srv->close();
}

}  // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    test_hub();
    test_sse_session();
    test_mux_session();
    test_gateway();
    test_recorder();
    test_users_reload();
    test_manager_cache();
    std::cout << (g_fail ? "FAILED " : "OK ") << g_checks - g_fail << "/" << g_checks << " checks\n";
    return g_fail ? 1 : 0;
}
