// 영상 로그인 판정 (docs/media-auth.md). 시스템 호출 없음: 사용자 파일 내용은 부르는 쪽(src/auth)이 넘긴다.
//
//   MediaMTX(authMethod: http)가 연결마다 POST하는 JSON {user, password, token, ip, action, path, protocol, id, query}
//   → 허락(200) / 자격 물음(401: RTSP 등은 이 답을 받아야 이름·비밀번호를 보냄) / 거절(403)
//
//   규칙: 보드 안(127.0.0.1·::1)은 늘 허락(워커 송출·nginx 넘김·중계). 로그인이 꺼져 있으면 모두 허락(예전과 같음).
//         켜져 있으면 바깥은 read·playback만, 사용자별 토큰(token 칸, 또는 password 칸 = 토큰) 또는 계정 이름+비밀번호,
//         역할 viewer 이상. 바깥 publish는 늘 거절. api·metrics·pprof는 보드 안에서만
// 또 워커 명령·Manager 오류 code → HTTP 상태(design.md 6.4·9.6)도 여기.
#pragma once

#include <string>

#include "access.hpp"
#include "text.hpp"

namespace gw {

// 워커 명령 오류 code → HTTP (design.md 9.6): bad_request 400, forbidden 403, busy 409, unavailable 503, timeout 504,
// internal·모르는 code 500
int worker_error_status(const Json& code);
// Manager 오류 code → HTTP (design.md 6.4): bad_request 400, forbidden 403, not_found 404, conflict 409, 그 밖 500
int manager_error_status(const Json& code);

struct MediaRequest {
    std::string user, password, token, ip, action, path, protocol;
};
// MediaMTX 본문 → MediaRequest. 객체가 아니면 false(없는 칸·글이 아닌 칸은 빈 글)
bool parse_media_request(const Json& body, MediaRequest& out);
// 보드 안 주소인가(127.0.0.0/8, ::1, ::ffff:127.x)
bool is_loopback(const std::string& ip);

enum class MediaVerdict { Allow, AskCredentials, Deny };
int media_status(MediaVerdict v);  // 200 / 401 / 403
// who: 자격으로 알아낸 역할(못 알아냈으면 Role::None). auth_on: 지금 로그인 켬(제품은 늘)
MediaVerdict media_verdict(const MediaRequest& rq, bool auth_on, Role who);
// 자격이 필요한 요청인가(보드 안·로그인 꺼짐·바깥 publish 등은 자격을 볼 필요 없음: 비밀번호 계산을 아낌)
bool media_needs_identity(const MediaRequest& rq, bool auth_on);
// 토큰으로 사용자 찾기: token 칸, 없거나 틀리면 password 칸을 토큰으로(SRT·RTSP는 이름+비밀번호 자리에 넣음). 없으면 nullptr
const User* media_token_user(const UsersDb& db, const MediaRequest& rq, std::string* token_name = nullptr);

}  // namespace gw
