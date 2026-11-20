// 워커 띄우기·신호 (design.md 9.1, 9.3). Spawner 포트 구현.
//
// posix_spawn이 아니라 fork+exec를 쓴다: exec 전에 PR_SET_PDEATHSIG(Manager가 죽으면 워커도 SIGTERM),
// nice·CPU 지정을 해야 하는데 posix_spawn에는 그 자리가 없다. 자식은 exec까지 시스템 호출만 한다
// (fork 뒤 malloc·잠금 없음: 다른 스레드가 쥔 잠금에 걸리지 않게). 준비(argv·env·PATH 찾기)는 부모에서.
#pragma once

#include "ports.hpp"

namespace pm {

class PosixSpawner : public Spawner {
public:
    int spawn(const SpawnSpec& spec, std::string& err) override;
    void signal(int pid, int sig) override;
    void kill_group(int pgid, int sig) override;
    bool executable(const std::string& path) override;
};

}  // namespace pm
