// pm.res: 워커별 CPU·메모리·프로세스 수·스레드 수 (design.md 8.2 resources, 9.5).
//
// 워커는 자기 세션의 그룹 리더라 프로세스 그룹(pgid = 워커 pid) 단위로 /proc를 합산한다(ffmpeg 같은 자식 포함).
// 루프는 1초마다 지금 pid 목록을 넣고(set_pgids), 이 스레드가 재서 최신 값 하나에 둔다(take).
// 루프가 늦으면 덮어쓴다: 지난 값은 쓸모없다.
// 이 스레드는 스레드 감시(ThreadWatch)도 본다: 제어 루프가 멈추면 루프는 스스로 알 수 없으므로.
#pragma once

#include <map>
#include <mutex>
#include <vector>

#include "json_util.hpp"
#include "worker/thread.hpp"

namespace pm {

// /proc 한 번 읽어 {pgid: {"cpu_pct", "rss_mb", "procs", "threads"}}. cpu_pct는 두 번째 표본부터
class ResourceSampler {
public:
    std::map<int, Json> sample(const std::vector<int>& pgids, double now);

private:
    std::map<int, std::pair<long long, double>> prev_;  // pgid → (ticks, 시각)
};

class Resources {
public:
    explicit Resources(worker::ThreadWatch& watch);
    ~Resources();

    void start();
    void stop();
    void set_pgids(std::vector<int> pgids);       // 루프
    bool take(std::map<int, Json>& out);          // 루프: 새 값이 있으면 true
    int wake_fd() const { return efd_; }

private:
    void run(worker::WorkerThread& t);

    worker::ThreadWatch& watch_;
    worker::WorkerThread thread_;
    std::mutex mu_;
    std::vector<int> pgids_;
    std::map<int, Json> latest_;
    bool fresh_ = false;
    int efd_ = -1;
};

}  // namespace pm
