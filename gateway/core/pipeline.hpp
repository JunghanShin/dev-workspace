// 요청 처리 단계 (docs/gateway.md 2장): 신분 확인 → 권한 → 전달 → 기록 → 응답.
//
// Auth 구현 둘:
//   AuthOff(여기)        WORKSPACE_AUTH=off(개발 보드 기본). 지금 REST와 같은 토큰 검사(WORKSPACE_REST_TOKEN, 비면 모두 허용)
//   AuthOn(src/auth.hpp) WORKSPACE_AUTH=on·제품 묶음. 사용자 파일·세션·토큰·역할(access.hpp 규칙)
// Recorder: 쓰기 요청마다 기록 한 줄(src/recorder.hpp의 gw.log). 데이터 폴더가 없으면 NoRecorder.
#pragma once

#include <functional>
#include <string>

#include "access.hpp"
#include "crypto.hpp"

namespace gw {

// 요청 보기(복사 없이). path는 접두어를 떼기 전 원래 경로(쿼리 없음)
struct RequestView {
    std::string method;
    std::string path;
    std::string query;
    std::function<std::string(const char*)> header;
};

struct Identity {
    std::string user = "-";
    Role role = Role::None;
    bool anonymous = true;
    std::string via;  // "session" | "token" | ""(익명·인증 꺼짐)
    std::string sid;  // 세션으로 왔으면 그 세션(로그아웃용)
};

struct Decision {
    bool allowed = true;
    int status = 0;          // 거절일 때 상태 코드
    std::string code, message;
};

class Auth {
public:
    virtual ~Auth() = default;
    virtual bool on() const = 0;
    virtual Identity identify(const RequestView& rq) = 0;
    virtual Decision authorize(const Identity& who, const RequestView& rq) = 0;
};

class Recorder {
public:
    virtual ~Recorder() = default;
    virtual void record(RecordFields f) = 0;  // 막히지 않는다(큐가 차면 버리고 셈)
};

class NoRecorder : public Recorder {
public:
    void record(RecordFields) override {}
};

// WORKSPACE_AUTH=off: 지금 REST와 같은 토큰 검사(Python Handler._authorized)
class AuthOff : public Auth {
public:
    AuthOff(std::string token, bool ui_enabled) : token_(std::move(token)), ui_enabled_(ui_enabled) {}
    bool on() const override { return false; }
    Identity identify(const RequestView&) override { return Identity{}; }
    Decision authorize(const Identity& who, const RequestView& rq) override;

    // 토큰 없이 열어 두는 경로인가(/health, 화면 파일)
    static bool open_path(const std::string& path, bool ui_enabled) { return gw::open_path(path, ui_enabled); }

private:
    std::string token_;
    bool ui_enabled_;
};

// Authorization: Bearer <값>(없으면 "")
std::string bearer_token(const std::string& authorization);

}  // namespace gw
