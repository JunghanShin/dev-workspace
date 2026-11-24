// mission-manager: 명령 송수신 매니저(기본 탑재, managers/mission/design.md). process-manager가 실행한다:
//     mission-manager --config <데이터>/state/workers/mission-manager.json
//
// 구조: 호스트(libworker Host) + 모듈 mission(lib/mission). 스레드 mission.pm·mission.io·mission.pub·mc.<대상>.<역할>
// exit: 0 정상 정지, 1 설정 오류, 3 자체 자원 실패, 4 내부 실패(스레드 예외·멈춤)
#include "mission/module.hpp"

int main(int argc, char** argv) {
    worker::Host host("mission-manager");
    host.add<mission::Module>();
    return host.run(argc, argv);
}
