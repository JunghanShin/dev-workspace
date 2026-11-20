// 요청 처리 단계 (docs/gateway.md 2장): 신분 확인 → 권한 → 전달 → 기록 → 응답.
//
// 지금(6장 1~2단계)은 WORKSPACE_AUTH=off만: 신분 확인은 익명, 권한은 지금 REST와 같은 토큰 검사
// (WORKSPACE_REST_TOKEN, 비어 있으면 모두 허용), 기록은 하지 않음. 로그인·역할·운용/정비 모드·명령 기록은
// 4단계에서 Auth·Recorder 구현을 바꿔 끼운다(server.cpp는 그대로).
#pragma once

#include <functional>
#include <string>

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
    std::string role = "-";
    bool anonymous = true;
};

struct Decision {
    bool allowed = true;
    int status = 0;          // 거절일 때 상태 코드
    std::string code, message;
};

class Auth {
public:
    virtual ~Auth() = default;
    virtual Identity identify(const RequestView& rq) const = 0;
    virtual Decision authorize(const Identity& who, const RequestView& rq) const = 0;
};

class Recorder {
public:
    virtual ~Recorder() = default;
    // 쓰기 요청 기록 한 줄(4단계: gw.log 큐로). 지금은 아무것도 안 함
    virtual void record(const Identity& who, const RequestView& rq, int status, double ms) = 0;
};

class NoRecorder : public Recorder {
public:
    void record(const Identity&, const RequestView&, int, double) override {}
};

// WORKSPACE_AUTH=off: 지금 REST와 같은 토큰 검사(Python Handler._authorized)
class AuthOff : public Auth {
public:
    AuthOff(std::string token, bool ui_enabled) : token_(std::move(token)), ui_enabled_(ui_enabled) {}
    Identity identify(const RequestView&) const override { return Identity{}; }
    Decision authorize(const Identity& who, const RequestView& rq) const override;

    // 토큰 없이 열어 두는 경로인가(/health, 화면 파일)
    static bool open_path(const std::string& path, bool ui_enabled);

private:
    std::string token_;
    bool ui_enabled_;
};

// 같은 길이면 끝까지 비교(시간으로 토큰을 알아내지 못하게)
bool constant_time_equal(const std::string& a, const std::string& b);

}  // namespace gw
