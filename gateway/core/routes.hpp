// 경로 표 (design.md 6.4, Python workspace_rest/__main__.py·reads.py와 같은 순서·규칙). 시스템 호출 없음.
//
// 요청 하나의 길(server.cpp가 이 순서로 부른다):
//   화면 경로(ui_route: /api/board 접두어 떼기, /api/remote, /api/panels, /api/ui, 화면 파일)
//   → route(): /health → 제품 빌드 → 설정(/workspace/settings) → 읽기(GET) → 스트림 → 명령 → Manager op 표 → 405/404
#pragma once

#include <map>
#include <string>

namespace gw {

using Params = std::map<std::string, std::string>;

// urllib.parse.parse_qs(query)에서 키마다 마지막 값(빈 값·"=" 없는 칸은 버림, + → 공백, %XX 풂)
Params parse_qs(const std::string& query);
std::string unquote_plus(const std::string& s);

enum class RouteKind { None, Health, ProductBuild, ProductOff, Read, Stream, Command, Manager, Settings, MethodNotAllowed };

struct Route {
    RouteKind kind = RouteKind::None;
    std::string id, name;      // 경로의 {id}, 프리셋 {name}
    bool has_id = false, has_name = false;
    std::string read;          // 읽기 이름(worker_log, history …)
    std::string op;            // Manager op
    int ok_status = 200;
    std::string body_key;      // 본문을 넣을 키(없으면 본문을 읽지 않음)
    bool save_false = false;   // POST /config/apply: {"save": false}
    std::string sub;           // 제품 빌드 하위 경로("", "/check", "/download", 그 밖)
};

// path: 쿼리 없는 경로(/api/board 접두어를 뗀 뒤)
Route route(const std::string& method, const std::string& path, bool product);
// 읽기 경로 표만(/api/remote가 만든 경로를 맞출 때)
bool match_read(const std::string& path, Route& out);
// 이 요청이 본문을 읽는가(크기 초과를 미리 400으로 답할지)
bool reads_body(const std::string& method, const Route& r);

// 워커 id 형식(reads.py ID_RE)
bool valid_worker_id(const std::string& id);
// 제품 빌드 profile·power_mode 형식
bool valid_profile(const std::string& s);
bool valid_power_mode(const std::string& s);

// 화면 경로 분류(GET에서만 쓰는 것과 /api/board 접두어)
enum class UiKind { None, Board, Remote, Panels, UiInfo, Root };
struct UiRoute {
    UiKind kind = UiKind::None;
    std::string remote;        // /api/remote/<이름>
};
UiRoute ui_route(const std::string& method, const std::string& path);

}  // namespace gw
