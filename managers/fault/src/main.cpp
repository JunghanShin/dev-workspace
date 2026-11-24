// fault-manager: 이상 판단 매니저(기본 탑재, managers/fault/design.md). process-manager가 실행한다:
//     fault-manager --config <데이터>/state/workers/fault-manager.json
//
// 구조: 호스트(libworker Host) + 모듈 fault(lib/fault). 스레드 fault.sys·fault.pm·fault.cmd·fault.pub·fs.<id>
// exit: 0 정상 정지, 1 설정 오류(규칙이 틀림), 3 자체 자원 실패, 4 내부 실패(스레드 예외·멈춤)
#include "fault/module.hpp"

int main(int argc, char** argv) {
    worker::Host host("fault-manager");
    host.add<fault::Module>();
    return host.run(argc, argv);
}
