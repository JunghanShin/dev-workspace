// 신분 확인·역할 규칙 (docs/gateway.md 3장). 시스템 호출 없음: 시계·무작위 값·파일 내용은 부르는 쪽(src)이 넘긴다.
//
//   비밀번호 해시   "pbkdf2-sha256$<반복>$<소금 base64>$<해시 base64>" (PBKDF2-HMAC-SHA256, 32바이트)
//   사용자 파일     <데이터>/config/users.json {"users": [{name, role, password_hash, tokens: [{name, hash}]}]}
//                   토큰은 sha256 16진으로만 둔다(원문은 만들 때 한 번만 보여 줌)
//   경로 → 필요한 권한(Need), 역할(Role)이 그 권한을 가지나(allows)
//   세션 표(Sessions: 쉼 만료·절대 만료), 로그인 실패 제한(LoginLimiter), 명령 기록 한 줄(record_line)
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "text.hpp"

namespace gw {

enum class Role { None = 0, Viewer = 1, Operator = 2, Maintainer = 3 };
bool parse_role(const std::string& s, Role& out);
const char* role_name(Role r);  // None → "-"

// ---- 비밀번호 해시 ----
constexpr uint32_t kPbkdf2Iterations = 200000;  // PC WSL 약 0.1 s(docs/gateway.md 6.2)
constexpr uint32_t kMinIterations = 1000;
constexpr uint32_t kMaxIterations = 5000000;    // 파일에 터무니없는 값이 있어도 로그인 하나가 오래 붙잡지 않게
constexpr size_t kSaltBytes = 16, kHashBytes = 32;

struct PasswordHash {
    uint32_t iterations = 0;
    std::string salt, hash;  // 이진
};
std::string make_password_hash(const std::string& password, const std::string& salt, uint32_t iterations);
bool parse_password_hash(const std::string& s, PasswordHash& out);
// 형식이 틀리면 false. 맞으면 계산해서 같은 시간 비교
bool verify_password(const std::string& stored, const std::string& password);
// 없는 사용자일 때도 같은 만큼 계산(걸린 시간으로 사용자 이름을 알아내지 못하게)
void burn_password_time(const std::string& password);

// ---- 사용자 파일 ----
struct UserToken {
    std::string name, hash;  // hash: sha256 16진
};
struct User {
    std::string name;
    Role role = Role::None;
    std::string password_hash;
    std::vector<UserToken> tokens;
};
struct UsersDb {
    std::vector<User> users;
    const User* find(const std::string& name) const;
    User* find(const std::string& name);
    // 토큰 원문 → 사용자(없으면 nullptr), token_name에 토큰 이름
    const User* by_token(const std::string& token, std::string* token_name = nullptr) const;
};
bool valid_user_name(const std::string& s);   // [A-Za-z0-9_.-]{1,64}, 첫 글자는 영문·숫자
bool valid_token_name(const std::string& s);  // 같은 규칙
bool parse_users(const std::string& text, UsersDb& out, std::string& err);
std::string dump_users(const UsersDb& db);  // 사람이 읽는 JSON(들여쓰기 2) + 줄바꿈

// ---- 경로 → 권한 ----
enum class Need { Open, Read, Operate, Maintain };
const char* need_name(Need n);
// 화면 파일·/health처럼 신분 확인 없이 여는 경로인가(WORKSPACE_REST_TOKEN 판과 같은 규칙)
bool open_path(const std::string& path, bool ui_enabled);
// path: 요청 경로 그대로(쿼리 없음, /api/board 접두어 포함 가능)
Need classify(const std::string& method, const std::string& path, bool ui_enabled);
bool allows(Role role, Need need);

// ---- 운용/개발 모드(docs/command-path.md 3장) ----
bool is_worker_command(const std::string& path);  // POST /workers/<id>/command 경로
// 워커 명령의 종류: 계약(get_io 답)으로. 읽기(get_status, "read": true) · 역할 명령(provides) · 그 밖(워커 전용·계약에 없음)
enum class CmdKind { Read, Role, Other };
struct CommandKinds {
    std::set<std::string> read, role;
    CmdKind of(const std::string& cmd) const {
        if (cmd == "get_status" || read.count(cmd)) return CmdKind::Read;
        return role.count(cmd) ? CmdKind::Role : CmdKind::Other;
    }
};
CommandKinds command_kinds(const Json& io);  // io가 null(계약 없음)이면 get_status만 읽기

// 게이트웨이가 직접 답하는 경로: /api/auth/{login,logout,me}, /api/mode(앞에 /api/board도, /api 없이도)
enum class Own { None, Login, Logout, Me, ModeGet, ModeSet, MethodNotAllowed };
Own own_route(const std::string& method, const std::string& path);

// ---- 세션(메모리에만: 게이트웨이를 다시 시작하면 다시 로그인) ----
struct SessionInfo {
    std::string user, stamp;  // stamp: 만들 때의 비밀번호 해시(비밀번호를 바꾸면 세션이 끝나게)
    double created = 0, last = 0;
};
class Sessions {
public:
    explicit Sessions(double idle_s = 12 * 3600.0, double absolute_s = 7 * 86400.0, size_t max = 4096)
        : idle_s_(idle_s), absolute_s_(absolute_s), max_(max) {}
    void add(const std::string& sid, const std::string& user, const std::string& stamp, double now);
    // 살아 있으면 마지막 사용 시각을 고치고 돌려준다. 만료면 지우고 nullopt
    std::optional<SessionInfo> touch(const std::string& sid, double now);
    void remove(const std::string& sid);
    size_t size(double now);
    double idle_s() const { return idle_s_; }
    double absolute_s() const { return absolute_s_; }

private:
    bool expired(const SessionInfo& s, double now) const {
        return now - s.last >= idle_s_ || now - s.created >= absolute_s_;
    }
    void sweep(double now);  // 잠금 안에서

    const double idle_s_, absolute_s_;
    const size_t max_;
    std::mutex mu_;  // m_ (I/O 없음)
    std::map<std::string, SessionInfo> m_;
};

// ---- 로그인 실패 제한(주소마다): max_failures번 틀리면 lock_s 동안 막음 ----
class LoginLimiter {
public:
    explicit LoginLimiter(int max_failures = 5, double lock_s = 30.0, size_t max_keys = 4096)
        : max_failures_(max_failures), lock_s_(lock_s), max_keys_(max_keys) {}
    double locked_for(const std::string& key, double now);  // 남은 막힘 초(0이면 됨)
    void failure(const std::string& key, double now);
    void success(const std::string& key);

private:
    struct Entry {
        int failures = 0;
        double until = 0, last = 0;
    };
    const int max_failures_;
    const double lock_s_;
    const size_t max_keys_;
    std::mutex mu_;  // m_
    std::map<std::string, Entry> m_;
};

// ---- 명령 기록 한 줄(gw.log). 본문·쿼리·헤더는 넣지 않는다(비밀번호·토큰이 섞이지 않게) ----
struct RecordFields {
    std::string t, user, role, mode, remote, method, path, cmd, from;
    int code = 0;
    double ms = 0;
};
std::string record_line(const RecordFields& f);  // JSON 한 줄 + "\n"

// Cookie 헤더에서 이름의 값(같은 이름이 여럿이면 마지막, 없으면 "")
std::string cookie_value(const std::string& header, const std::string& name);
// 기록용 쿼리: token·access_token·password 값은 ***로
std::string redact_query(const std::string& query);

}  // namespace gw
