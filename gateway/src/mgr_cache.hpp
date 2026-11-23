// Manager 캐시(gw.mgr 스레드, docs/gateway.md 2장): Manager 구독 하나(topics workers·system)를 유지하며
// 워커 상태·시스템 상태 최신 값을 들고 있다. 화면이 1초마다 묻는 GET /workers·/workers/{id}·/system을 여기서 답해
// process-manager의 list 처리를 줄인다.
//
//   새로움: 마지막 system 이벤트(Manager가 주기마다 워커 전체 + system을 보냄)가 fresh_s(3 s) 안에 왔을 때만 답한다.
//           아니면 부르는 쪽이 지금처럼 Manager에 직접 묻는다(같은 답 모양). 쓰기 op는 늘 직접.
//   순서:   GET /workers는 Manager의 list 순서 그대로여야 해서, 주기 한 바퀴에 본 워커 id가 아는 목록과 다르면(처음,
//           워커가 더해짐·빠짐) list를 한 번 직접 불러 순서를 다시 배운다.
//   끊김:   연결 실패·끊김·5 s 동안 아무 줄 없음 → 1 s 뒤 다시. 모든 기다림에 상한(연결 1 s, 읽기 0.5 s씩).
//
// 공유 상태(잠금 하나, I/O 없음): 워커 값·순서·system·갱신 시각. 줄 읽기·해석·list 호출은 잠금 밖.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "sys.hpp"
#include "text.hpp"
#include "worker/thread.hpp"

namespace gw {

class ManagerCache {
public:
    ManagerCache(std::string socket, double connect_s, double response_s, double fresh_s = 3.0, double period_ms = 1000);
    ~ManagerCache();
    void start(worker::ThreadWatch* watch = nullptr);
    void stop();

    // 새로우면 true와 답(Manager list·get·system의 data와 같은 모양)
    bool workers(Json& out);
    bool worker(const std::string& id, Json& out);
    bool system(Json& out);
    Json state();  // {"connected", "fresh", "age_s", "workers", "events", "direct_lists"}

    // system 이벤트마다(gw.mgr 스레드, 잠금 밖). 게이트웨이가 운용/정비 모드를 맞출 때 쓴다
    std::function<void(const Json& system)> on_system;

private:
    void run();
    void session();               // 연결 하나가 끝날 때까지
    void handle(const Json& msg); // 줄 하나
    void relearn_order();         // list 직접 호출(잠금 밖)
    bool fresh_locked(double now) const;
    void sleep_s(double s);

    const std::string socket_;
    const double connect_s_, response_s_, fresh_s_, period_ms_;
    ManagerClient direct_;
    worker::ThreadWatch* watch_ = nullptr;
    int slot_ = -1;
    std::thread t_;
    std::atomic<bool> stop_{false};
    std::mutex sleep_mu_;
    std::condition_variable sleep_cv_;

    std::mutex mu_;  // 아래 (I/O 없음)
    std::map<std::string, Json> values_;
    std::vector<std::string> order_;
    bool order_ok_ = false, connected_ = false;
    Json system_;
    double updated_ = -1;  // mono, 마지막 system 이벤트
    unsigned long long events_ = 0, direct_lists_ = 0;

    // gw.mgr 혼자 쓰는 것
    std::set<std::string> seen_;  // 지난 system 이벤트 뒤로 본 워커 id
    bool need_order_ = true;
};

}  // namespace gw
