#include "media_auth.hpp"

namespace gw {

int worker_error_status(const Json& code) {
    if (!code.is_string()) return 500;
    const std::string c = code.get<std::string>();
    if (c == "bad_request") return 400;
    if (c == "forbidden") return 403;   // 설정상 금지·허용 목록 밖·명령 조건(0.86부터, 예전 500)
    if (c == "busy") return 409;        // 같은 동작이 진행 중·mission 대기 넘침(0.86부터, 예전 500)
    if (c == "unavailable") return 503;
    if (c == "timeout") return 504;
    return 500;  // internal과 모르는 code
}

int manager_error_status(const Json& code) {
    if (!code.is_string()) return 500;
    const std::string c = code.get<std::string>();
    if (c == "bad_request") return 400;
    if (c == "forbidden") return 403;
    if (c == "not_found") return 404;
    if (c == "conflict") return 409;
    return 500;
}

bool parse_media_request(const Json& body, MediaRequest& out) {
    out = MediaRequest{};
    if (!body.is_object()) return false;
    auto s = [&body](const char* k) {
        auto it = body.find(k);
        return it != body.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    out.user = s("user");
    out.password = s("password");
    out.token = s("token");
    out.ip = s("ip");
    out.action = s("action");
    out.path = s("path");
    out.protocol = s("protocol");
    return true;
}

bool is_loopback(const std::string& ip) {
    std::string a = ip;
    if (starts_with(a, "::ffff:")) a = a.substr(7);
    if (a == "::1") return true;
    if (!starts_with(a, "127.")) return false;
    // 127.x.y.z 꼴만(숫자와 점)
    int dots = 0;
    for (char c : a) {
        if (c == '.') ++dots;
        else if (c < '0' || c > '9') return false;
    }
    return dots == 3;
}

int media_status(MediaVerdict v) {
    switch (v) {
        case MediaVerdict::Allow: return 200;
        case MediaVerdict::AskCredentials: return 401;
        case MediaVerdict::Deny: return 403;
    }
    return 403;
}

namespace {
bool admin_action(const std::string& a) { return a == "api" || a == "metrics" || a == "pprof"; }
bool view_action(const std::string& a) { return a == "read" || a == "playback"; }
}  // namespace

bool media_needs_identity(const MediaRequest& rq, bool auth_on) {
    if (admin_action(rq.action) || is_loopback(rq.ip) || !auth_on) return false;
    return view_action(rq.action);
}

MediaVerdict media_verdict(const MediaRequest& rq, bool auth_on, Role who) {
    const bool local = is_loopback(rq.ip);
    if (admin_action(rq.action)) return local ? MediaVerdict::Allow : MediaVerdict::Deny;  // MediaMTX 관리(기본 꺼짐)
    if (local) return MediaVerdict::Allow;    // 워커 송출·nginx /media/(그쪽에서 이미 로그인 확인)·중계
    if (!auth_on) return MediaVerdict::Allow; // 로그인 꺼짐: 예전처럼 열림
    if (!view_action(rq.action)) return MediaVerdict::Deny;  // 바깥 publish(와 모르는 동작)는 늘 거절
    if (who == Role::None) return MediaVerdict::AskCredentials;
    return allows(who, Need::Read) ? MediaVerdict::Allow : MediaVerdict::Deny;
}

const User* media_token_user(const UsersDb& db, const MediaRequest& rq, std::string* token_name) {
    if (const User* u = db.by_token(rq.token, token_name)) return u;
    return db.by_token(rq.password, token_name);
}

}  // namespace gw
