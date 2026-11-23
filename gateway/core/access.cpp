#include "access.hpp"

#include <algorithm>
#include <cmath>

#include "crypto.hpp"
#include "text.hpp"

namespace gw {

bool parse_role(const std::string& s, Role& out) {
    if (s == "viewer") out = Role::Viewer;
    else if (s == "operator") out = Role::Operator;
    else if (s == "maintainer") out = Role::Maintainer;
    else return false;
    return true;
}

const char* role_name(Role r) {
    switch (r) {
        case Role::Viewer: return "viewer";
        case Role::Operator: return "operator";
        case Role::Maintainer: return "maintainer";
        case Role::None: break;
    }
    return "-";
}

// ---- 비밀번호 해시 ----

std::string make_password_hash(const std::string& password, const std::string& salt, uint32_t iterations) {
    return "pbkdf2-sha256$" + std::to_string(iterations) + "$" + base64_encode(salt) + "$" +
           base64_encode(pbkdf2_sha256(password, salt, iterations, kHashBytes));
}

bool parse_password_hash(const std::string& s, PasswordHash& out) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t d = s.find('$', start);
        parts.push_back(s.substr(start, d == std::string::npos ? std::string::npos : d - start));
        if (d == std::string::npos) break;
        start = d + 1;
    }
    if (parts.size() != 4 || parts[0] != "pbkdf2-sha256" || parts[1].empty() || parts[1].size() > 9) return false;
    uint32_t it = 0;
    for (char c : parts[1]) {
        if (c < '0' || c > '9') return false;
        it = it * 10 + static_cast<uint32_t>(c - '0');
    }
    if (it < kMinIterations || it > kMaxIterations) return false;
    PasswordHash h;
    h.iterations = it;
    if (!base64_decode(parts[2], h.salt) || h.salt.size() < 8) return false;
    if (!base64_decode(parts[3], h.hash) || h.hash.size() != kHashBytes) return false;
    out = std::move(h);
    return true;
}

bool verify_password(const std::string& stored, const std::string& password) {
    PasswordHash h;
    if (!parse_password_hash(stored, h)) return false;
    return constant_time_equal(pbkdf2_sha256(password, h.salt, h.iterations, h.hash.size()), h.hash);
}

void burn_password_time(const std::string& password) {
    (void)pbkdf2_sha256(password, "0123456789abcdef", kPbkdf2Iterations, kHashBytes);
}

// ---- 사용자 파일 ----

const User* UsersDb::find(const std::string& name) const {
    for (const auto& u : users)
        if (u.name == name) return &u;
    return nullptr;
}

User* UsersDb::find(const std::string& name) {
    for (auto& u : users)
        if (u.name == name) return &u;
    return nullptr;
}

const User* UsersDb::by_token(const std::string& token, std::string* token_name) const {
    if (token.empty()) return nullptr;
    const std::string h = sha256_hex(token);
    for (const auto& u : users)
        for (const auto& t : u.tokens)
            if (constant_time_equal(t.hash, h)) {
                if (token_name) *token_name = t.name;
                return &u;
            }
    return nullptr;
}

bool valid_user_name(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!(alnum || (i > 0 && (c == '_' || c == '.' || c == '-')))) return false;
    }
    return true;
}

bool valid_token_name(const std::string& s) { return valid_user_name(s); }

namespace {
bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
}  // namespace

bool parse_users(const std::string& text, UsersDb& out, std::string& err) {
    auto j = parse_json(text, &err);
    if (!j) return false;
    if (!j->is_object() || !j->contains("users") || !(*j)["users"].is_array()) {
        err = "top level must be {\"users\": [...]}";
        return false;
    }
    UsersDb db;
    for (const auto& u : (*j)["users"]) {
        if (!u.is_object()) {
            err = "user entry must be an object";
            return false;
        }
        User x;
        const Json name = u.value("name", Json(nullptr)), role = u.value("role", Json(nullptr));
        if (!name.is_string() || !valid_user_name(name.get<std::string>())) {
            err = "bad user name";
            return false;
        }
        x.name = name.get<std::string>();
        if (db.find(x.name)) {
            err = "duplicate user " + x.name;
            return false;
        }
        if (!role.is_string() || !parse_role(role.get<std::string>(), x.role)) {
            err = "user " + x.name + ": role must be viewer, operator or maintainer";
            return false;
        }
        const Json ph = u.value("password_hash", Json(""));
        if (!ph.is_string()) {
            err = "user " + x.name + ": password_hash must be a string";
            return false;
        }
        x.password_hash = ph.get<std::string>();  // 비어 있으면 비밀번호 로그인 없음(토큰만)
        PasswordHash tmp;
        if (!x.password_hash.empty() && !parse_password_hash(x.password_hash, tmp)) {
            err = "user " + x.name + ": bad password_hash";
            return false;
        }
        const Json toks = u.value("tokens", Json::array());
        if (!toks.is_array()) {
            err = "user " + x.name + ": tokens must be a list";
            return false;
        }
        for (const auto& t : toks) {
            if (!t.is_object() || !t.value("name", Json(nullptr)).is_string() || !t.value("hash", Json(nullptr)).is_string() ||
                !is_hex64(t["hash"].get<std::string>())) {
                err = "user " + x.name + ": token must be {\"name\", \"hash\": <sha256 hex>}";
                return false;
            }
            x.tokens.push_back({t["name"].get<std::string>(), t["hash"].get<std::string>()});
        }
        db.users.push_back(std::move(x));
    }
    out = std::move(db);
    return true;
}

std::string dump_users(const UsersDb& db) {
    Json users = Json::array();
    for (const auto& u : db.users) {
        Json toks = Json::array();
        for (const auto& t : u.tokens) toks.push_back(Json{{"name", t.name}, {"hash", t.hash}});
        users.push_back(Json{{"name", u.name}, {"role", role_name(u.role)}, {"password_hash", u.password_hash}, {"tokens", toks}});
    }
    return Json{{"users", users}}.dump(2) + "\n";
}

// ---- 경로 → 권한 ----

const char* need_name(Need n) {
    switch (n) {
        case Need::Open: return "open";
        case Need::Read: return "read";
        case Need::Operate: return "operate";
        case Need::Maintain: return "maintain";
    }
    return "?";
}

bool open_path(const std::string& path, bool ui_enabled) {
    const std::string t = rstrip_char(path, '/');
    if (t == "/health" || t == "/api/board/health") return true;
    if (!ui_enabled) return false;
    if (path == "/" || path == "/index.html") return true;
    // 화면 파일 자체에는 데이터가 없다. 데이터는 /api·REST 경로에서 본다
    for (const char* p : {"/api/", "/workers", "/system", "/logs", "/records", "/history"})
        if (starts_with(path, p)) return false;
    for (const char* e : {".html", ".js", ".css", ".svg", ".png", ".ico", ".woff2", ".json"})
        if (ends_with(path, e)) return true;
    return false;
}

namespace {
// /api/board 접두어와 /api를 떼고 끝의 / 를 뗀 경로(게이트웨이 전용 경로를 맞출 때)
std::string own_path(const std::string& path) {
    std::string p = starts_with(path, "/api/board/") ? path.substr(10) : path;
    p = rstrip_char(p, '/');
    if (starts_with(p, "/api/")) p = p.substr(4);
    return p;
}

bool is_command(const std::string& path) {
    // /workers/<id>/command (끝의 / 하나 허용)
    std::string p = starts_with(path, "/api/board/") ? path.substr(10) : path;
    if (p.size() > 1 && p.back() == '/') p.pop_back();
    if (!starts_with(p, "/workers/") || !ends_with(p, "/command")) return false;
    const std::string mid = p.substr(9, p.size() - 9 - 8);
    return !mid.empty() && mid.find('/') == std::string::npos;
}
}  // namespace

Own own_route(const std::string& method, const std::string& path) {
    const std::string p = own_path(path);
    if (p == "/auth/login") return method == "POST" ? Own::Login : Own::MethodNotAllowed;
    if (p == "/auth/logout") return method == "POST" ? Own::Logout : Own::MethodNotAllowed;
    if (p == "/auth/me") return method == "GET" ? Own::Me : Own::MethodNotAllowed;
    if (p == "/mode") return method == "GET" ? Own::ModeGet : method == "PUT" ? Own::ModeSet : Own::MethodNotAllowed;
    return Own::None;
}

Need classify(const std::string& method, const std::string& path, bool ui_enabled) {
    switch (own_route(method, path)) {
        case Own::Login:
        case Own::Logout:
        case Own::Me:
        case Own::MethodNotAllowed:
            return Need::Open;  // 처리하는 쪽이 신분을 본다(me는 401, 405는 그대로)
        case Own::ModeGet:
            return Need::Read;
        case Own::ModeSet:
            return Need::Maintain;
        case Own::None:
            break;
    }
    const bool board = starts_with(path, "/api/board/");
    const std::string p = board ? path.substr(10) : path;
    if (rstrip_char(p, '/') == "/health") return Need::Open;
    if (!board && method == "GET" && open_path(path, ui_enabled)) return Need::Open;
    if (starts_with(p, "/system/product-build")) return Need::Maintain;  // 개발 전용, 내려받기도
    if (method == "GET") return Need::Read;
    if (method == "POST" && is_command(path)) return Need::Operate;
    return Need::Maintain;
}

bool allows(Role role, Need need) {
    switch (need) {
        case Need::Open: return true;
        case Need::Read: return role >= Role::Viewer;
        case Need::Operate: return role >= Role::Operator;
        case Need::Maintain: return role >= Role::Maintainer;
    }
    return false;
}

// ---- 세션 ----

void Sessions::sweep(double now) {
    for (auto it = m_.begin(); it != m_.end();) {
        if (expired(it->second, now)) it = m_.erase(it);
        else ++it;
    }
}

void Sessions::add(const std::string& sid, const std::string& user, const std::string& stamp, double now) {
    std::lock_guard<std::mutex> lock(mu_);
    sweep(now);
    while (m_.size() >= max_) {  // 넘치면 가장 오래 안 쓴 세션부터
        auto oldest = std::min_element(m_.begin(), m_.end(),
                                       [](const auto& a, const auto& b) { return a.second.last < b.second.last; });
        m_.erase(oldest);
    }
    m_[sid] = SessionInfo{user, stamp, now, now};
}

std::optional<SessionInfo> Sessions::touch(const std::string& sid, double now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = m_.find(sid);
    if (it == m_.end()) return std::nullopt;
    if (expired(it->second, now)) {
        m_.erase(it);
        return std::nullopt;
    }
    it->second.last = now;
    return it->second;
}

void Sessions::remove(const std::string& sid) {
    std::lock_guard<std::mutex> lock(mu_);
    m_.erase(sid);
}

size_t Sessions::size(double now) {
    std::lock_guard<std::mutex> lock(mu_);
    sweep(now);
    return m_.size();
}

// ---- 로그인 실패 제한 ----

double LoginLimiter::locked_for(const std::string& key, double now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = m_.find(key);
    if (it == m_.end() || it->second.until <= now) return 0;
    return it->second.until - now;
}

void LoginLimiter::failure(const std::string& key, double now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (m_.size() >= max_keys_ && !m_.count(key)) {
        // 표가 차면 막히지 않은 오래된 줄부터 버린다(주소를 바꿔 가며 표를 키우지 못하게)
        for (auto it = m_.begin(); it != m_.end();) {
            if (it->second.until <= now && now - it->second.last > lock_s_) it = m_.erase(it);
            else ++it;
        }
        if (m_.size() >= max_keys_) {  // 그래도 차면 막히지 않은 것 중 가장 오래된 것(없으면 아무거나)
            auto victim = m_.begin();
            for (auto it = m_.begin(); it != m_.end(); ++it)
                if (it->second.until <= now && (victim->second.until > now || it->second.last < victim->second.last))
                    victim = it;
            m_.erase(victim);
        }
    }
    Entry& e = m_[key];
    if (now - e.last > 10 * 60) e.failures = 0;  // 10분 지난 실패는 잊음
    e.last = now;
    if (++e.failures >= max_failures_) {
        e.until = now + lock_s_;
        e.failures = 0;
    }
}

void LoginLimiter::success(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    m_.erase(key);
}

// ---- 명령 기록 ----

std::string record_line(const RecordFields& f) {
    auto cut = [](const std::string& s, size_t n) { return utf8_replace(s.size() > n ? s.substr(0, n) : s); };
    Json o = Json::object();
    o["t"] = f.t;
    o["user"] = cut(f.user, 64);
    o["role"] = f.role;
    o["mode"] = f.mode;
    o["remote"] = cut(f.remote, 64);
    o["method"] = cut(f.method, 16);
    o["path"] = cut(f.path, 512);
    o["cmd"] = f.cmd.empty() ? Json(nullptr) : Json(cut(f.cmd, 128));
    o["code"] = f.code;
    o["ms"] = std::round(f.ms * 10) / 10;
    return compact(o) + "\n";
}

std::string cookie_value(const std::string& header, const std::string& name) {
    std::string got;
    size_t start = 0;
    while (start <= header.size()) {
        size_t semi = header.find(';', start);
        if (semi == std::string::npos) semi = header.size();
        const std::string part = strip(header.substr(start, semi - start));
        const size_t eq = part.find('=');
        if (part.substr(0, eq) == name) got = eq == std::string::npos ? "" : part.substr(eq + 1);
        start = semi + 1;
    }
    return got;
}

std::string redact_query(const std::string& query) {
    std::string out;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        if (amp == std::string::npos) amp = query.size();
        std::string part = query.substr(start, amp - start);
        const size_t eq = part.find('=');
        const std::string key = to_lower(part.substr(0, eq));
        if (eq != std::string::npos && (key == "token" || key == "access_token" || key == "password"))
            part = part.substr(0, eq + 1) + "***";
        if (start > 0) out += '&';
        out += part;
        start = amp + 1;
    }
    return out;
}

}  // namespace gw
