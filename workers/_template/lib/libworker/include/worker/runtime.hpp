// 워커 실행 뼈대 (design.md 6.6, 9.3, 9.5). Manager가 있어도 없어도 같은 코드로 돈다.
//
// - Notifier(heartbeat)는 워크스페이스 연동이라 worker/workspace.hpp로 옮겼다(여기서 함께 include)
// - arm_watchdog: 자체 워치독. 처리 루프 안에서만 다시 맞춘다. 만료되면 SIGALRM 기본 동작으로 커널이 끝낸다
// - install_signals / stop_requested: SIGTERM·SIGINT → 정지 요청, SIGPIPE 무시
// - take_reload_request: SIGHUP(설정 다시 읽기 요청)이 왔으면 한 번 참
#pragma once

#include <string>

#include "worker/workspace.hpp"

namespace worker {

void arm_watchdog(double seconds);

void install_signals();
bool stop_requested();
bool take_reload_request();


}  // namespace worker
