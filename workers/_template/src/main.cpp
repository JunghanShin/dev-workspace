// example-worker: 워커 틀의 예제 워커 (type: example_worker).
// 새 워커는 dev/new-worker.sh로 이 틀을 복사해 이름을 바꾼 뒤, 모듈 자리에 그 워커의 일을 만든다(필요 없는 모듈은 뗀다).
//
// 실행 (Manager 또는 단독, 워크스페이스 design.md 6.6):
//     example-worker --config <파일>
//
// 이 파일은 모듈을 만들어 잇기만 한다. 워커 계약(설정, 워치독, 스레드 감시, heartbeat, 명령 소켓, 종료)은
// 호스트(libworker worker::Host)가 맡는다(6.6.3). 워크스페이스 연동은 빌드 옵션 WORKER_WORKSPACE와 workspace/ 폴더에만 있다.
#include "peer/module.hpp"
#include "processor/module.hpp"
#include "source/module.hpp"

int main(int argc, char** argv) {
    worker::Host host("example-worker");
    auto& src = host.add<source::Module>();       // 데이터 만들기(포트 SourcePort를 내줌)
    host.add<processor::Module>(src.port());      // 워커 안 연결: 포트로만
    host.add<peer::Module>();                     // 워커 간 연결: 설정 peer의 통로로(비우면 꺼짐)
    return host.run(argc, argv);
}
