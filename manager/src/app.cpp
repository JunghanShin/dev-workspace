#include "app.hpp"

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "log.hpp"
#include "posix_fs.hpp"

namespace pm {

namespace {

const std::set<std::string> kWriteOps = {"set_config", "use_preset", "save_preset", "rename_preset",
                                         "delete_preset", "set_autostart", "start", "stop", "set_connections"};
const std::set<std::string> kConfigOps = {"set_config", "use_preset", "save_preset", "rename_preset",
                                          "delete_preset", "set_autostart", "set_connections"};
constexpr double kCommandsRefreshS = 10.0;  // 워커 계약 다시 읽기 주기(계약 파일은 운용 중 거의 안 바뀜)

Json jget(const Json& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? Json(nullptr) : *it;
}

Json error(const std::string& code, const std::string& message) {
    Json e = Json::object();
    e["code"] = code;
    e["message"] = message;
    Json r = Json::object();
    r["ok"] = false;
    r["error"] = e;
    return r;
}

Json ok(const Json& data) {
    Json r = Json::object();
    r["ok"] = true;
    r["data"] = data;
    return r;
}

Json from_api_error(const ApiError& e) {
    Json r = error(e.code, e.message);
    if (e.fields.is_object() && !e.fields.empty()) r["error"]["fields"] = e.fields;
    return r;
}

double cfg_num(const Json& api, const char* key, double dflt) {
    const Json v = jget(api, key);
    return v.is_number() ? v.get<double>() : dflt;
}

int listen_unix(const std::string& path, std::string& err) {
    ::unlink(path.c_str());  // 이전 실행이 남긴 소켓 파일
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        err = path + ": path too long";
        ::close(fd);
        return -1;
    }
    std::strcpy(addr.sun_path, path.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 128) != 0) {
        err = errno_msg(errno, path);
        ::close(fd);
        return -1;
    }
    ::chmod(path.c_str(), 0600);  // 같은 계정만
    return fd;
}

}  // namespace

App::App(AppConfig cfg, Supervisor& sup, StoreThread& store, Publisher& pub, Resources& res,
         worker::ThreadWatch& watch)
    : cfg_(std::move(cfg)), sup_(sup), store_(store), pub_(pub), res_(res), watch_(watch) {
    loop_slot_ = watch_.add("pm.loop");
    client_names_ = {"rest", "logger"};
    for (const auto& w : cfg_.api_workers) client_names_.push_back(w);  // std::set이라 이름순
    for (const auto& w : cfg_.manager_clients) client_names_.push_back(w);
    sup_.listener = [this](const std::string& e, const std::string& k, const Json& d) { on_event(e, k, d); };
    sup_.inputs_for = [this](const std::string& id) {
        return inputs_res_.inject.contains(id) ? inputs_res_.inject[id] : Json::object();
    };
    sup_.inputs_status = [this](const std::string& id) {
        return inputs_res_.status.contains(id) ? inputs_res_.status[id] : Json::object();
    };
    sup_.many_for = [this](const std::string& id) {
        return inputs_res_.many.contains(id) ? inputs_res_.many[id] : Json::object();
    };
}

App::~App() {
    for (auto& [fd, c] : conns_) ::close(fd);
    for (auto& [fd, n] : listeners_) ::close(fd);
    if (notify_fd_ >= 0) ::close(notify_fd_);
    if (sigfd_ >= 0) ::close(sigfd_);
}

double App::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool App::open(std::string& err) {
    sigset_t mask;
    sigemptyset(&mask);
    for (int s : {SIGCHLD, SIGTERM, SIGINT, SIGHUP}) sigaddset(&mask, s);
    sigfd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd_ < 0) {
        err = std::string("signalfd: ") + std::strerror(errno);
        return false;
    }
    // heartbeat 소켓(9.5): SOCK_DGRAM + SO_PASSCRED. 보낸 프로세스 pid는 커널이 붙이는 SCM_CREDENTIALS로
    ::unlink(cfg_.notify_path.c_str());
    notify_fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    const int one = 1;
    ::setsockopt(notify_fd_, SOL_SOCKET, SO_PASSCRED, &one, sizeof one);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (cfg_.notify_path.size() >= sizeof addr.sun_path) {
        err = cfg_.notify_path + ": path too long";
        return false;
    }
    std::strcpy(addr.sun_path, cfg_.notify_path.c_str());
    if (::bind(notify_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        err = errno_msg(errno, cfg_.notify_path);
        return false;
    }
    ::chmod(cfg_.notify_path.c_str(), 0660);

    // API 소켓. 지금 클라이언트가 아닌 manager-*.sock(예전 이름, 빠진 워커)은 지운다
    std::vector<std::pair<std::string, std::optional<std::string>>> socks = {{cfg_.run_dir + "/manager.sock", std::nullopt}};
    std::set<std::string> keep = {"manager.sock"};
    for (const auto& n : client_names_) {
        socks.emplace_back(cfg_.run_dir + "/manager-" + n + ".sock", n);
        keep.insert("manager-" + n + ".sock");
    }
    if (DIR* d = ::opendir(cfg_.run_dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string f = e->d_name;
            if (f.rfind("manager-", 0) == 0 && f.size() > 5 && f.compare(f.size() - 5, 5, ".sock") == 0 && !keep.count(f)) {
                if (::unlink((cfg_.run_dir + "/" + f).c_str()) == 0) PM_INFO("removed stale socket %s", f.c_str());
                else PM_WARN("cannot remove stale socket %s: %s", f.c_str(), std::strerror(errno));
            }
        }
        ::closedir(d);
    }
    std::string names;
    for (const auto& [path, client] : socks) {
        const int fd = listen_unix(path, err);
        if (fd < 0) return false;
        listeners_.emplace_back(fd, client);
        if (client) names += (names.empty() ? "" : ", ") + std::string("manager-") + *client + ".sock";
    }
    PM_INFO("api listening in %s: manager.sock (read-only), %s", cfg_.run_dir.c_str(), names.c_str());
    // 명령 길: 시작 때 허용 목록을 한 번 만든다(게이트웨이가 붙으면 그 pid를 넣어 다시)
    load_connections(true);
    refresh_commands();
    write_peers(true);
    return true;
}

// ---- 명령 길(docs/command-path.md 4장) ----------------------------------------------------------------

void App::load_connections(bool force) {
    if (cfg_.connections_path.empty()) return;
    struct stat st{};
    const long long mtime = ::stat(cfg_.connections_path.c_str(), &st) == 0
                                ? static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec
                                : 0;
    if (!force && mtime == connections_mtime_) return;
    connections_mtime_ = mtime;
    peers_dirty_ = true;
    if (mtime == 0) {  // 파일 없음: 선언 없음(화면만, 입력은 자동 연결·프리셋)
        connections_ = Json::object();
        connections_error_.clear();
        if (!ios_.empty()) resolve_inputs();
        return;
    }
    std::string text, err;
    PosixFs fs;
    if (fs.read(cfg_.connections_path, text, err) != FileSystem::Read::Ok) {
        connections_error_ = err;
        PM_WARN("connections: cannot read %s: %s (keeping last)", cfg_.connections_path.c_str(), err.c_str());
        return;
    }
    auto j = parse(text, &err);
    std::string shape;
    if (!j || !j->is_object()) shape = j ? "connections.json must be an object" : err;
    else shape = commanders_shape_error(j->value("commanders", Json()));
    if (shape.empty() && j && j->is_object()) shape = connections_shape_error(j->value("connections", Json()));
    if (!shape.empty()) {
        connections_error_ = shape;
        PM_WARN("connections: %s: %s (keeping last)", cfg_.connections_path.c_str(), shape.c_str());
        return;
    }
    connections_ = *j;
    connections_error_.clear();
    PM_INFO("connections: loaded %s", cfg_.connections_path.c_str());
    if (!ios_.empty()) resolve_inputs();
}

void App::refresh_commands() {
    std::map<std::string, WorkerCommands> next;
    std::map<std::string, WorkerIo> ios;
    for (const auto& id : sup_.ids()) {
        Json io;
        try {
            io = sup_.get_io(Json(id));
        } catch (...) {
            io = Json();  // 계약 없음: 읽기(get_status)만, 역할 명령 없음, 입력 없음
        }
        next[id] = commands_from_io(io);
        ios[id] = worker_io_from(io, sup_.config_of(id));
    }
    ios_ = std::move(ios);
    resolve_inputs();
    cmds_at_ = now();
    if (next.size() != cmds_.size() ||
        !std::equal(next.begin(), next.end(), cmds_.begin(), [](const auto& a, const auto& b) {
            return a.first == b.first && a.second.read == b.second.read && a.second.roles == b.second.roles &&
                   a.second.uses == b.second.uses && a.second.operate == b.second.operate;
        })) {
        cmds_ = std::move(next);
        peers_dirty_ = true;
    }
}

void App::write_peers(bool force) {
    if (cfg_.peers_path.empty() || (!force && !peers_dirty_)) return;
    peers_dirty_ = false;
    std::vector<PeerProcess> procs;
    if (gateway_pid_ > 0) procs.push_back({"gateway", gateway_pid_, 0, true});
    for (const auto& [id, pid] : sup_.pids_by_id()) procs.push_back({id, 0, pid, cfg_.command_entries.count(id) > 0});
    // 매니저가 역할 mission을 쓰면(io.json uses) 명령 입구 매니저에 명령할 수 있게 자동으로(요청 방향 fault → mission 등은
    // 플랫폼 규칙이라 사람이 commanders에 적지 않아도 되게, managers/fault/design.md 5장)
    Json commanders = connections_.value("commanders", Json());
    if (!commanders.is_object()) commanders = Json::object();
    for (const auto& [id, c] : cmds_) {
        if (!c.uses.count("mission") || !sup_.is_manager(id)) continue;
        for (const auto& [target, tc] : cmds_) {
            if (target == id || !tc.roles.count("mission")) continue;
            Json& list = commanders[target + ".mission"];
            if (!list.is_array()) list = Json::array();
            if (std::find(list.begin(), list.end(), Json(id)) == list.end()) list.push_back(id);
        }
    }
    // 명령 입구(설치된 command_entry 매니저 중 역할 mission을 내보내는 것, managers/mission/external-link.md)
    std::string entry;
    for (const auto& id : cfg_.command_entries) {
        const auto it = cmds_.find(id);
        if (it != cmds_.end() && it->second.roles.count("mission")) {
            entry = id;
            break;
        }
    }
    Json doc = build_peers(commanders, cmds_, procs, access_mode_, cfg_.command_guard,
                           cfg_.boot_id, entry);
    const std::string text = dump_file(doc);
    if (!force && text == peers_text_) return;
    peers_text_ = text;
    doc["written_at_ms"] = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    FileOp op;
    op.path = cfg_.peers_path;
    op.text = dump_file(doc);
    store_.submit({op}, [this](bool good, const std::string& err) {
        if (!good) {
            PM_WARN("peers.json write failed: %s", err.c_str());
            peers_text_.clear();  // 다음 번에 다시 쓰게
            peers_dirty_ = true;
        }
    });
}

void App::resolve_inputs() {
    InputResolution r = resolve_connections(connections_.value("connections", Json()), ios_);
    const bool changed = r.inject != inputs_res_.inject || r.status != inputs_res_.status || r.many != inputs_res_.many;
    inputs_res_ = std::move(r);
    if (!changed) return;
    for (const auto& e : inputs_res_.errors) PM_WARN("connections: %s", py_str(e).c_str());
    sup_.restart_changed_inputs();  // 넣어 줄 주소가 바뀐 워커만(설정 바뀜과 같음, 6.6.7.4)
    sup_.rewrite_changed_many();    // 여러 워커 입력 목록만 바뀐 워커: 파일만(docs/managers.md 5장)
}

Json App::connections_view() {
    Json out = Json::object();
    out["path"] = cfg_.connections_path;
    out["connections"] = connections_;
    out["error"] = connections_error_.empty() ? Json(nullptr) : Json(connections_error_);
    out["check"] = check_commanders(connections_.value("commanders", Json()), cmds_);
    out["inputs"] = inputs_res_.status;  // 워커 → 입력 → {from, how, ok, error, required}
    out["errors"] = inputs_res_.errors;
    out["guard"] = cfg_.command_guard;
    out["access_mode"] = access_mode_;
    return out;
}

// set_connections: 검사하고 파일 쓰기 → 끝나면 표 다시 쓰고 답. 틀리면 바로 답할 오류
std::optional<Json> App::plan_set_connections(int fd, const Json& req) {
    const Json c = jget(req, "connections");
    if (!c.is_object()) return error("bad_request", "connections must be an object (the whole connections.json)");
    std::string shape = commanders_shape_error(c.value("commanders", Json()));
    if (shape.empty()) shape = connections_shape_error(c.value("connections", Json()));
    if (!shape.empty()) return error("bad_request", shape);
    if (cfg_.connections_path.empty()) return error("unavailable", "no connections file path");
    write_busy_ = true;
    FileOp op;
    op.path = cfg_.connections_path;
    op.text = dump_file(c);
    op.sync_dir = true;
    store_.submit({op}, [this, fd, c](bool good, const std::string& err) {
        if (!good) {
            finish_write(fd, error("internal", "connections write failed: " + err));
            return;
        }
        load_connections(true);
        refresh_commands();
        write_peers(true);
        finish_write(fd, ok(connections_view()));
    });
    return std::nullopt;
}

// ---- 상태·이벤트 --------------------------------------------------------------------------------

void App::on_event(const std::string& event, const std::string& key, const Json& data) {
    if (subs_.empty()) return;  // 구독자가 없으면 줄을 만들지 않는다
    Json obj = Json::object();
    obj["event"] = event;
    obj["data"] = data;
    pub_.offer({"workers", event + '\x1f' + key, dump_line(obj) + "\n"});
}

Json App::system_status() {
    const double t = now();
    std::set<std::string> subscribed;
    for (const auto& [id, s] : subs_)
        if (s.client) subscribed.insert(*s.client);
    Json clients = Json::object();
    for (const auto& n : client_names_) {
        if (subscribed.count(n)) clients[n] = "subscribed";
        else if (last_seen_.count(n) && t - last_seen_[n] < 10.0) clients[n] = "idle";
        else clients[n] = "disconnected";
    }
    Json out = Json::object();
    out["clients"] = clients;
    Json apiw = Json::array();
    for (const auto& w : cfg_.api_workers) apiw.push_back(w);
    out["manager_api_workers"] = apiw;
    out["product"] = cfg_.product;
    out["config_write"] = cfg_.config_write;
    out["access_mode"] = access_mode_;
    return out;
}

Json App::set_access_mode(const Json& req, const std::optional<std::string>& client) {
    // 운용/정비 모드(원칙 17): 주인은 게이트웨이. 여기서는 받아 적기만 하고 저장하지 않는다(다시 시작하면 운용)
    if (!client || *client != "rest") return error("forbidden", "set_access_mode is allowed only from manager-rest.sock");
    const Json mode = jget(req, "mode");
    if (!mode.is_string() || (mode != "operation" && mode != "development"))
        return error("bad_request", "mode must be 'operation' or 'development'");
    const std::string m = mode.get<std::string>();
    if (m != access_mode_) {
        PM_INFO("access mode %s -> %s", access_mode_.c_str(), m.c_str());
        access_mode_ = m;
        peers_dirty_ = true;
        write_peers();  // 허용 명령이 모드에 따라 바뀜(운용: 역할 명령만, 개발: 전부)
        if (!subs_.empty()) {  // 구독자에게 바로(주기를 기다리지 않음)
            Json obj = Json::object();
            obj["event"] = "system";
            obj["data"] = system_status();
            pub_.offer({"system", "system", dump_line(obj) + "\n"});
        }
    }
    Json d = Json::object();
    d["access_mode"] = access_mode_;
    return ok(d);
}

void App::record_command(const Json& req, const std::optional<std::string>& client, const Json& resp) {
    // 쓰기 명령 이력(topic "commands"). 키가 매번 달라 최신값 덮어쓰기에서 빠지지 않는다
    ++command_seq_;
    if (subs_.empty()) return;
    const std::string op = req["op"].get<std::string>();
    Json data = Json::object();
    data["op"] = op;
    data["id"] = jget(req, "id");
    data["client"] = client ? Json(*client) : Json(nullptr);
    data["ok"] = resp["ok"];
    if (op == "set_config") {
        data["config"] = jget(req, "config");
        data["save"] = !(req.contains("save") && req["save"] == Json(false));
    } else if (op == "set_autostart") {
        data["autostart"] = jget(req, "autostart");
    } else if (op == "set_access_mode") {
        data["mode"] = jget(req, "mode");
    } else if (op == "use_preset" || op == "rename_preset" || op == "delete_preset" || op == "save_preset") {
        const Json name = jget(req, "name");
        const Json preset = jget(req, "preset");
        data["preset"] = py_truthy(name) ? name : (preset.is_object() ? jget(preset, "name") : Json(nullptr));
    }
    if (!resp["ok"].get<bool>()) data["error"] = resp["error"];
    Json obj = Json::object();
    obj["event"] = "command";
    obj["data"] = data;
    pub_.offer({"commands", "command\x1f" + std::to_string(command_seq_), dump_line(obj) + "\n"});
}

// ---- 연결 ---------------------------------------------------------------------------------------

void App::accept_on(int lfd, const std::optional<std::string>& client) {
    while (true) {
        const int fd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        if (conns_.size() >= 256) {  // 너무 많은 연결: 루프를 지키려고 바로 닫는다
            ::close(fd);
            continue;
        }
        if (client) last_seen_[*client] = now();
        if (client && *client == "rest") {  // 게이트웨이 pid는 커널이 알려 준다(따로 등록하지 않음)
            ucred cr{};
            socklen_t len = sizeof cr;
            if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0 && cr.pid > 0 && cr.pid != gateway_pid_) {
                PM_INFO("gateway pid %d", static_cast<int>(cr.pid));
                gateway_pid_ = cr.pid;
                peers_dirty_ = true;
                write_peers();
            }
        }
        Conn c;
        c.fd = fd;
        c.client = client;
        c.deadline = now() + cfg_num(cfg_.api, "read_timeout_s", 1.0);
        conns_[fd] = std::move(c);
    }
}

void App::close_conn(int fd) {
    ::close(fd);
    conns_.erase(fd);
}

void App::read_conn(Conn& c) {
    const size_t limit = static_cast<size_t>(cfg_num(cfg_.api, "max_message_bytes", 65536));
    char buf[16384];
    while (true) {
        const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_conn(c.fd);
            return;
        }
        if (n == 0) {
            // 줄바꿈 없이 끝남: 받은 것을 한 건으로(빈 것이면 닫음)
            if (c.in.empty() || c.in.size() > limit) {
                close_conn(c.fd);
                return;
            }
            const std::string line = c.in;
            handle_line(c, line);
            return;
        }
        c.in.append(buf, static_cast<size_t>(n));
        const size_t nl = c.in.find('\n');
        if (nl != std::string::npos) {
            if (nl > limit) {  // 크기 상한 초과: 답 없이 닫는다
                close_conn(c.fd);
                return;
            }
            const std::string line = c.in.substr(0, nl + 1);
            handle_line(c, line);
            return;
        }
        if (c.in.size() > limit) {
            close_conn(c.fd);
            return;
        }
    }
}

void App::reply(Conn& c, const Json& resp) {
    c.out = dump_line(resp) + "\n";
    c.st = Conn::St::Writing;
    c.deadline = now() + cfg_num(cfg_.api, "read_timeout_s", 1.0);
    write_conn(c);
}

void App::write_conn(Conn& c) {
    while (!c.out.empty()) {
        const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.out.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;  // 나머지는 POLLOUT 때
        break;
    }
    close_conn(c.fd);
}

void App::handle_line(Conn& c, const std::string& line) {
    std::string err;
    auto req = parse(line, &err);
    if (!req || !req->is_object() || !jget(*req, "op").is_string()) {
        reply(c, error("bad_request", "invalid request: " + (req ? std::string("op missing") : err)));
        return;
    }
    const std::string op = (*req)["op"].get<std::string>();
    if (op == "subscribe") {
        subscribe(c, *req);
        return;
    }
    if (op == "set_access_mode") {  // 파일을 쓰지 않으니 쓰기 차례(pm.store)를 거치지 않고 바로
        const Json resp = set_access_mode(*req, c.client);
        record_command(*req, c.client, resp);
        reply(c, resp);
        return;
    }
    if (kWriteOps.count(op)) {
        if (auto denied = check_write(*req, c.client)) {
            record_command(*req, c.client, *denied);
            reply(c, *denied);
            return;
        }
        c.req = *req;
        c.st = Conn::St::Queued;
        write_fifo_.push_back(c.fd);
        pump_writes();
        return;
    }
    reply(c, dispatch_read(*req));
}

Json App::dispatch_read(const Json& req) {
    const std::string op = req["op"].get<std::string>();
    const Json id = jget(req, "id");
    try {
        if (op == "ping") {
            Json d = Json::object();
            d["pong"] = true;
            return ok(d);
        }
        if (op == "system") return ok(system_status());
        if (op == "list") return ok(sup_.list());
        if (op == "get") return ok(sup_.get(id));
        if (op == "get_config") return ok(sup_.get_config(id));
        if (op == "get_schema") return ok(sup_.get_schema(id));
        if (op == "get_io") return ok(sup_.get_io(id));
        if (op == "get_connections") return ok(connections_view());
        return error("bad_request", "unknown op " + py_quote(op));
    } catch (const ApiError& e) {
        return from_api_error(e);
    } catch (const std::exception& e) {
        PM_ERROR("op %s failed: %s", op.c_str(), e.what());
        return error("internal", e.what());
    }
}

std::optional<Json> App::check_write(const Json& req, const std::optional<std::string>& client) {
    const std::string op = req["op"].get<std::string>();
    if (!client || *client == "logger" || cfg_.manager_clients.count(*client))
        return error("forbidden", (client ? *client : "manager.sock") + " is read-only");
    // 제품 모드: 설정 잠금(시작·정지는 됨), 가짜 장치 설정 금지(design.md 11.4)
    if (kConfigOps.count(op) && !cfg_.config_write) return error("forbidden", "settings are locked (product)");
    if (cfg_.product && (op == "set_config" || op == "save_preset")) {
        const Json preset = jget(req, "preset");
        const Json cfg = op == "set_config" ? jget(req, "config") : jget(preset, "config");
        if (cfg.is_object() && cfg.contains("sim")) return error("bad_request", "sim (fake device) not allowed in product");
    }
    // 매니저는 운용 모드에서 정지·설정 바꾸기를 막는다(docs/managers.md 9장 2). 다시 켜기(start)는 됨. 모든 클라이언트에
    if (op != "start" && op != "set_connections" && jget(req, "id").is_string() && access_mode_ != "development" &&
        sup_.is_manager(jget(req, "id").get<std::string>())) {
        Json e = error("forbidden", "managers can be stopped or reconfigured only in development mode");
        e["need"] = "development";
        return e;
    }
    // Manager API를 쓰는 워커는 자기 자신을 정지·설정하지 못한다(스스로 끊기지 않게)
    if (cfg_.api_workers.count(*client) && jget(req, "id") == Json(*client))
        return error("forbidden", "worker " + *client + " cannot control itself");
    // 모듈 운용 매니저(managers/payload/design.md 3장): 매니저는 못 건드리고, 워커 start·stop은 운용 모드에서도 된다
    if (cfg_.module_controls.count(*client)) {
        const Json id = jget(req, "id");
        if (id.is_string() && sup_.is_manager(id.get<std::string>()))
            return error("forbidden", *client + " cannot start or stop managers");
        if (op == "start" || op == "stop") return std::nullopt;
    }
    // 운용 모드에서는 관리 조작(시작·정지·설정·연결 선언)을 개발 모드에서만(docs/command-path.md 3·5장 ③).
    // 화면의 같은 요청은 게이트웨이가 먼저 막는다. 게이트웨이(rest)는 사용자 역할과 함께 직접 판단하므로 여기서 보지 않음
    if (cfg_.api_workers.count(*client) && access_mode_ != "development") {
        Json e = error("forbidden", "development mode required");
        e["need"] = "development";
        return e;
    }
    return std::nullopt;
}

void App::pump_writes() {
    while (!write_busy_ && !write_fifo_.empty()) {
        const int fd = write_fifo_.front();
        write_fifo_.pop_front();
        if (conns_.count(fd)) run_write(fd);
    }
}

void App::run_write(int fd) {
    Conn& c = conns_.at(fd);
    c.st = Conn::St::Busy;
    const Json& req = c.req;
    const std::string op = req["op"].get<std::string>();
    const Json id = jget(req, "id");
    if (op == "set_connections") {  // 워커 상태가 아니라 설정 파일 하나: Supervisor를 거치지 않음
        if (auto bad = plan_set_connections(fd, req)) finish_write(fd, *bad);
        return;
    }
    OpPlan plan;
    try {
        if (op == "set_config")
            plan = sup_.set_config(id, jget(req, "config"), !(req.contains("save") && req["save"] == Json(false)));
        else if (op == "use_preset") plan = sup_.use_preset(id, jget(req, "name"));
        else if (op == "save_preset") plan = sup_.save_preset(id, jget(req, "preset"));
        else if (op == "rename_preset") plan = sup_.rename_preset(id, jget(req, "name"), jget(req, "preset"));
        else if (op == "delete_preset") plan = sup_.delete_preset(id, jget(req, "name"));
        else if (op == "set_autostart") plan = sup_.set_autostart(id, jget(req, "autostart"));
        else if (op == "start") plan = sup_.start(id);
        else plan = sup_.stop(id);
    } catch (const ApiError& e) {
        finish_write(fd, from_api_error(e));
        return;
    } catch (const std::exception& e) {
        PM_ERROR("op %s failed: %s", op.c_str(), e.what());
        finish_write(fd, error("internal", e.what()));
        return;
    }
    write_busy_ = true;
    auto commit = std::make_shared<std::function<Json()>>(std::move(plan.commit));
    auto apply = [this, fd, commit]() {
        Json data;
        try {
            data = (*commit)();
        } catch (const std::exception& e) {
            finish_write(fd, error("internal", e.what()));
            return;
        }
        // 상태를 바꾸며 낸 실행 기록 쓰기(desired.json 등)까지 끝난 뒤 답한다: 답을 받은 뒤 Manager가
        // 다시 시작돼도 그 명령이 남아 있게. pm.store는 낸 순서대로 하므로 빈 요청이 끝나면 앞 것도 끝난 것
        store_.submit({}, [this, fd, data](bool, const std::string&) { finish_write(fd, ok(data)); });
    };
    if (plan.pre.empty()) {
        apply();
        return;
    }
    store_.submit(std::move(plan.pre), [this, fd, apply](bool good, const std::string& err) {
        if (!good) finish_write(fd, error("internal", "worker folder write failed: " + err));
        else apply();
    });
}

void App::finish_write(int fd, const Json& resp) {
    write_busy_ = false;
    auto it = conns_.find(fd);
    if (it != conns_.end()) {
        record_command(it->second.req, it->second.client, resp);
        reply(it->second, resp);
    }
    sup_.tick();  // start·설정 변경이 바로 반영되게(다음 tick을 기다리지 않음)
    pump_writes();
}

// ---- 구독 ---------------------------------------------------------------------------------------

void App::subscribe(Conn& c, const Json& req) {
    std::set<std::string> topics;
    const Json t = jget(req, "topics");
    if (t.is_array()) {
        for (const auto& x : t)
            if (x.is_string()) topics.insert(x.get<std::string>());
    } else if (t.is_string()) {
        for (char ch : t.get<std::string>()) topics.insert(std::string(1, ch));  // Python set("abc")와 같음
    }
    if (!py_truthy(t) || topics.empty()) topics = {"workers", "system"};
    double period = cfg_num(cfg_.api, "subscribe_period_ms", 1000) / 1000.0;
    const Json p = req.contains("period_ms") ? req["period_ms"] : Json(cfg_num(cfg_.api, "subscribe_period_ms", 1000));
    if (p.is_number() || p.is_boolean()) {
        period = (p.is_boolean() ? (p.get<bool>() ? 1.0 : 0.0) : p.get<double>()) / 1000.0;
    } else if (p.is_string()) {
        char* end = nullptr;
        const double v = std::strtod(p.get<std::string>().c_str(), &end);
        if (end && *end == '\0' && !p.get<std::string>().empty()) period = v / 1000.0;
    }
    period = std::max(0.1, period);
    Json sorted = Json::array();
    for (const auto& x : topics) sorted.push_back(x);  // std::set이라 정렬됨
    Json d = Json::object();
    d["subscribed"] = sorted;
    const int id = next_sub_++;
    const int fd = c.fd;
    const auto client = c.client;
    conns_.erase(fd);  // fd는 pm.pub으로 넘어간다(닫지 않음)
    if (!pub_.add(id, fd, topics, dump_line(ok(d)) + "\n")) {
        ::close(fd);
        return;
    }
    subs_[id] = Sub{client, period, 0.0};  // 바로 전체 상태부터
}

void App::send_snapshots(double t) {
    std::vector<int> due;
    for (auto& [id, s] : subs_)
        if (t >= s.next) {
            due.push_back(id);
            s.next = t + s.period;
        }
    if (due.empty()) return;
    std::vector<Publisher::Item> items;
    for (const auto& st : sup_.list()) {
        Json obj = Json::object();
        obj["event"] = "worker_status";
        obj["data"] = st;
        items.push_back({"workers", "worker_status\x1f" + st["id"].get<std::string>(), dump_line(obj) + "\n"});
    }
    Json obj = Json::object();
    obj["event"] = "system";
    obj["data"] = system_status();
    items.push_back({"system", "system", dump_line(obj) + "\n"});
    pub_.offer_to(std::move(due), std::move(items));
}

// ---- heartbeat·신호 -----------------------------------------------------------------------------

void App::read_notify() {
    while (true) {
        char buf[4096];
        char cbuf[CMSG_SPACE(sizeof(ucred))];
        iovec iov{buf, sizeof buf};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof cbuf;
        const ssize_t n = ::recvmsg(notify_fd_, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        int pid = -1;
        for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_CREDENTIALS) {
                ucred cr{};
                std::memcpy(&cr, CMSG_DATA(cm), sizeof cr);
                pid = cr.pid;
            }
        if (pid < 0) continue;
        // "KEY=VALUE KEY2=VALUE2 FLAG" → {"KEY": "VALUE", ..., "FLAG": ""}
        Json fields = Json::object();
        const std::string text(buf, static_cast<size_t>(n));
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            size_t j = i;
            while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j]))) ++j;
            if (j > i) {
                const std::string tok = text.substr(i, j - i);
                const size_t eq = tok.find('=');
                if (eq == std::string::npos) fields[tok] = "";
                else fields[tok.substr(0, eq)] = tok.substr(eq + 1);
            }
            i = j;
        }
        try {
            sup_.on_notify(pid, fields);
        } catch (const std::exception& e) {
            PM_ERROR("notify handler failed: %s", e.what());
        }
    }
}

bool App::read_signals() {
    bool stop = false;
    signalfd_siginfo si;
    while (::read(sigfd_, &si, sizeof si) == static_cast<ssize_t>(sizeof si)) {
        if (si.ssi_signo == SIGCHLD) continue;  // reap()이 처리
        if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) stop = true;
        else PM_INFO("signal %u ignored", si.ssi_signo);
    }
    reap();
    return stop;
}

void App::reap() {
    while (true) {
        int st = 0;
        const pid_t pid = ::waitpid(-1, &st, WNOHANG);
        if (pid <= 0) return;
        int rc;
        if (WIFEXITED(st)) rc = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) rc = -WTERMSIG(st);
        else continue;
        sup_.on_exit(pid, rc);
    }
}

void App::housekeeping(double t) {
    if (t - last_house_ < 1.0) return;
    last_house_ = t;
    std::vector<int> pgids = sup_.running_pids();
    if (pgids != last_pgids_) {
        res_.set_pgids(pgids);
        last_pgids_ = pgids;
        peers_dirty_ = true;  // 허용 목록의 프로세스 그룹이 바뀜
    }
    // 명령 길: 연결 선언이 손으로·프로필로 바뀌었나, 계약을 다시 읽을 때인가 → 바뀌었으면 peers.json
    load_connections(false);
    if (t - cmds_at_ >= kCommandsRefreshS) refresh_commands();
    write_peers();
    // 로그 회전(크기만 보고 복사 후 비움). 파일 일이라 pm.store가, 큐가 비어 있을 때만
    FileOp op;
    op.kind = FileOp::Kind::RotateLogs;
    op.path = cfg_.logs_dir;
    op.max_bytes = cfg_.log_max_bytes;
    op.backups = cfg_.log_backups;
    store_.submit_idle(op);
}

// ---- 루프 ---------------------------------------------------------------------------------------

void App::step(int timeout_ms, bool serve_api) {
    std::vector<pollfd> fds;
    fds.push_back({sigfd_, POLLIN, 0});
    fds.push_back({notify_fd_, POLLIN, 0});
    fds.push_back({store_.wake_fd(), POLLIN, 0});
    fds.push_back({res_.wake_fd(), POLLIN, 0});
    fds.push_back({pub_.dropped_fd(), POLLIN, 0});
    const size_t base = fds.size();
    std::vector<std::pair<int, size_t>> lsn, cns;  // (fd, listeners_ 자리)
    if (serve_api) {
        for (size_t i = 0; i < listeners_.size(); ++i) {
            fds.push_back({listeners_[i].first, POLLIN, 0});
            lsn.emplace_back(listeners_[i].first, i);
        }
        for (auto& [fd, c] : conns_) {
            short ev = c.st == Conn::St::Reading ? POLLIN : c.st == Conn::St::Writing ? POLLOUT : 0;
            fds.push_back({fd, ev, 0});
        }
    }
    double t = now();
    // 대기 상한: 연결 시간 제한, 구독 주기, pm.store 시간 제한 중 가장 이른 것
    double wake = t + timeout_ms / 1000.0;
    for (const auto& [fd, c] : conns_)
        if (c.st == Conn::St::Reading || c.st == Conn::St::Writing) wake = std::min(wake, c.deadline);
    for (const auto& [id, s] : subs_) wake = std::min(wake, s.next);
    if (store_.next_deadline() > 0) wake = std::min(wake, store_.next_deadline());
    if (store_.has_deferred()) wake = t;
    const int ms = std::max(0, static_cast<int>((wake - t) * 1000.0 + 0.999));
    ::poll(fds.data(), fds.size(), ms);
    watch_.beat(loop_slot_);
    t = now();

    if (fds[0].revents & POLLIN) stopping_ = read_signals() || stopping_;
    if (fds[1].revents & POLLIN) read_notify();
    store_.poll(t);
    std::map<int, Json> res;
    if (res_.take(res)) sup_.apply_resources(res);
    for (int id : pub_.take_dropped()) subs_.erase(id);
    if (serve_api) {
        for (size_t i = 0; i < lsn.size(); ++i)
            if (fds[base + i].revents & POLLIN) accept_on(listeners_[lsn[i].second].first, listeners_[lsn[i].second].second);
        for (size_t i = base + lsn.size(); i < fds.size(); ++i) {
            auto it = conns_.find(fds[i].fd);
            if (it == conns_.end() || !fds[i].revents) continue;
            if (it->second.st == Conn::St::Reading) read_conn(it->second);
            else if (it->second.st == Conn::St::Writing) write_conn(it->second);
        }
        // 시간 제한: 느린·비정상 클라이언트가 Manager를 붙잡지 못하게
        std::vector<int> expired;
        for (const auto& [fd, c] : conns_)
            if ((c.st == Conn::St::Reading || c.st == Conn::St::Writing) && t >= c.deadline) expired.push_back(fd);
        for (int fd : expired) close_conn(fd);
        pump_writes();
    }
}

int App::run() {
    double last_tick = 0;
    const int tick_ms = std::max(1, static_cast<int>(cfg_.tick_s * 1000));
    while (!stopping_) {
        step(tick_ms, true);
        const double t = now();
        if (t - last_tick >= cfg_.tick_s * 0.5) {
            sup_.tick();
            last_tick = t;
        }
        housekeeping(t);
        send_snapshots(t);
        std::string who;
        if (!watch_.all_alive(worker::ThreadWatch::Clock::now(), &who)) {
            PM_ERROR("thread stalled: %s; exiting", who.c_str());
            ::_exit(4);
        }
    }
    PM_INFO("shutting down");
    for (auto& [fd, n] : listeners_) ::close(fd);
    listeners_.clear();
    // 워커 정리. desired_state는 그대로 두어 다시 시작하면 복원.
    // 순서대로 끈다: 워커 → 매니저 → record-manager(stop_order, 마지막까지 기록). 순서마다 그 stop_timeout까지 기다림
    for (int order : sup_.stop_orders()) {
        const double deadline = now() + sup_.shutdown_begin(order);
        while (sup_.any_running(order) && now() < deadline) step(100, false);
    }
    sup_.shutdown_kill();
    const double end = now() + 2.0;
    while (sup_.any_running() && now() < end) step(100, false);
    // 남은 응답(쓰기 중인 연결)은 닫는다
    std::vector<int> fds;
    for (const auto& [fd, c] : conns_) fds.push_back(fd);
    for (int fd : fds) close_conn(fd);
    return 0;
}

}  // namespace pm
