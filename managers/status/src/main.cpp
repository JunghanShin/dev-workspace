// status-manager: 시스템 상태 매니저(기본 탑재, managers/status/design.md). process-manager가 실행한다:
//     status-manager --config <데이터>/state/workers/status-manager.json
//
// 구조: 호스트(libworker Host) + 모듈 status(lib/status). 스레드 status.pm·status.pub·ss.<id>
// exit: 0 정상 정지, 1 설정 오류, 3 자체 자원 실패, 4 내부 실패(스레드 예외·멈춤)
#include "status/module.hpp"

int main(int argc, char** argv) {
    worker::Host host("status-manager");
    host.add<status::Module>();
    return host.run(argc, argv);
}
