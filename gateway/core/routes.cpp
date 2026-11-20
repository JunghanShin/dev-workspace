#include "routes.hpp"

#include <vector>

#include "text.hpp"

namespace gw {

namespace {

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 경로 → 칸들. 끝의 "/" 하나는 없는 것으로 본다(Python 정규식 "/?$")
bool segments(const std::string& path, std::vector<std::string>& out) {
    if (path.empty() || path[0] != '/') return false;
    std::string p = path;
    if (p.size() > 1 && p.back() == '/') p.pop_back();
    out.clear();
    size_t start = 1;
    while (true) {
        const size_t slash = p.find('/', start);
        out.push_back(p.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool valid_name(const std::string& s) {
    // [a-z0-9][a-z0-9_-]{0,31}
    if (s.empty() || s.size() > 32) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!(alnum || (i > 0 && (c == '_' || c == '-')))) return false;
    }
    return true;
}

// 틀: 글자 그대로, ":id"(빈 칸 아닌 칸 하나), ":name"(프리셋 이름)
bool match(const std::vector<std::string>& segs, const std::vector<const char*>& pat, Route& r) {
    if (segs.size() != pat.size()) return false;
    Route tmp = r;
    for (size_t i = 0; i < pat.size(); ++i) {
        const std::string p = pat[i];
        if (p == ":id") {
            if (segs[i].empty()) return false;
            tmp.id = segs[i];
            tmp.has_id = true;
        } else if (p == ":name") {
            if (!valid_name(segs[i])) return false;
            tmp.name = segs[i];
            tmp.has_name = true;
        } else if (segs[i] != p) {
            return false;
        }
    }
    r = tmp;
    return true;
}

struct ManagerRoute {
    const char* method;
    std::vector<const char*> pat;
    const char* op;
    int ok;
    const char* body;
    bool save_false;
};

const std::vector<ManagerRoute>& manager_routes() {
    static const std::vector<ManagerRoute> t = {
        {"GET", {"system"}, "system", 200, "", false},
        {"GET", {"workers"}, "list", 200, "", false},
        {"GET", {"workers", ":id"}, "get", 200, "", false},
        {"GET", {"workers", ":id", "config"}, "get_config", 200, "", false},
        // 저장: 사용 중 프리셋에 기록하고 적용. apply: 이번 부팅 동안만(임시)
        {"PUT", {"workers", ":id", "config"}, "set_config", 202, "config", false},
        {"POST", {"workers", ":id", "config", "apply"}, "set_config", 202, "config", true},
        {"POST", {"workers", ":id", "presets"}, "save_preset", 202, "preset", false},
        {"POST", {"workers", ":id", "presets", ":name", "use"}, "use_preset", 202, "", false},
        {"PUT", {"workers", ":id", "presets", ":name"}, "rename_preset", 202, "preset", false},
        {"DELETE", {"workers", ":id", "presets", ":name"}, "delete_preset", 202, "", false},
        {"GET", {"workers", ":id", "schema"}, "get_schema", 200, "", false},
        {"GET", {"workers", ":id", "io"}, "get_io", 200, "", false},
        {"PUT", {"workers", ":id", "autostart"}, "set_autostart", 202, "autostart", false},
        {"POST", {"workers", ":id", "start"}, "start", 202, "", false},
        {"POST", {"workers", ":id", "stop"}, "stop", 202, "", false},
    };
    return t;
}

struct ReadRoute {
    std::vector<const char*> pat;
    const char* name;
};

const std::vector<ReadRoute>& read_routes() {
    static const std::vector<ReadRoute> t = {
        {{"logs", "workers", ":id"}, "worker_log"},
        {{"logs", "service"}, "service_log"},
        {{"records", "commands"}, "commands"},
        {{"system", "processes"}, "processes"},
        {{"system", "power_modes"}, "power_modes"},
        {{"system", "workspace"}, "workspace"},
        {{"history", ":id"}, "history"},
        {{"workers", ":id", "tasks"}, "worker_tasks"},
    };
    return t;
}

}  // namespace

std::string unquote_plus(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out += static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return utf8_replace(out);  // unquote(errors="replace")
}

Params parse_qs(const std::string& query) {
    Params out;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        if (amp == std::string::npos) amp = query.size();
        const std::string part = query.substr(start, amp - start);
        const size_t eq = part.find('=');
        if (eq != std::string::npos) {
            const std::string value = unquote_plus(part.substr(eq + 1));
            if (!value.empty()) out[unquote_plus(part.substr(0, eq))] = value;
        }
        start = amp + 1;
    }
    return out;
}

bool match_read(const std::string& path, Route& out) {
    std::vector<std::string> segs;
    if (!segments(path, segs)) return false;
    for (const auto& rr : read_routes()) {
        Route r;
        if (match(segs, rr.pat, r)) {
            r.kind = RouteKind::Read;
            r.read = rr.name;
            out = r;
            return true;
        }
    }
    return false;
}

Route route(const std::string& method, const std::string& path, bool product) {
    Route r;
    const std::string trimmed = rstrip_char(path, '/');
    if (method == "GET" && trimmed == "/health") {
        r.kind = RouteKind::Health;
        return r;
    }
    if (product && starts_with(path, "/system/product-build")) {
        r.kind = RouteKind::ProductOff;
        return r;
    }
    if (starts_with(trimmed, "/system/product-build")) {
        r.kind = RouteKind::ProductBuild;
        r.sub = trimmed.substr(std::string("/system/product-build").size());
        return r;
    }
    if (method == "GET" && match_read(path, r)) return r;

    std::vector<std::string> segs;
    if (!segments(path, segs)) return r;
    Route m;
    if (match(segs, {"workers", ":id", "stream"}, m)) {
        m.kind = method == "GET" ? RouteKind::Stream : RouteKind::MethodNotAllowed;
        return m;
    }
    if (match(segs, {"workers", ":id", "command"}, m)) {
        m.kind = method == "POST" ? RouteKind::Command : RouteKind::MethodNotAllowed;
        return m;
    }
    bool matched_path = false;
    for (const auto& mr : manager_routes()) {
        Route x;
        if (!match(segs, mr.pat, x)) continue;
        matched_path = true;
        if (method != mr.method) continue;
        x.kind = RouteKind::Manager;
        x.op = mr.op;
        x.ok_status = mr.ok;
        x.body_key = mr.body;
        x.save_false = mr.save_false;
        return x;
    }
    if (matched_path) r.kind = RouteKind::MethodNotAllowed;
    return r;
}

bool reads_body(const std::string& method, const Route& r) {
    if (r.kind == RouteKind::Manager) return !r.body_key.empty();
    if (r.kind == RouteKind::Command) return true;
    if (r.kind == RouteKind::ProductBuild) return r.sub.empty() && method == "POST";
    return false;
}

bool valid_worker_id(const std::string& raw) {
    // ^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$ (Python $는 끝의 \n 하나를 봐줌)
    const std::string s = drop_final_newline(raw);
    if (s.empty() || s.size() > 64) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!(alnum || (i > 0 && (c == '_' || c == '.' || c == '-')))) return false;
    }
    return true;
}

namespace {
bool charset_ok(const std::string& raw, size_t maxlen, bool dash) {
    const std::string s = drop_final_newline(raw);
    if (s.empty() || s.size() > maxlen) return false;
    for (char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        (dash && c == '-');
        if (!ok) return false;
    }
    return true;
}
}  // namespace

bool valid_profile(const std::string& s) { return charset_ok(s, 64, true); }
bool valid_power_mode(const std::string& s) { return charset_ok(s, 32, false); }

UiRoute ui_route(const std::string& method, const std::string& path) {
    UiRoute u;
    if (starts_with(path, "/api/board/")) {
        u.kind = UiKind::Board;
        return u;
    }
    if (method != "GET") return u;
    if (starts_with(path, "/api/remote/")) {
        u.kind = UiKind::Remote;
        u.remote = path.substr(std::string("/api/remote/").size());
    } else if (path == "/api/panels") {
        u.kind = UiKind::Panels;
    } else if (path == "/api/ui") {
        u.kind = UiKind::UiInfo;
    } else if (path == "/" || path == "/index.html") {
        u.kind = UiKind::Root;
    }
    return u;
}

}  // namespace gw
