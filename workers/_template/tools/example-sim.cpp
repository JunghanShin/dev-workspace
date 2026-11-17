// example-sim: 가짜 장치 프로그램의 틀(가짜 장치 모드, 워크스페이스 design.md 6.6.8).
//
// 실제 워커에서는 여기서 장치 흉내를 낸다: 장치가 쓰는 통로(UDP·TCP·시리얼 대신 pty 등)를 127.0.0.1에 열고, 워커의
// 요청에 실제 장치와 같은 형식으로 답하거나(예: UDP 명령에 응답) 장치처럼 데이터를 보낸다(예: 주기 텔레메트리).
// 시험의 가짜 장치와 같은 구현을 헤더 하나(lib/<모듈>/test/fake_<장치>.hpp)로 함께 쓰면 시험과 가짜 장치 모드가 같아진다.
// 라이브러리를 바꿔 끼우는 장치(SDK)는 프로그램 없이 설정만으로 한다(예: sdk_lib = 가짜 라이브러리).
//
// 이 틀은 장치가 없으므로 시작·정지만 보여 준다. 워커가 자식으로 띄우고(설정 "sim": {"cmd": ...}),
// 죽으면 2초 뒤 다시 띄우고, 워커가 끝나면 함께 끝낸다(libworker Host).
//
//   example-sim [포트]
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

static std::atomic<bool> g_stop{false};

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 0;
    struct sigaction sa{};
    sa.sa_handler = [](int) { g_stop = true; };
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    std::printf("example-sim: fake device ready (port %d)\n", port);
    std::fflush(stdout);
    while (!g_stop) usleep(100 * 1000);
    std::printf("example-sim: stopped\n");
    return 0;
}
