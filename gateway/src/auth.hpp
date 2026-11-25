// 신분 확인 켜짐(WORKSPACE_AUTH=on, 제품 묶음은 늘): 사용자 파일·세션·토큰·역할 (docs/gateway.md 3장).
//
//   누구인가   Authorization: Bearer <토큰>(프로그램), 쿠키 workspace_session(화면 로그인), GET이면 ?token=도(EventSource)
//   해도 되나  경로 → 필요한 권한(access.hpp classify) ↔ 역할. 익명 401, 역할 부족 403
//   사용자 파일이 없거나 비면 /health 말고 모두 503(조용히 열어 두지 않음. 제품 모드, 0.86부터 켬이면 늘)
//
// 공유 상태: 세션 표·실패 제한 표(각자 잠금 하나, I/O 없음), 사용자 파일 사본(shared_ptr 바꿔 끼우기).
// 사용자 파일은 1 s마다 바뀌었는지(mtime·크기·inode) 보고, 바뀌었으면 잠금 밖에서 다시 읽는다(재시작 없이 반영).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "media_auth.hpp"
#include "pipeline.hpp"
#include "text.hpp"

namespace gw {

// 커널 무작위(getrandom). 실패하면 std::runtime_error
std::string random_bytes(size_t n);
// tmp → fsync → rename → 폴더 fsync, 권한 mode. 실패하면 false와 이유
bool write_file_atomic(const std::string& path, const std::string& data, int mode, std::string& err);
// 사용자 파일 읽기. 없으면 빈 목록으로 true. 깨졌으면 false와 이유
bool load_users(const std::string& path, UsersDb& out, std::string& err);

class UsersFile {
public:
    explicit UsersFile(std::string path, double check_s = 1.0);
    std::shared_ptr<const UsersDb> get();  // 확인 주기가 지났으면 파일을 보고, 바뀌었으면 다시 읽은 것
    const std::string& path() const { return path_; }

private:
    void reload_if_changed();
    const std::string path_;
    const double check_s_;
    std::atomic<double> next_check_{0};
    std::mutex reload_mu_;  // 다시 읽기는 한 스레드만(try_lock, 나머지는 지금 사본을 씀)
    std::mutex mu_;         // db_, sig_ (I/O 없음)
    std::shared_ptr<const UsersDb> db_;
    std::string sig_;
};

struct AuthConfig {
    std::string users_file;
    bool product = false;
    bool ui_enabled = true;
    double idle_s = 12 * 3600.0, absolute_s = 7 * 86400.0;
    int max_failures = 5;
    double lock_s = 30.0;
    double users_check_s = 1.0;
};

class AuthOn : public Auth {
public:
    explicit AuthOn(AuthConfig cfg);
    bool on() const override { return true; }
    Identity identify(const RequestView& rq) override;
    Decision authorize(const Identity& who, const RequestView& rq) override;

    struct LoginResult {
        int status = 401;
        std::string code, message;
        std::string sid;           // 성공하면 새 세션
        Identity who;              // 성공하면 그 사용자
        bool known_user = false;   // 기록용: 있는 사용자 이름이었나(없는 이름은 기록하지 않음)
        double retry_after = 0;    // 429면 남은 초
    };
    LoginResult login(const std::string& name, const std::string& password, const std::string& remote);
    void logout(const std::string& sid) { sessions_.remove(sid); }
    bool has_users();
    // 영상(MediaMTX authMethod http, docs/media-auth.md) 자격 → 역할(못 알아내면 None). 사용자별 토큰(token 칸, 또는
    // password 칸 = 토큰) 먼저, 아니면 계정 이름+비밀번호. 틀리면 주소마다 실패를 셈(로그인과 같은 제한, 따로 셈). who: 기록용
    Role media_role(const MediaRequest& rq, std::string& who);
    Json state();
    double idle_s() const { return cfg_.idle_s; }
    double absolute_s() const { return cfg_.absolute_s; }

private:
    AuthConfig cfg_;
    UsersFile users_;
    Sessions sessions_;
    LoginLimiter limiter_;
    std::atomic<bool> warned_empty_{false};
};

}  // namespace gw
