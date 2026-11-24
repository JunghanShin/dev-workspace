// payload-manager: 모듈 운용 매니저(기본 탑재, managers/payload/design.md). process-manager가 실행한다:
//     payload-manager --config <데이터>/state/workers/payload-manager.json
//
// 구조: 호스트(libworker Host) + 모듈 payload(lib/payload). 스레드 payload.pm·payload.io·payload.sys·payload.ctl·payload.pub
// exit: 0 정상 정지, 1 설정 오류, 3 자체 자원 실패, 4 내부 실패(스레드 예외·멈춤)
#include "payload/module.hpp"

int main(int argc, char** argv) {
    worker::Host host("payload-manager");
    host.add<payload::Module>();
    return host.run(argc, argv);
}
