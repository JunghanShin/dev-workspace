#include "server.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>

#include "httplib/httplib.h"  // CPPHTTPLIB_USE_POLL은 CMake가 정한다(select의 FD_SETSIZE 한계를 피함)
#include "log.hpp"
#include "ui_rules.hpp"

namespace gw {

namespace {

constexpr size_t kMaxBody = 65536;
constexpr size_t kPoolSpare = 4;       // 상한을 넘은 요청에 503을 답할 여분 스레드
constexpr size_t kPoolQueue = 256;     // 받았지만 스레드를 기다리는 연결 상한(넘치면 닫음)
constexpr double kStallS = 10.0;       // 받기·위쪽 스레드가 이만큼 멈추면 끝냄(exit 4)

int error_status(const Json& code) {
    // Manager 오류 code → HTTP (design.md 6.4)
    if (!code.is_string()) return 500;
    const std::string c = code.get<std::string>();
    if (c == "bad_request") return 400;
    if (c == "forbidden") return 403;
    if (c == "not_found") return 404;
    if (c == "conflict") return 409;
    return 500;  // internal과 모르는 code
}

int command_status(const Json& code) {
    // 워커 명령 오류 code → HTTP (design.md 9.6). busy·forbidden은 지금 REST처럼 500
    if (!code.is_string()) return 500;
    const std::string c = code.get<std::string>();
    if (c == "bad_request") return 400;
    if (c == "unavailable") return 503;
    if (c == "timeout") return 504;
    return 500;
}

Reply json_reply(int status, const Json& body) {
    Reply r;
    r.status = status;
    r.body = py_dumps(body);
    r.ctype = "application/json; charset=utf-8";
    return r;
}

Reply json_error(int status, const std::string& code, const std::string& message) {
    return json_reply(status, Json{{"error", {{"code", code}, {"message", message}}}});
}

// 값으로 돌려준다(기본값이 임시 객체여도 안전하게)
Json get_or(const Json& obj, const char* key, const Json& dflt) {
    if (!obj.is_object()) return dflt;
    auto it = obj.find(key);
    return it == obj.end() ? dflt : *it;
}

// 동시 요청 수(응답을 다 쓰면 줄어듦. SSE면 그 화면이 끝날 때)
struct SlotGuard {
    std::atomic<size_t>& n;
    ~SlotGuard() { n.fetch_sub(1); }
};

class PoolQueue : public httplib::TaskQueue {
public:
    PoolQueue(Pool& pool, worker::ThreadWatch& watch, int slot) : pool_(pool), watch_(watch), slot_(slot) {}
    bool enqueue(std::function<void()> fn) override {
        watch_.beat(slot_);
        return pool_.enqueue(std::move(fn));
    }
    void shutdown() override {}  // 풀은 Gateway::stop이 멈춘다(TCP·Unix가 함께 씀)
    void on_idle() override { watch_.beat(slot_); }

private:
    Pool& pool_;
    worker::ThreadWatch& watch_;
    int slot_;
};

}  // namespace

// ---- Pool ----

Pool::Pool(size_t threads, size_t queue_cap) : cap_(queue_cap) {
    alive_ = threads;
    for (size_t i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
}

Pool::~Pool() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (!joined_)
        for (auto& t : threads_)
            if (t.joinable()) t.join();
}

bool Pool::enqueue(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_ || q_.size() >= cap_) return false;
        q_.push_back(std::move(fn));
    }
    cv_.notify_one();
    return true;
}

void Pool::run() {
    pthread_setname_np(pthread_self(), "gw.http");
    while (true) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::seconds(1), [&] { return stop_ || !q_.empty(); });
            if (q_.empty()) {
                if (stop_) break;
                continue;
            }
            fn = std::move(q_.front());
            q_.pop_front();
        }
        fn();  // 요청 하나(잠금 밖)
    }
    std::lock_guard<std::mutex> lock(mu_);
    --alive_;
    idle_cv_.notify_all();
}

bool Pool::shutdown(double timeout_s) {
    std::unique_lock<std::mutex> lock(mu_);
    stop_ = true;
    cv_.notify_all();
    if (!idle_cv_.wait_for(lock, std::chrono::duration<double>(timeout_s), [&] { return alive_ == 0; })) return false;
    lock.unlock();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    joined_ = true;
    return true;
}

// ---- Gateway ----

Gateway::Gateway(GatewayConfig cfg)
    : cfg_(std::move(cfg)),
      mgr_(cfg_.manager_socket, cfg_.connect_timeout, cfg_.response_timeout),
      reads_(cfg_.workspace, cfg_.data_dir),
      ui_(cfg_.ui),
      build_(path_join(path_join(cfg_.workspace, "dev"), "build_product.py"), cfg_.data_dir, cfg_.python),
      hub_([this](const std::string& wid, std::string& why) { return open_stream(wid, why); }, cfg_.stream),
      auth_(new AuthOff(cfg_.token, cfg_.ui_enabled)),
      recorder_(new NoRecorder()),
      watch_(std::chrono::milliseconds(static_cast<long>(kStallS * 1000))),
      pool_(cfg_.threads + kPoolSpare, kPoolQueue) {}

Gateway::~Gateway() {
    stopping_.store(true);
    if (tcp_) tcp_->stop();
    if (unix_) unix_->stop();
    for (auto& t : listeners_)
        if (t.joinable()) t.join();
}

void Gateway::setup(httplib::Server& svr, int watch_slot) {
    svr.new_task_queue = [this, watch_slot] { return new PoolQueue(pool_, watch_, watch_slot); };
    const auto sec = static_cast<time_t>(cfg_.io_timeout);
    const auto usec = static_cast<time_t>((cfg_.io_timeout - static_cast<double>(sec)) * 1e6);
    svr.set_read_timeout(sec, usec);
    svr.set_write_timeout(sec, usec);
    svr.set_keep_alive_max_count(1);
    svr.set_idle_interval(1, 0);
    svr.set_payload_max_length(kMaxBody);

    auto serve = [this](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader* reader) {
        if (req.method == "HEAD") {
            res.status = 501;
            res.set_content(py_dumps(Json{{"error", {{"code", "not_implemented"}, {"message", "Unsupported method ('HEAD')"}}}}),
                            "application/json; charset=utf-8");
            return;
        }
        if (in_flight_.fetch_add(1) >= cfg_.threads) {
            in_flight_.fetch_sub(1);
            res.status = 503;
            res.set_content(py_dumps(Json{{"error", {{"code", "unavailable"}, {"message", "too many requests"}}}}),
                            "application/json; charset=utf-8");
            return;
        }
        std::shared_ptr<SlotGuard> slot(new SlotGuard{in_flight_});  // 임시 객체 없이(소멸자가 한 번만 줄이게)
        const std::string& target = req.target;
        const size_t q = target.find('?');
        RequestView rq;
        rq.method = req.method;
        rq.path = target.substr(0, q);
        rq.query = q == std::string::npos ? "" : target.substr(q + 1);
        rq.header = [&req](const char* k) { return req.get_header_value(k); };
        auto body = [&req, reader]() {
            Body b;
            const std::string cl = req.get_header_value("Content-Length");
            long long len = 0;
            if (!cl.empty() && !py_int(cl, len)) {
                b.ok = false;
                b.error = "invalid literal for int() with base 10: '" + cl + "'";
                return b;
            }
            if (len > static_cast<long long>(kMaxBody) || len < 0) {
                b.ok = false;
                b.error = len < 0 ? "negative Content-Length" : "body too large";
                return b;
            }
            std::string raw;
            if (len > 0 && reader) {
                const bool ok = (*reader)([&raw](const char* d, size_t n) {
                    raw.append(d, n);
                    return raw.size() <= kMaxBody;
                });
                if (!ok) {
                    b.ok = false;
                    b.error = "incomplete body";
                    return b;
                }
            }
            // 바이트로 받은 JSON은 앞의 UTF-8 BOM을 봐준다(Python json.loads(bytes)와 같음)
            if (raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3);
            if (raw.empty()) return b;  // 본문 없음 → null
            auto j = parse_json(raw, &b.error);
            if (!j) b.ok = false;
            else b.value = std::move(*j);
            return b;
        };
        Reply r = handle(rq, body, req.remote_addr);
        res.status = r.status;
        for (const auto& h : r.headers) res.set_header(h.first, h.second);
        switch (r.kind) {
            case Reply::Kind::Json:
            case Reply::Kind::Raw:
                res.set_content(r.body, r.ctype);
                break;
            case Reply::Kind::File: {
                const int fd = ::open(r.file_path.c_str(), O_RDONLY | O_CLOEXEC);
                struct stat st{};
                if (fd < 0 || ::fstat(fd, &st) != 0) {
                    if (fd >= 0) ::close(fd);
                    res.status = 500;
                    res.set_content(py_dumps(Json{{"error", {{"code", "internal"}, {"message", "cannot open bundle"}}}}),
                                    "application/json; charset=utf-8");
                    break;
                }
                // 큰 파일(묶음 약 90 MB)은 조각으로. 조각 쓰기마다 상한(io_timeout)
                res.set_content_provider(
                    static_cast<size_t>(st.st_size), r.ctype,
                    [fd](size_t off, size_t len, httplib::DataSink& sink) {
                        std::string buf(std::min<size_t>(len, 1 << 20), '\0');
                        const ssize_t n = ::pread(fd, &buf[0], buf.size(), static_cast<off_t>(off));
                        if (n <= 0) return false;
                        return sink.write(buf.data(), static_cast<size_t>(n));
                    },
                    [fd, slot](bool) { ::close(fd); });
                break;
            }
            case Reply::Kind::Sse: {
                auto sse = r.sse;
                res.set_content_provider(
                    r.ctype,
                    [this, sse, slot](size_t, httplib::DataSink& sink) {
                        return sse->step([&sink](const std::string& s) { return sink.write(s.data(), s.size()); },
                                         stopping_);
                    },
                    [sse](bool) { sse->finish(); });
                break;
            }
        }
    };
    const std::string any = "[\\s\\S]*";  // 경로에 어떤 글자가 와도(디코딩한 \n 포함) 여기로
    svr.Get(any, [serve](const httplib::Request& req, httplib::Response& res) { serve(req, res, nullptr); });
    auto with_body = [serve](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
        serve(req, res, &reader);
    };
    svr.Post(any, with_body);
    svr.Put(any, with_body);
    svr.Delete(any, with_body);
    auto unsupported = [](const httplib::Request& req, httplib::Response& res) {
        res.status = 501;
        res.set_content(py_dumps(Json{{"error", {{"code", "not_implemented"},
                                                 {"message", "Unsupported method ('" + req.method + "')"}}}}),
                        "application/json; charset=utf-8");
    };
    svr.Patch(any, unsupported);
    svr.Options(any, unsupported);
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string what = "unknown";
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        GW_ERROR("request failed: %s", what.c_str());
        res.status = 500;
        res.set_content(py_dumps(Json{{"error", {{"code", "internal"}, {"message", what}}}}), "application/json; charset=utf-8");
    });
}

bool Gateway::start(std::string& err) {
    tcp_.reset(new httplib::Server());
    setup(*tcp_, watch_.add("gw.listen.tcp"));
    // cpp-httplib 기본은 Linux에서 SO_REUSEPORT만 켠다. 그러면 ① 앞 서버(지난 실행, Python REST)의 연결이
    // TIME-WAIT로 남아 있는 동안 같은 포트를 못 연다(보드에서 재시작 직후 bind 실패로 확인) ② 다른 프로그램이
    // 같은 포트를 열어 요청을 나눠 가질 수 있다. 그래서 SO_REUSEADDR만 켠다(Python http.server와 같음)
    tcp_->set_socket_options([](socket_t sock) {
        int one = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    });
    if (cfg_.port == 0) {
        bound_port_ = tcp_->bind_to_any_port(cfg_.bind);
        if (bound_port_ < 0) {
            err = "bind " + cfg_.bind + ":0 failed";
            return false;
        }
    } else {
        if (!tcp_->bind_to_port(cfg_.bind, cfg_.port)) {
            err = "bind " + cfg_.bind + ":" + std::to_string(cfg_.port) + " failed: " + errno_text(errno);
            return false;
        }
        bound_port_ = cfg_.port;
    }
    if (!cfg_.unix_socket.empty()) {
        struct stat st{};
        if (::lstat(cfg_.unix_socket.c_str(), &st) == 0) {
            if (!S_ISSOCK(st.st_mode)) {
                err = cfg_.unix_socket + " exists and is not a socket";
                return false;
            }
            ::unlink(cfg_.unix_socket.c_str());  // 지난 실행이 남긴 소켓
        }
        unix_.reset(new httplib::Server());
        setup(*unix_, watch_.add("gw.listen.unix"));
        unix_->set_address_family(AF_UNIX);
        if (!unix_->bind_to_port(cfg_.unix_socket, 80)) {
            err = "bind " + cfg_.unix_socket + " failed: " + errno_text(errno);
            return false;
        }
        ::chmod(cfg_.unix_socket.c_str(), 0660);  // 같은 그룹(nginx)만
    }
    listeners_.emplace_back([this] {
        pthread_setname_np(pthread_self(), "gw.listen");
        tcp_->listen_after_bind();
    });
    if (unix_)
        listeners_.emplace_back([this] {
            pthread_setname_np(pthread_self(), "gw.listen");
            unix_->listen_after_bind();
        });
    return true;
}

bool Gateway::stop(double timeout_s) {
    stopping_.store(true);
    if (tcp_) tcp_->stop();
    if (unix_) unix_->stop();
    for (auto& t : listeners_)
        if (t.joinable()) t.join();
    listeners_.clear();
    if (unix_) ::unlink(cfg_.unix_socket.c_str());
    const bool ok = pool_.shutdown(timeout_s);
    if (!ok) return false;
    build_.shutdown();
    hub_.stop_all();
    return true;
}

std::string Gateway::stalled() {
    std::string s;
    watch_.all_alive(worker::ThreadWatch::Clock::now(), &s);
    const std::string h = hub_.stalled(kStallS);
    if (!h.empty()) s += (s.empty() ? "" : ",") + h;
    return s;
}

void Gateway::tick() { hub_.reap(); }

// ---- 요청 단계: 신분 확인 → 권한 → 전달 → 기록 → 응답 ----

Reply Gateway::handle(const RequestView& rq, const std::function<Body()>& body, const std::string& remote) {
    const double t0 = mono_now();
    const Identity who = auth_->identify(rq);
    const Decision d = auth_->authorize(who, rq);
    Reply r;
    if (!d.allowed) {
        r = json_error(d.status, d.code, d.message);
    } else {
        try {
            r = forward(rq, body);
        } catch (const std::exception& e) {  // 처리 실패가 게이트웨이를 멈추지 않게
            GW_ERROR("%s %s failed: %s", rq.method.c_str(), rq.path.c_str(), e.what());
            r = json_error(500, "internal", e.what());
        }
    }
    recorder_->record(who, rq, r.status, (mono_now() - t0) * 1000);
    GW_INFO("%s \"%s %s%s%s\" %d", remote.c_str(), rq.method.c_str(), rq.path.c_str(), rq.query.empty() ? "" : "?",
            rq.query.c_str(), r.status);
    return r;
}

Reply Gateway::forward(const RequestView& rq, const std::function<Body()>& body) {
    const std::string& m = rq.method;
    std::string path = rq.path;
    if (!cfg_.ui_enabled && m == "GET" &&
        (path == "/" || path == "/index.html" || (starts_with(path, "/api/") && !starts_with(path, "/api/board/"))))
        return json_error(404, "not_found", "board UI disabled (WORKSPACE_UI=0)");

    // 화면 경로(ui.py). /api/board/<경로>는 접두어만 떼고 아래 경로 표로
    const UiRoute u = ui_route(m, path);
    switch (u.kind) {
        case UiKind::Board:
            path = path.substr(std::string("/api/board").size());
            break;
        case UiKind::Remote: {
            auto res = ui_.remote(u.remote, parse_qs(rq.query), reads_);
            return json_reply(res.first, res.second);
        }
        case UiKind::Panels:
            return json_reply(200, ui_.panels());
        case UiKind::UiInfo:
            return json_reply(200, ui_.ui_info(rq.header("Host"), cfg_.data_dir, cfg_.product));
        case UiKind::Root:
        case UiKind::None:
            break;
    }
    if (u.kind != UiKind::Board && m == "GET") {
        std::string file = path;
        if (u.kind == UiKind::Root) {
            if (!ui_.available()) return json_error(404, "not_found", "UI files not found in " + ui_.config().web);
            file = "/shell.html";
        }
        if (auto f = ui_.static_file(file)) {
            Reply r;
            r.kind = Reply::Kind::Raw;
            r.body = std::move(f->data);
            r.ctype = f->ctype;
            r.headers.push_back({"Cache-Control", "no-store"});
            return r;
        }
    }

    const Route rt = route(m, path, cfg_.product);
    switch (rt.kind) {
        case RouteKind::Health:
            return health();
        case RouteKind::ProductOff:
            return json_error(404, "not_found", "not available in product mode");
        case RouteKind::ProductBuild:
            return product_build(m, rstrip_char(path, '/'), rt.sub, rq.query, body);
        case RouteKind::Read:
            return read(rt, rq.query);
        case RouteKind::Stream:
            return stream(rt.id, rq.query);
        case RouteKind::Command:
            return command(rt.id, body);
        case RouteKind::Manager:
            return manager_op(rt, body);
        case RouteKind::MethodNotAllowed:
            return json_error(405, "method_not_allowed", m + " not allowed on " + path);
        case RouteKind::None:
            break;
    }
    return json_error(404, "not_found", "no route " + path);
}

Reply Gateway::manager_reply(const Json& resp, int ok_status) {
    static const Json null_json = nullptr, empty = Json::object();
    if (py_truthy(get_or(resp, "ok", null_json))) return json_reply(ok_status, get_or(resp, "data", null_json));
    const Json& err = get_or(resp, "error", empty);
    return json_reply(error_status(get_or(err, "code", null_json)), Json{{"error", err}});
}

Reply Gateway::manager_op(const Route& r, const std::function<Body()>& body) {
    Json req = Json::object();
    if (r.save_false) req["save"] = false;
    req["op"] = r.op;
    if (r.has_id) req["id"] = r.id;
    if (r.has_name) req["name"] = r.name;
    if (!r.body_key.empty()) {
        Body b = body();
        if (!b.ok) return json_error(400, "bad_request", "invalid JSON body: " + b.error);
        req[r.body_key] = b.value;
    }
    Json resp;
    try {
        resp = mgr_.call(req);
    } catch (const ManagerUnavailable& e) {
        return json_error(503, "unavailable", std::string("manager unavailable: ") + e.what());
    }
    return manager_reply(resp, r.ok_status);
}

Reply Gateway::health() {
    std::string manager;
    try {
        manager = py_truthy(get_or(mgr_.call(Json{{"op", "ping"}}), "ok", Json(nullptr))) ? "ok" : "error";
    } catch (const ManagerUnavailable&) {
        manager = "unreachable";
    }
    Json o = Json::object();
    o["rest"] = "ok";
    o["manager"] = manager;
    return json_reply(200, o);
}

Reply Gateway::read(const Route& r, const std::string& query) {
    try {
        const ReadResult res = reads_.handle(r.read, r.id, r.has_id, parse_qs(query));
        return json_reply(res.status, res.body);
    } catch (const BadRequest& e) {
        return json_error(400, "bad_request", e.what());
    } catch (const std::exception& e) {  // 읽기 실패가 게이트웨이를 멈추지 않게
        GW_ERROR("read %s failed: %s", r.read.c_str(), e.what());
        return json_error(500, "internal", r.read + ": " + e.what());
    }
}

namespace {
// 워커 설정의 소켓 위치(없으면 기본 위치 <데이터>/run/<kind>/<id>.sock)
std::string socket_path(const Json& resp, const char* key, const std::string& data_dir, const char* kind,
                        const std::string& id) {
    static const Json empty = Json::object(), null_json = nullptr;
    const Json data = get_or(resp, "data", null_json);
    const Json config = get_or(py_truthy(data) ? data : empty, "config", null_json);
    const Json sock = get_or(py_truthy(config) ? config : empty, key, null_json);
    if (py_truthy(sock)) return py_str(sock);
    return path_join(path_join(path_join(data_dir, "run"), kind), id + ".sock");
}
}  // namespace

Reply Gateway::command(const std::string& id, const std::function<Body()>& body) {
    Body b = body();
    if (!b.ok) return json_error(400, "bad_request", "invalid JSON body: " + b.error);
    if (!b.value.is_object() || !b.value.contains("cmd") || !b.value["cmd"].is_string())
        return json_error(400, "bad_request", "body must be {\"cmd\": \"...\", \"args\": {...}}");
    // 소켓 위치는 워커 설정(command_socket), 없으면 기본 위치 (design.md 9.6)
    Json resp;
    try {
        resp = mgr_.call(Json{{"op", "get_config"}, {"id", id}});
    } catch (const ManagerUnavailable& e) {
        return json_error(503, "unavailable", std::string("manager unavailable: ") + e.what());
    }
    if (!py_truthy(get_or(resp, "ok", Json(nullptr)))) return manager_reply(resp, 200);
    const std::string path = socket_path(resp, "command_socket", cfg_.data_dir, "cmd", id);
    Json request = Json::object();
    request["cmd"] = b.value["cmd"];
    const Json& args = get_or(b.value, "args", Json(nullptr));
    request["args"] = py_truthy(args) ? args : Json::object();
    const CommandResult r = worker_command(path, request, 1.0, cfg_.command_timeout);
    switch (r.kind) {
        case CommandResult::NoSocket:
            return json_error(503, "unavailable", "worker " + id + " is not running (no command socket)");
        case CommandResult::Timeout:
            return json_error(504, "timeout", "worker " + id + " did not answer");
        case CommandResult::Failed:
            return json_error(503, "unavailable", "worker " + id + ": " + r.message);
        case CommandResult::Ok:
            break;
    }
    if (py_truthy(get_or(r.reply, "ok", Json(nullptr)))) return json_reply(200, get_or(r.reply, "data", Json(nullptr)));
    const Json& err = get_or(r.reply, "error", Json::object());
    return json_reply(command_status(get_or(err, "code", Json(nullptr))), Json{{"error", err}});
}

int Gateway::open_stream(const std::string& wid, std::string& why) {
    Json resp;
    try {
        resp = mgr_.call(Json{{"op", "get_config"}, {"id", wid}});
    } catch (const ManagerUnavailable& e) {
        why = std::string("manager unavailable: ") + e.what();
        return -1;
    }
    if (!py_truthy(get_or(resp, "ok", Json(nullptr)))) {
        why = py_str(get_or(resp, "error", Json(nullptr)));
        return -1;
    }
    const std::string path = socket_path(resp, "stream_socket", cfg_.data_dir, "stream", wid);
    int err;
    bool timed_out;
    const int fd = connect_unix(path, 1.0, err, timed_out);
    why = fd >= 0 ? path : path + ": " + (timed_out ? std::string("timed out") : errno_text(err));
    return fd;
}

Reply Gateway::stream(const std::string& id, const std::string& query) {
    const Params p = parse_qs(query);
    double hz = 10;  // 화면당 초당 최대 전송(0.2~50)
    auto it = p.find("hz");
    if (it != p.end() && !py_float(it->second, hz)) return json_error(400, "bad_request", "hz must be a number");
    hz = py_min(py_max(hz, 0.2), 50);
    // 없는 워커는 바로 404(있는 워커가 꺼져 있으면 스트림은 열고 stream:"down"으로 알린다)
    Json resp;
    try {
        resp = mgr_.call(Json{{"op", "get"}, {"id", id}});
    } catch (const ManagerUnavailable& e) {
        return json_error(503, "unavailable", std::string("manager unavailable: ") + e.what());
    }
    if (!py_truthy(get_or(resp, "ok", Json(nullptr)))) return manager_reply(resp, 200);
    auto sub = hub_.subscribe(id, hz);
    Reply r;
    r.kind = Reply::Kind::Sse;
    r.ctype = "text/event-stream; charset=utf-8";
    r.headers.push_back({"Cache-Control", "no-store"});
    r.headers.push_back({"X-Accel-Buffering", "no"});
    r.sse = std::make_shared<SseSession>(sub.first, sub.second, cfg_.ping_s);
    return r;
}

Reply Gateway::product_build(const std::string& method, const std::string& path, const std::string& sub,
                             const std::string& query, const std::function<Body()>& body) {
    if (sub.empty() && method == "GET") return json_reply(200, build_.status());
    if (sub.empty() && method == "POST") {
        Body b = body();
        if (!b.ok) return json_error(400, "bad_request", "invalid JSON body: " + b.error);
        const Json obj = py_truthy(b.value) ? b.value : Json::object();
        const Json profile = obj.is_object() ? obj.value("profile", Json(nullptr)) : Json(nullptr);
        if (!profile.is_string() || !valid_profile(profile.get<std::string>()))
            return json_error(400, "bad_request", "body must be {\"profile\": \"<name>\"}");
        const Json power = obj.is_object() ? obj.value("power_mode", Json(nullptr)) : Json(nullptr);
        if (!power.is_null() && (!power.is_string() || !valid_power_mode(power.get<std::string>())))
            return json_error(400, "bad_request", "power_mode must be a mode name or keep");
        try {
            return json_reply(202, build_.start(profile.get<std::string>(), power.is_null() ? "" : power.get<std::string>()));
        } catch (const Busy& e) {
            return json_error(409, "conflict", e.what());
        }
    }
    if (sub.empty() && method == "DELETE") return json_reply(200, Json{{"cancelled", build_.cancel()}});
    if (sub == "/check" && method == "GET") {
        const Params p = parse_qs(query);
        auto it = p.find("profile");
        const std::string profile = it == p.end() ? "" : it->second;
        if (!valid_profile(profile)) return json_error(400, "bad_request", "profile is required");
        return json_reply(200, build_.check(profile));
    }
    if (sub == "/download" && method == "GET") {
        const Json last = build_.last();
        if (last.is_null()) return json_error(404, "not_found", "no product bundle yet");
        Reply r;
        r.kind = Reply::Kind::File;
        r.file_path = last["path"].get<std::string>();
        r.ctype = "application/gzip";
        r.headers.push_back({"Content-Disposition", "attachment; filename=\"" + last["file"].get<std::string>() + "\""});
        r.headers.push_back({"Cache-Control", "no-store"});
        return r;
    }
    if (sub.empty() || sub == "/check" || sub == "/download")
        return json_error(405, "method_not_allowed", method + " not allowed on " + path);
    return json_error(404, "not_found", "no route " + path);
}

}  // namespace gw
