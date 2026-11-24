// 워크스페이스 연동 (워커 템플릿 ③층, 워크스페이스 design.md 6.6.4).
//
// 워커 코드가 워크스페이스에 닿는 곳은 여기 하나다. 빌드 옵션으로 구현을 고른다:
//   WORKER_WORKSPACE=ON  (기본) src/workspace/workspace.cpp: Manager heartbeat(WORKSPACE_NOTIFY_SOCKET),
//                        데이터 폴더 기준 소켓 경로(WORKSPACE_DATA), WORKSPACE_WORKER_ID
//   WORKER_WORKSPACE=OFF src/standalone.cpp: heartbeat 없음, 소켓은 $XDG_RUNTIME_DIR 또는 /tmp
// 워크스페이스 밖에서 쓸 때는 OFF로 빌드하고 워커의 workspace/ 폴더를 지우면 된다. 나머지 코드는 같다.
#pragma once

#include <sys/un.h>

#include <string>

namespace worker {

// 이 빌드에 워크스페이스 연동이 들어 있는지(WORKER_WORKSPACE)
bool workspace_build();

// Manager heartbeat(READY, WATCHDOG 한 줄). 연동이 없거나 WORKSPACE_NOTIFY_SOCKET이 없으면(단독 실행) 보내지 않는다
class Notifier {
public:
    Notifier();
    ~Notifier();
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    bool enabled() const { return fd_ >= 0; }
    // 한 줄 송신. non-blocking, 실패는 무시한다(워커는 heartbeat 때문에 멈추지 않는다)
    void send(const std::string& msg);

private:
    int fd_ = -1;
    sockaddr_un addr_{};
};

// 설정 파일에 id가 없을 때 쓸 id(연동: WORKSPACE_WORKER_ID). 없으면 빈 문자열(→ 설정 파일 이름)
std::string worker_id_from_environment();

// 명령 소켓 기본 경로. 앞에서부터 처음 쓸 수 있는 것:
//   (연동) <WORKSPACE_DATA>/run/cmd/<id>.sock  →  $XDG_RUNTIME_DIR/<id>.sock  →  /tmp/<id>.sock
std::string default_command_socket(const std::string& id);

// 텔레메트리 스트림(발행) 소켓 기본 경로. 같은 순서로 (연동) run/stream/<id>.sock, <id>.stream.sock (design.md 9.7)
std::string default_stream_socket(const std::string& id);

// 그 밖의 소켓 기본 경로(kind 예: "frames" 프레임 버스). 같은 순서로
//   (연동) <WORKSPACE_DATA>/run/<kind>/<id>.sock  →  $XDG_RUNTIME_DIR/<id>.<kind>.sock  →  /tmp/<id>.<kind>.sock
std::string default_socket(const std::string& id, const std::string& kind);

// 워크스페이스 데이터 폴더(도구가 워크스페이스 밖에서 실행될 때 소켓을 찾는 용도).
//   (연동) $WORKSPACE_DATA, 없으면 이 실행 파일이 <워크스페이스>/workers/ 아래 어딘가에 있을 때 <워크스페이스>_data.
//   연동이 없거나 못 찾으면 빈 문자열
std::string workspace_data_dir();

}  // namespace worker
