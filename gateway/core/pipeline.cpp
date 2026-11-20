#include "pipeline.hpp"

#include "routes.hpp"
#include "text.hpp"

namespace gw {

bool constant_time_equal(const std::string& a, const std::string& b) {
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const std::string& c = a.size() == b.size() ? b : a;  // 길이가 다르면 a끼리 비교(시간은 a 길이)
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ c[i]);
    return diff == 0;
}

bool AuthOff::open_path(const std::string& path, bool ui_enabled) {
    const std::string t = rstrip_char(path, '/');
    if (t == "/health" || t == "/api/board/health") return true;
    if (!ui_enabled) return false;
    if (path == "/" || path == "/index.html") return true;
    // 화면 파일 자체에는 데이터가 없다. 데이터는 /api·REST 경로에서 토큰을 본다
    for (const char* p : {"/api/", "/workers", "/system", "/logs", "/records", "/history"})
        if (starts_with(path, p)) return false;
    for (const char* e : {".html", ".js", ".css", ".svg", ".png", ".ico", ".woff2", ".json"})
        if (ends_with(path, e)) return true;
    return false;
}

Decision AuthOff::authorize(const Identity&, const RequestView& rq) const {
    Decision d;
    if (token_.empty() || open_path(rq.path, ui_enabled_)) return d;
    std::string got;
    const std::string auth = rq.header("Authorization");
    if (starts_with(auth, "Bearer ")) got = strip(auth.substr(7));
    if (got.empty()) {
        // Cookie: workspace_token=<토큰>(같은 이름이 여럿이면 마지막)
        const std::string cookie = rq.header("Cookie");
        size_t start = 0;
        while (start <= cookie.size()) {
            size_t semi = cookie.find(';', start);
            if (semi == std::string::npos) semi = cookie.size();
            const std::string part = strip(cookie.substr(start, semi - start));
            const size_t eq = part.find('=');
            if (part.substr(0, eq) == "workspace_token") got = eq == std::string::npos ? "" : part.substr(eq + 1);
            start = semi + 1;
        }
    }
    if (got.empty()) {
        // EventSource는 헤더를 못 붙인다(같은 출처면 쿠키로도 됨)
        const Params q = parse_qs(rq.query);
        auto it = q.find("token");
        if (it != q.end()) got = it->second;
    }
    if (constant_time_equal(got, token_)) return d;
    d.allowed = false;
    d.status = 401;
    d.code = "unauthorized";
    d.message = "token required (Authorization: Bearer <token>)";
    return d;
}

}  // namespace gw
