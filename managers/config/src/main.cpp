// config-manager: 설정·버전 매니저(기본 탑재, managers/config/design.md). process-manager가 실행한다:
//     config-manager --config <데이터>/state/workers/config-manager.json
//
// 구조: 호스트(libworker Host) + 모듈 config(lib/config). 스레드 config.pm·config.io·config.pub
// exit: 0 정상 정지, 1 설정 오류, 3 자체 자원 실패, 4 내부 실패(스레드 예외·멈춤)
#include "config/module.hpp"

int main(int argc, char** argv) {
    worker::Host host("config-manager");
    host.add<config::Module>();
    return host.run(argc, argv);
}
