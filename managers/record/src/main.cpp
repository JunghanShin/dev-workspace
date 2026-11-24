// record-manager: 기록 매니저(기본 탑재, managers/record/design.md). process-manager가 실행한다:
//     record-manager --config <데이터>/state/workers/record-manager.json
//
// 구조: 호스트(libworker Host) + 모듈 record(lib/record). 스레드 record.pm·record.write·record.find
// exit: 0 정상 정지, 1 설정 오류, 3 자체 자원 실패, 4 내부 실패(스레드 예외·멈춤)
#include "record/module.hpp"

int main(int argc, char** argv) {
    worker::Host host("record-manager");
    host.add<record::Module>();
    return host.run(argc, argv);
}
