// replay-worker: 데이터 버스 녹화·재생(type replay_worker). workers/replay/design.md
//
// 실행 (Manager 또는 단독, 워크스페이스 design.md 6.6):
//     replay-worker --config <파일>
//
// 구조: 호스트(libworker Host) + 모듈 replay(lib/replay). 녹화 스레드 replay.rec, 재생 스레드 replay.play
// 쓰는 곳: 카메라 없이 영상 처리·AI 워커 개발(녹화본을 실제 장치처럼 재생), 같은 장면 반복 시험, 현장 문제 재현
#include "replay/module.hpp"

int main(int argc, char** argv) {
    worker::Host host("replay-worker");
    host.add<replay::Module>();
    return host.run(argc, argv);
}
