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
#include "settings.hpp"
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

// 운용 모드에서 막을 때(docs/command-path.md 3장): 403 + need
Reply json_error(int status, const std::string& code, const std::string& message) {
    return json_reply(status, Json{{"error", {{"code", code}, {"message", message}}}});
}

Reply development_required() {
    Json o = Json::object();
    o["error"] = Json{{"code", "forbidden"}, {"message", "development mode required"}};
    o["need"] = "development";
    return json_reply(403, o);
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
            // 시간 없이 기다린다: 일이 오거나(enqueue가 깨움) 멈출 때(shutdown이 모두 깨움)만 깬다.
            // 예전에는 1초마다 깼는데, 스레드 48개가 그러니 쉬는 동안에도 CPU 0.5%(보드 실측)
            cv_.wait(lock, [&] { return stop_ || !q_.empty(); });
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
      cache_(cfg_.manager_socket, cfg_.connect_timeout, cfg_.response_timeout),
      watch_(std::chrono::milliseconds(static_cast<long>(kStallS * 1000))),
      pool_(cfg_.threads + kPoolSpare, kPoolQueue) {
    command_timeout_.store(cfg_.command_timeout);
    mode_.mode = cfg_.product ? "operation" : cfg_.access_mode;  // docs/modes.md: 제품은 늘 운용
    mode_.since = iso_now();
    mode_.by = cfg_.product ? "product" : "workspace";
    if (cfg_.auth_on) {
        AuthConfig ac;
        ac.users_file = cfg_.users_file.empty() ? path_join(path_join(cfg_.data_dir, "config"), "users.json") : cfg_.users_file;
        ac.product = cfg_.product;
        ac.ui_enabled = cfg_.ui_enabled;
        auth_on_ = new AuthOn(ac);
        auth_.reset(auth_on_);
    } else {
        auth_.reset(new AuthOff(cfg_.token, cfg_.ui_enabled));
    }
    if (cfg_.record && !cfg_.data_dir.empty()) {
        file_rec_ = new FileRecorder(path_join(path_join(cfg_.data_dir, "records"), "gateway"), boot_id8());
        recorder_.reset(file_rec_);
    } else {
        recorder_.reset(new NoRecorder());
    }
    cache_.on_system = [this](const Json& sys) { sync_mode(sys); };
}

Gateway::~Gateway() {
    stopping_.store(true);
    if (tcp_) tcp_->stop();
    if (unix_) unix_->stop();
    for (auto& t : listeners_)
        if (t.joinable()) t.join();
    cache_.stop();
}

void Gateway::setup(httplib::Server& svr, int watch_slot, bool via_unix) {
    svr.new_task_queue = [this, watch_slot] { return new PoolQueue(pool_, watch_, watch_slot); };
    const auto sec = static_cast<time_t>(cfg_.io_timeout);
    const auto usec = static_cast<time_t>((cfg_.io_timeout - static_cast<double>(sec)) * 1e6);
    svr.set_read_timeout(sec, usec);
    svr.set_write_timeout(sec, usec);
    svr.set_keep_alive_max_count(1);
    svr.set_idle_interval(1, 0);
    svr.set_payload_max_length(kMaxBody);

    auto serve = [this, via_unix](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader* reader) {
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
        // 보낸 쪽 주소: Unix 소켓이면 앞의 nginx가 넣은 X-Forwarded-For(nginx는 $remote_addr로 덮어씀),
        // TCP면 접속 주소(직접 붙은 클라이언트가 X-Forwarded-For를 꾸며도 무시)
        std::string remote = req.remote_addr;
        if (via_unix) {
            const std::string xff = req.get_header_value("X-Forwarded-For");
            const size_t comma = xff.rfind(',');
            remote = strip(comma == std::string::npos ? xff : xff.substr(comma + 1));
            if (remote.empty()) remote = "unix";
        }
        Reply r = handle(rq, body, remote);
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
    if (!cfg_.tcp && cfg_.unix_socket.empty()) {
        err = "WORKSPACE_GATEWAY_TCP=0 needs WORKSPACE_GATEWAY_SOCKET";
        return false;
    }
    if (cfg_.tcp && !start_tcp(err)) return false;
    if (!cfg_.unix_socket.empty() && !start_unix(err)) return false;
    if (file_rec_) file_rec_->start();
    if (cfg_.cache) cache_.start(&watch_);
    if (tcp_)
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

bool Gateway::start_tcp(std::string& err) {
    tcp_.reset(new httplib::Server());
    setup(*tcp_, watch_.add("gw.listen.tcp"), false);
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
    return true;
}

bool Gateway::start_unix(std::string& err) {
    {
        struct stat st{};
        if (::lstat(cfg_.unix_socket.c_str(), &st) == 0) {
            if (!S_ISSOCK(st.st_mode)) {
                err = cfg_.unix_socket + " exists and is not a socket";
                return false;
            }
            ::unlink(cfg_.unix_socket.c_str());  // 지난 실행이 남긴 소켓
        }
        unix_.reset(new httplib::Server());
        setup(*unix_, watch_.add("gw.listen.unix"), true);
        unix_->set_address_family(AF_UNIX);
        if (!unix_->bind_to_port(cfg_.unix_socket, 80)) {
            err = "bind " + cfg_.unix_socket + " failed: " + errno_text(errno);
            return false;
        }
        ::chmod(cfg_.unix_socket.c_str(), 0660);  // 같은 그룹(nginx)만
    }
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
    cache_.stop();
    if (!ok) return false;
    build_.shutdown();
    hub_.stop_all();
    if (file_rec_) file_rec_->stop(2.0);  // 남은 기록 줄을 쓰고(상한)
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

// 쿠키 Secure: 앞의 nginx가 HTTPS로 받았다고 알려 줄 때만(proxy_set_header X-Forwarded-Proto $scheme가 덮어씀)
static std::string secure_flag(const RequestView& rq) {
    return rq.header && rq.header("X-Forwarded-Proto") == "https" ? "; Secure" : "";
}

Reply Gateway::handle(const RequestView& rq, const std::function<Body()>& body, const std::string& remote) {
    const double t0 = mono_now();
    const std::string mode_at = mode().mode;
    Identity who;
    Ctx ctx;
    Reply r;
    try {
        who = auth_->identify(rq);
        ctx.from = who.via == "token" ? "remote:" + who.token : "screen";
        const Decision d = auth_->authorize(who, rq);
        if (!d.allowed) r = json_error(d.status, d.code, d.message);
        else r = dispatch(rq, body, remote, who, ctx);
    } catch (const std::exception& e) {  // 처리 실패가 게이트웨이를 멈추지 않게
        GW_ERROR("%s %s failed: %s", rq.method.c_str(), rq.path.c_str(), e.what());
        r = json_error(500, "internal", e.what());
    }
    // 쓰기 요청(GET이 아닌 것, 읽기 명령 빼고)마다 기록 한 줄. 본문·쿼리·헤더는 넣지 않는다(비밀번호·토큰)
    if (rq.method != "GET" && !ctx.read) {
        RecordFields f;
        f.t = iso_now();
        f.user = ctx.user_set ? ctx.user : who.user;
        f.role = role_name(ctx.user_set ? ctx.role : who.role);
        f.mode = mode_at;
        f.remote = remote;
        f.method = rq.method;
        f.path = rq.path;
        f.cmd = ctx.cmd;
        if (!ctx.cmd.empty() && is_worker_command(rq.path)) f.from = ctx.from;
        f.code = r.status;
        f.ms = (mono_now() - t0) * 1000;
        for (const auto& [k, v] : r.headers)
            if (k == "X-Workspace-Advisory") f.advisory = v;
        recorder_->record(std::move(f));
    }
    if (log_enabled(Level::Info)) {
        const std::string q = redact_query(rq.query);
        GW_INFO("%s \"%s %s%s%s\" %d", remote.c_str(), rq.method.c_str(), rq.path.c_str(), q.empty() ? "" : "?",
                q.c_str(), r.status);
    }
    return r;
}

Gateway::Mode Gateway::mode() {
    std::lock_guard<std::mutex> lock(mode_mu_);
    return mode_;
}

namespace {
Json mode_json(const Gateway::Mode& m) {
    Json o = Json::object();
    o["mode"] = m.mode;
    o["since"] = m.since.empty() ? Json(nullptr) : Json(m.since);
    o["by"] = m.by.empty() ? Json(nullptr) : Json(m.by);
    o["fixed"] = true;  // 바꾸는 길 없음(docs/modes.md)
    return o;
}

std::string body_str(const Json& obj, const char* key) {
    if (!obj.is_object()) return "";
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : "";
}
}  // namespace

Reply Gateway::dispatch(const RequestView& rq, const std::function<Body()>& body, const std::string& remote,
                        const Identity& who, Ctx& ctx) {
    const Own o = own_route(rq.method, rq.path);
    if (o != Own::None) return own(o, rq, body, remote, who, ctx);
    return forward(rq, body, ctx);
}

Reply Gateway::own(Own o, const RequestView& rq, const std::function<Body()>& body, const std::string& remote,
                   const Identity& who, Ctx& ctx) {
    const std::string auth = auth_->on() ? "on" : "off";
    switch (o) {
        case Own::Login: {
            ctx.cmd = "login";
            if (!auth_on_) return json_error(400, "bad_request", "authentication is off (WORKSPACE_AUTH=off)");
            Body b = body();
            if (!b.ok) return json_error(400, "bad_request", "invalid JSON body: " + b.error);
            const std::string name = body_str(b.value, "name"), password = body_str(b.value, "password");
            if (name.empty() || password.empty())
                return json_error(400, "bad_request", "body must be {\"name\": \"...\", \"password\": \"...\"}");
            const auto res = auth_on_->login(name, password, remote);
            // 기록: 있는 사용자 이름만(없는 이름 칸에 비밀번호를 잘못 넣었을 수도 있어서)
            ctx.user_set = true;
            ctx.user = res.known_user ? name : "?";
            ctx.role = res.status == 200 ? res.who.role : Role::None;
            if (res.status != 200) {
                Reply r = json_error(res.status, res.code, res.message);
                if (res.status == 429)
                    r.headers.push_back({"Retry-After", std::to_string(static_cast<int>(res.retry_after + 0.999))});
                return r;
            }
            Json out = Json::object();
            out["auth"] = auth;
            out["name"] = res.who.user;
            out["role"] = role_name(res.who.role);
            out["mode"] = mode().mode;
            Reply r = json_reply(200, out);
            // 쿠키: 스크립트가 못 읽게(HttpOnly), 다른 사이트에서 온 요청에는 안 붙게(SameSite=Strict).
            // 브라우저 보관은 절대 만료(7일)까지, 쉼 만료(12시간)는 게이트웨이가 본다
            // HTTPS로 왔으면(nginx가 넣는 X-Forwarded-Proto, docs/https.md) Secure: 평문으로는 쿠키가 안 나감
            r.headers.push_back({"Set-Cookie", "workspace_session=" + res.sid + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
                                                   std::to_string(static_cast<long long>(auth_on_->absolute_s())) + secure_flag(rq)});
            return r;
        }
        case Own::Logout: {
            ctx.cmd = "logout";
            if (auth_on_ && !who.sid.empty()) auth_on_->logout(who.sid);
            Reply r = json_reply(200, Json::object());
            r.headers.push_back({"Set-Cookie", "workspace_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0" + secure_flag(rq)});
            return r;
        }
        case Own::Me: {
            Json out = Json::object();
            out["auth"] = auth;
            if (!auth_on_) {
                // 신분 확인 꺼짐: 누구나 모든 것을 한다(지금 개발 보드)
                out["name"] = nullptr;
                out["role"] = "maintainer";
                out["mode"] = mode().mode;
                return json_reply(200, out);
            }
            if (who.anonymous) {
                Json e = Json{{"error", {{"code", "unauthorized"}, {"message", "login required"}}}};
                e["auth"] = auth;
                return json_reply(401, e);
            }
            out["name"] = who.user;
            out["role"] = role_name(who.role);
            out["mode"] = mode().mode;
            out["via"] = who.via;
            return json_reply(200, out);
        }
        case Own::ModeGet:
            return json_reply(200, mode_json(mode()));
        case Own::MethodNotAllowed: {
            std::string p = starts_with(rq.path, "/api/board/") ? rq.path.substr(10) : rq.path;
            return json_error(405, "method_not_allowed", rq.method + " not allowed on " + p);
        }
        case Own::None:
            break;
    }
    return json_error(404, "not_found", "no route " + rq.path);
}

void Gateway::sync_mode(const Json& system) {
    // gw.mgr 스레드. Manager가 다시 시작돼(운용으로 돌아감) 게이트웨이와 다르면 게이트웨이 값을 다시 알린다
    if (!system.is_object()) return;
    auto it = system.find("access_mode");
    if (it == system.end() || !it->is_string()) return;  // 모드를 모르는 Manager
    const std::string want = mode().mode;
    if (*it == want) return;
    const double now = mono_now();
    if (now - last_mode_push_ < 2.0) return;  // 바꾸는 중(PUT과 엇갈림)이면 다음 주기에
    last_mode_push_ = now;
    try {
        mgr_.call(Json{{"op", "set_access_mode"}, {"mode", want}});
        GW_INFO("manager had access_mode %s; told it %s", it->get<std::string>().c_str(), want.c_str());
    } catch (const ManagerUnavailable& e) {
        GW_DEBUG("set_access_mode resync failed: %s", e.what());
    }
}

Reply Gateway::forward(const RequestView& rq, const std::function<Body()>& body, Ctx& ctx) {
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
            return health(rq.query);
        case RouteKind::ProductOff:
            return json_error(404, "not_found", "not available in product mode");
        case RouteKind::ProductBuild:
            ctx.cmd = "product_build";
            if (m != "GET" && mode().mode != "development") return development_required();  // 관리 조작(command-path 3장)
            return product_build(m, rstrip_char(path, '/'), rt.sub, rq.query, body);
        case RouteKind::Settings:
            return settings(m, body, ctx);
        case RouteKind::Read:
            return read(rt, rq.query);
        case RouteKind::Stream:
            return stream(rt.id, rq.query);
        case RouteKind::Command:
            return command(rt.id, body, ctx);
        case RouteKind::Manager:
            ctx.cmd = rt.op;
            // 쓰기(시작·정지·설정·프리셋·자동 시작·연결 선언)는 개발 모드에서만(docs/command-path.md 3장)
            if (m != "GET" && mode().mode != "development") return development_required();
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
    // 화면이 1초마다 묻는 상태는 캐시가 새로우면 거기서(같은 답 모양). 아니면 지금처럼 Manager에 직접
    if (cfg_.cache) {
        Json cached;
        if ((r.op == "list" && cache_.workers(cached)) || (r.op == "get" && r.has_id && cache_.worker(r.id, cached)) ||
            (r.op == "system" && cache_.system(cached)))
            return json_reply(r.ok_status, cached);
    }
    Json resp;
    try {
        resp = mgr_.call(req);
    } catch (const ManagerUnavailable& e) {
        return json_error(503, "unavailable", std::string("manager unavailable: ") + e.what());
    }
    return manager_reply(resp, r.ok_status);
}

Reply Gateway::health(const std::string& query) {
    std::string manager;
    try {
        manager = py_truthy(get_or(mgr_.call(Json{{"op", "ping"}}), "ok", Json(nullptr))) ? "ok" : "error";
    } catch (const ManagerUnavailable&) {
        manager = "unreachable";
    }
    Json o = Json::object();
    o["rest"] = "ok";
    o["manager"] = manager;
    // ?detail=1: 게이트웨이 속사정(신분 확인·모드·명령 기록·캐시). 기본 답은 지금 REST와 같게 둔다
    const Params p = parse_qs(query);
    auto it = p.find("detail");
    if (it != p.end() && it->second == "1") {
        Json g = Json::object();
        g["auth"] = auth_->on() ? "on" : "off";
        if (auth_on_) g["users"] = auth_on_->state();
        g["mode"] = mode_json(mode());
        g["log"] = file_rec_ ? file_rec_->stats() : Json(nullptr);
        g["cache"] = cfg_.cache ? cache_.state() : Json(nullptr);
        o["gateway"] = g;
    }
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

// ---- 워크스페이스 설정(ui/workspace-screen.md 2장, docs/gateway.md 3장) ----

std::string Gateway::settings_env_path() const { return path_join(path_join(cfg_.data_dir, "config"), "workspace.env"); }

std::string Gateway::settings_def_path() const {
    return path_join(path_join(path_join(cfg_.workspace, "install"), "config"), "workspace-settings.json");
}

Reply Gateway::settings(const std::string& method, const std::function<Body()>& body, Ctx& ctx) {
    if (method == "PUT") ctx.cmd = "settings";
    // 정의는 요청마다 다시 읽는다(작은 파일, 고치면 바로)
    const std::string def_path = settings_def_path();
    Json def;
    std::string err;
    int e = 0;
    const auto def_text = read_file(def_path, 1 << 20, &e);
    if (!def_text) return json_error(500, "internal", "settings definition " + def_path + ": " + errno_text(e));
    if (!settings_parse_def(*def_text, def, err)) return json_error(500, "internal", "settings definition " + def_path + ": " + err);
    const std::string env_path = settings_env_path();

    // 지금 게이트웨이에서 쓰는 값(live 칸) / 시작할 때의 값(restart 칸), 파일 글 모양
    auto start_text = [this](const Json& f) {
        auto it = cfg_.start_env.find(f["key"].get<std::string>());
        return it == cfg_.start_env.end() || it->second.empty() ? settings_default_text(f) : it->second;
    };
    auto effective = [&](const Json& f) -> Json {
        const std::string key = f["key"].get<std::string>();
        if (f["apply"] == "live") {
            if (key == "WORKSPACE_UI_TITLE") return ui_.title();
            if (key == "WORKSPACE_UI_MEDIA_URL") return ui_.media_url();
            if (key == "WORKSPACE_REST_COMMAND_TIMEOUT") return command_timeout_.load();
        }
        return settings_typed(f, start_text(f));
    };

    if (method == "GET") {
        e = 0;
        const auto text = read_file(env_path, 1 << 20, &e);
        if (!text && e != ENOENT) return json_error(500, "internal", env_path + ": " + errno_text(e));
        const auto active = env_active(text.value_or(""));
        Json groups = Json::array(), pending = Json::array();
        for (const auto& g : def["groups"]) {
            Json og = Json::object();
            og["title"] = g.value("title", "");
            Json fields = Json::array();
            for (const auto& f : g["fields"]) {
                const std::string key = f["key"].get<std::string>();
                Json o = f;
                auto it = active.find(key);
                const bool set = it != active.end() && !it->second.empty();
                o["value"] = set ? settings_typed(f, it->second) : Json(nullptr);
                o["effective"] = effective(f);
                // 다시 시작을 기다림: restart 칸의 파일 값(없으면 기본)이 시작할 때의 값과 다름
                const bool wait = f["apply"] == "restart" &&
                                  settings_typed(f, set ? it->second : settings_default_text(f)) != settings_typed(f, start_text(f));
                o["pending"] = wait;
                if (wait) pending.push_back(key);
                fields.push_back(o);
            }
            og["fields"] = fields;
            groups.push_back(og);
        }
        Json out = Json::object();
        out["groups"] = groups;
        out["pending"] = pending;
        out["file"] = env_path;
        out["product"] = cfg_.product;
        return json_reply(200, out);
    }

    // PUT: 제품은 잠금(design.md 11.4), 운용 모드면 관리 쓰기와 같이 막음(docs/modes.md)
    if (cfg_.product) return json_error(403, "forbidden", "settings are locked in product");
    if (mode().mode != "development") return development_required();
    if (cfg_.data_dir.empty()) return json_error(500, "internal", "no data folder (WORKSPACE_DATA)");
    Body b = body();
    if (!b.ok) return json_error(400, "bad_request", "invalid JSON body: " + b.error);
    EnvChanges changes;
    Json fields;
    if (!settings_check(def, b.value, changes, fields, err)) {
        Json eo = Json{{"code", "bad_request"}, {"message", err}};
        if (!fields.empty()) eo["fields"] = fields;
        return json_reply(400, Json{{"error", eo}});
    }
    Json saved = Json::array(), live = Json::array(), restart = Json::array();
    std::lock_guard<std::mutex> lock(settings_mu_);
    e = 0;
    const auto text = read_file(env_path, 1 << 20, &e);
    if (!text && e != ENOENT) return json_error(500, "internal", env_path + ": " + errno_text(e));
    const std::string before = text.value_or("");
    const std::string after = env_edit(before, changes);
    if (after != before) {
        int fmode = 0644;  // 있던 파일의 권한 그대로
        struct stat st{};
        if (::stat(env_path.c_str(), &st) == 0) fmode = static_cast<int>(st.st_mode & 07777);
        ::mkdir(path_join(cfg_.data_dir, "config").c_str(), 0755);
        if (!write_file_atomic(env_path, after, fmode, err)) return json_error(500, "internal", "write " + err);
    }
    for (const auto& ch : changes) {
        const Json& f = *settings_field(def, ch.first);
        saved.push_back(ch.first);
        if (f["apply"] != "live") {
            restart.push_back(ch.first);
            continue;
        }
        live.push_back(ch.first);
        const std::string v = ch.second ? *ch.second : settings_default_text(f);
        if (ch.first == "WORKSPACE_UI_TITLE") {
            ui_.set_title(v.empty() ? "Workspace" : v);
        } else if (ch.first == "WORKSPACE_UI_MEDIA_URL") {
            ui_.set_media_url(v.empty() ? ui_.media_default() : v);  // 비우면 자동(docs/media-proxy.md)
        } else if (ch.first == "WORKSPACE_REST_COMMAND_TIMEOUT") {
            double s = 0;
            if (py_float(v, s) && s > 0) command_timeout_.store(s);
        }
    }
    GW_INFO("settings saved: %s", py_dumps(saved).c_str());
    Json out = Json::object();
    out["saved"] = saved;
    out["live"] = live;
    out["restart"] = restart;
    return json_reply(200, out);
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

Reply Gateway::command(const std::string& id, const std::function<Body()>& body, Ctx& ctx) {
    Body b = body();
    if (!b.ok) return json_error(400, "bad_request", "invalid JSON body: " + b.error);
    if (!b.value.is_object() || !b.value.contains("cmd") || !b.value["cmd"].is_string())
        return json_error(400, "bad_request", "body must be {\"cmd\": \"...\", \"args\": {...}}");
    ctx.cmd = b.value["cmd"].get<std::string>();  // 기록에는 명령 이름만(인자는 넣지 않음)
    ctx.read = kinds_of(id).of(ctx.cmd) == CmdKind::Read;
    // 운용 모드: 읽기·역할 명령만(워커 전용·계약에 없는 명령은 개발 모드에서, docs/command-path.md 3장)
    if (mode().mode != "development" && kinds_of(id).of(ctx.cmd) == CmdKind::Other) return development_required();
    const Json& args = get_or(b.value, "args", Json(nullptr));
    // 읽기가 아닌 명령은 명령 입구(mission-manager)가 있으면 그리로(managers/mission/design.md 2장). 떠 있지 않으면 503(우회 안 함)
    if (kinds_of(id).of(ctx.cmd) != CmdKind::Read) {
        const std::string entry = mission_entry();
        if (!entry.empty() && entry != id) {
            Json mresp;
            try {
                mresp = mgr_.call(Json{{"op", "get_config"}, {"id", entry}});
            } catch (const ManagerUnavailable& e) {
                return json_error(503, "unavailable", std::string("manager unavailable: ") + e.what());
            }
            Json mreq = Json::object();
            mreq["cmd"] = "mission.send";
            mreq["args"] = Json{{"target", id}, {"cmd", b.value["cmd"]}, {"args", py_truthy(args) ? args : Json::object()}};
            mreq["from"] = ctx.from;
            const std::string mpath = socket_path(mresp, "command_socket", cfg_.data_dir, "cmd", entry);
            const CommandResult mr = worker_command(mpath, mreq, 1.0, command_timeout_.load());
            if (mr.kind == CommandResult::NoSocket)
                return json_error(503, "unavailable", entry + " is not running (commands go through it)");
            return finish_command(entry, mr);
        }
    }
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
    request["args"] = py_truthy(args) ? args : Json::object();
    request["from"] = ctx.from;  // 워커는 게이트웨이(믿는 곳)가 붙인 것만 쓴다(docs/command-path.md 5장)
    const CommandResult r = worker_command(path, request, 1.0, command_timeout_.load());
    return finish_command(id, r);
}

Reply Gateway::finish_command(const std::string& id, const CommandResult& r) {
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
    // advisory(docs/modes.md 3장): mission이 "운용이었으면 막힘"을 붙이면 머리 X-Workspace-Advisory로(본문 data는 그대로)
    const Json adv = get_or(r.reply, "advisory", Json(nullptr));
    Reply out;
    if (py_truthy(get_or(r.reply, "ok", Json(nullptr)))) {
        out = json_reply(200, get_or(r.reply, "data", Json(nullptr)));
    } else {
        const Json& err = get_or(r.reply, "error", Json::object());
        Json body{{"error", err}};
        if (adv.is_object()) body["advisory"] = adv;
        out = json_reply(command_status(get_or(err, "code", Json(nullptr))), body);
    }
    if (adv.is_object()) out.headers.push_back({"X-Workspace-Advisory", adv.dump(-1, ' ', true)});
    return out;
}

std::string Gateway::mission_entry() {
    const double now = mono_now();
    {
        std::lock_guard<std::mutex> lock(kinds_mu_);
        if (now - mission_.first < 10.0) return mission_.second;
    }
    std::string found;
    try {
        const Json list = mgr_.call(Json{{"op", "list"}});
        const Json data = get_or(list, "data", Json::array());
        for (const auto& w : data.is_array() ? data : Json::array()) {
            if (get_or(w, "kind", Json(nullptr)) != Json("manager")) continue;  // 매니저만 본다(목록 거르기)
            const std::string wid = py_str(get_or(w, "id", Json("")));
            const Json io = mgr_.call(Json{{"op", "get_io"}, {"id", wid}});
            const Json prov = get_or(get_or(io, "data", Json::object()), "provides", Json::object());
            if (prov.is_object() && prov.contains("mission")) {
                found = wid;
                break;
            }
        }
    } catch (const std::exception&) {
        return "";  // Manager가 없으면 기억하지 않음(뒤에서 어차피 503)
    }
    std::lock_guard<std::mutex> lock(kinds_mu_);
    mission_ = {now, found};
    return found;
}

CommandKinds Gateway::kinds_of(const std::string& id) {
    const double now = mono_now();
    {
        std::lock_guard<std::mutex> lock(kinds_mu_);
        auto it = kinds_.find(id);
        if (it != kinds_.end() && now - it->second.first < 10.0) return it->second.second;
    }
    Json io;  // 계약(Manager get_io). 없으면 null → get_status만 읽기, 나머지는 워커 전용
    try {
        const Json resp = mgr_.call(Json{{"op", "get_io"}, {"id", id}});
        if (py_truthy(get_or(resp, "ok", Json(nullptr)))) io = get_or(resp, "data", Json(nullptr));
    } catch (const std::exception&) {
        return command_kinds(Json());  // Manager가 없으면 기억하지 않음(뒤에서 어차피 503)
    }
    CommandKinds k = command_kinds(io);
    std::lock_guard<std::mutex> lock(kinds_mu_);
    if (kinds_.size() > 256) kinds_.clear();
    kinds_[id] = {now, k};
    return k;
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
