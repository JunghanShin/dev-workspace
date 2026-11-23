#include "auth.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

#include "log.hpp"
#include "routes.hpp"
#include "sys.hpp"

namespace gw {

std::string random_bytes(size_t n) {
    std::string out(n, '\0');
    size_t got = 0;
    while (got < n) {
        const ssize_t r = ::getrandom(&out[got], n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("getrandom: " + errno_text(errno));
        }
        got += static_cast<size_t>(r);
    }
    return out;
}

bool write_file_atomic(const std::string& path, const std::string& data, int mode, std::string& err) {
    const std::string tmp = path + ".tmp." + std::to_string(::getpid());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        err = tmp + ": " + errno_text(errno);
        return false;
    }
    ::fchmod(fd, static_cast<mode_t>(mode));  // umask와 상관없이
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = tmp + ": " + errno_text(errno);
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        err = tmp + ": " + errno_text(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        err = path + ": " + errno_text(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    const std::string dir = path.substr(0, path.rfind('/') == std::string::npos ? 0 : path.rfind('/'));
    const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {  // 이름 바꾸기까지 디스크에(전원이 나가도 옛 파일 또는 새 파일)
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

bool load_users(const std::string& path, UsersDb& out, std::string& err) {
    int e = 0;
    auto text = read_file(path, 1 << 20, &e);
    if (!text) {
        if (e == ENOENT) {
            out = UsersDb{};
            return true;
        }
        err = path + ": " + errno_text(e);
        return false;
    }
    if (strip(*text).empty()) {
        out = UsersDb{};
        return true;
    }
    return parse_users(*text, out, err);
}

// ---- UsersFile ----

namespace {
std::string file_sig(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return errno == ENOENT ? "missing" : "error";
    return std::to_string(st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec) + ":" +
           std::to_string(st.st_size) + ":" + std::to_string(st.st_ino);
}
}  // namespace

UsersFile::UsersFile(std::string path, double check_s) : path_(std::move(path)), check_s_(check_s) {
    db_ = std::make_shared<const UsersDb>();
    reload_if_changed();
    next_check_.store(mono_now() + check_s_);
}

std::shared_ptr<const UsersDb> UsersFile::get() {
    const double now = mono_now();
    double due = next_check_.load();
    // 확인은 주기마다 한 스레드만(나머지는 지금 사본)
    if (now >= due && next_check_.compare_exchange_strong(due, now + check_s_)) reload_if_changed();
    std::lock_guard<std::mutex> lock(mu_);
    return db_;
}

void UsersFile::reload_if_changed() {
    std::unique_lock<std::mutex> rl(reload_mu_, std::try_to_lock);
    if (!rl.owns_lock()) return;
    const std::string sig = file_sig(path_);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (sig == sig_) return;
    }
    UsersDb db;
    std::string err;
    const bool ok = load_users(path_, db, err);  // 잠금 밖에서 읽고 해석
    std::lock_guard<std::mutex> lock(mu_);
    sig_ = sig;
    if (!ok) {
        // 고치는 중에 깨진 파일: 마지막으로 읽은 목록을 그대로 쓴다
        GW_ERROR("users file %s: %s (keeping %zu users from the last good read)", path_.c_str(), err.c_str(),
                 db_->users.size());
        return;
    }
    GW_INFO("users file %s: %zu users", path_.c_str(), db.users.size());
    db_ = std::make_shared<const UsersDb>(std::move(db));
}

// ---- AuthOn ----

AuthOn::AuthOn(AuthConfig cfg)
    : cfg_(std::move(cfg)),
      users_(cfg_.users_file, cfg_.users_check_s),
      sessions_(cfg_.idle_s, cfg_.absolute_s),
      limiter_(cfg_.max_failures, cfg_.lock_s) {}

bool AuthOn::has_users() { return !users_.get()->users.empty(); }

Identity AuthOn::identify(const RequestView& rq) {
    const auto db = users_.get();
    std::string tok = bearer_token(rq.header("Authorization"));
    if (tok.empty() && rq.method == "GET") {  // EventSource는 헤더를 못 붙인다
        const Params q = parse_qs(rq.query);
        auto it = q.find("token");
        if (it != q.end()) tok = it->second;
    }
    Identity who;
    if (!tok.empty()) {
        std::string tname;
        if (const User* u = db->by_token(tok, &tname)) {
            who.user = u->name;
            who.role = u->role;
            who.anonymous = false;
            who.via = "token";
            who.token = tname;
        }
        return who;  // 틀린 토큰이면 쿠키를 보지 않고 익명(뒤섞이지 않게)
    }
    const std::string sid = cookie_value(rq.header("Cookie"), "workspace_session");
    if (sid.empty()) return who;
    const auto s = sessions_.touch(sid, mono_now());
    if (!s) return who;
    const User* u = db->find(s->user);
    if (!u || u->password_hash != s->stamp) {  // 지워졌거나 비밀번호가 바뀐 사용자
        sessions_.remove(sid);
        return who;
    }
    who.user = u->name;
    who.role = u->role;
    who.anonymous = false;
    who.via = "session";
    who.sid = sid;
    return who;
}

Decision AuthOn::authorize(const Identity& who, const RequestView& rq) {
    Decision d;
    const std::string p = starts_with(rq.path, "/api/board/") ? rq.path.substr(10) : rq.path;
    const bool health = rstrip_char(p, '/') == "/health";
    if (cfg_.product && !health && !has_users()) {
        if (!warned_empty_.exchange(true))
            GW_ERROR("product mode: no users in %s; refusing all requests but /health (add one: gateway user add)",
                     users_.path().c_str());
        d.allowed = false;
        d.status = 503;
        d.code = "unavailable";
        d.message = "no users configured on this board (product mode requires login)";
        return d;
    }
    const Need need = classify(rq.method, rq.path, cfg_.ui_enabled);
    if (need == Need::Open) return d;
    if (who.anonymous) {
        d.allowed = false;
        d.status = 401;
        d.code = "unauthorized";
        d.message = "login required (POST /api/auth/login or Authorization: Bearer <token>)";
        return d;
    }
    if (!allows(who.role, need)) {
        d.allowed = false;
        d.status = 403;
        d.code = "forbidden";
        d.message = std::string("role ") + role_name(who.role) + " may not " + need_name(need) +
                    (need == Need::Operate ? " (needs operator)" : need == Need::Maintain ? " (needs maintainer)" : "");
    }
    return d;
}

AuthOn::LoginResult AuthOn::login(const std::string& name, const std::string& password, const std::string& remote) {
    LoginResult r;
    const double now = mono_now();
    const double wait = limiter_.locked_for(remote, now);
    if (wait > 0) {
        r.status = 429;
        r.code = "too_many_requests";
        r.retry_after = wait;
        r.message = "too many failed logins; retry in " + std::to_string(static_cast<int>(wait + 0.999)) + " s";
        return r;
    }
    const auto db = users_.get();
    const User* u = db->find(name);
    r.known_user = u != nullptr;
    bool ok = false;
    if (u && !u->password_hash.empty()) ok = verify_password(u->password_hash, password);
    else burn_password_time(password);  // 없는 이름도 같은 시간(이름을 알아내지 못하게)
    if (!ok) {
        limiter_.failure(remote, now);
        r.status = 401;
        r.code = "unauthorized";
        r.message = "wrong name or password";
        return r;
    }
    limiter_.success(remote);
    r.sid = to_hex(random_bytes(32));
    sessions_.add(r.sid, u->name, u->password_hash, now);
    r.status = 200;
    r.who.user = u->name;
    r.who.role = u->role;
    r.who.anonymous = false;
    r.who.via = "session";
    r.who.sid = r.sid;
    return r;
}

Json AuthOn::state() {
    Json o = Json::object();
    o["users"] = users_.get()->users.size();
    o["sessions"] = sessions_.size(mono_now());
    return o;
}

}  // namespace gw
