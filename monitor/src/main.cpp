// sysmon: 보드 상태 감시 워커(type sysmon, id system-monitor, 기본 탑재). monitor/design.md
//
// Manager가 실행한다(monitor/worker-types.json):
//     sysmon --config <데이터>/state/workers/system-monitor.json
//
// 구조: 호스트(libworker Host) + 모듈 sysmon(lib/sysmon). 스레드 없음(가벼운 감시라 호스트 루프 하나)
// exit: 0 정상 정지, 1 설정 오류, 2 tegrastats 없음·비정상 종료·멈춤·수집기 멈춤(재시작으로 낫는 경우라 스스로 종료),
//       3 소켓 등 자체 자원 실패, 4 내부 실패. 시그널 SIGALRM은 자체 워치독 만료
#include "sysmon/module.hpp"
#include "worker/runtime.hpp"

int main(int argc, char** argv) {
    worker::Host host("sysmon");
    host.legacy_keys("sysmon", {"interval_ms"});  // 옛 Python 판 형식(맨 위 interval_ms)도 받는다
    auto& mon = host.add<sysmon::Module>();
    const int rc = host.run(argc, argv);
    // 장치 오류는 exit 2(design.md 9.2). 호스트는 시작 실패 3·실패 4로 끝내므로 여기서 바꾼다. 정지 요청 중이면 정상 정지
    if (rc != 0 && mon.device_error()) return worker::stop_requested() ? 0 : 2;
    return rc;
}
