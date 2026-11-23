// 제어 루프 (docs/process-manager.md 2.1). 상태는 Supervisor 하나가 가지고, 이 루프 스레드만 부른다.
//
//   poll: signalfd(SIGCHLD·SIGTERM·SIGINT·SIGHUP) · notify.sock(heartbeat) · API 소켓(클라이언트마다)
//         · pm.store 결과 · pm.res 결과 · pm.pub 끊긴 구독자. 대기 상한 = 다음 타이머(최대 tick_s)
//
// API(design.md 6.1·6.2): 요청 1건 = 연결 1개, JSON 한 줄. 읽기 1 s 상한, 64 KiB 상한.
// 쓰기 op는 한 번에 하나씩 차례로 처리한다: 검사 → 워커 폴더 파일(pm.store) → 상태 바꿈 → 실행 기록 파일
// (pm.store) → 응답. 다음 쓰기 op는 앞 것이 끝난 상태를 보고 검사한다(Python 판처럼 순서대로).
// 읽기 op는 기다리지 않고 바로 답한다. set_access_mode(운용/정비 모드, REST 소켓만)는 메모리 값만 바꿔 바로 답한다.
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "json_util.hpp"
#include "publisher.hpp"
#include "resources.hpp"
#include "store.hpp"
#include "supervisor.hpp"

namespace pm {

struct AppConfig {
    Json api;                      // settings["api"]
    double tick_s = 0.2;
    long long log_max_bytes = 10 << 20;
    int log_backups = 3;
    std::string run_dir, logs_dir, notify_path;
    std::set<std::string> api_workers;
    bool product = false, config_write = true;
};

class App {
public:
    App(AppConfig cfg, Supervisor& sup, StoreThread& store, Publisher& pub, Resources& res, worker::ThreadWatch& watch);
    ~App();

    bool open(std::string& err);  // notify·API 소켓
    int run();                    // SIGTERM·SIGINT까지. 워커를 정리하고 돌아온다

private:
    struct Conn {
        int fd = -1;
        std::optional<std::string> client;  // 없으면 manager.sock(로컬 도구, 읽기 전용)
        enum class St { Reading, Queued, Busy, Writing } st = St::Reading;
        std::string in, out;
        double deadline = 0;
        Json req;
    };
    struct Sub {
        std::optional<std::string> client;
        double period = 1.0, next = 0;
    };

    double now() const;
    void on_event(const std::string& event, const std::string& key, const Json& data);
    Json system_status();
    Json set_access_mode(const Json& req, const std::optional<std::string>& client);
    void accept_on(int lfd, const std::optional<std::string>& client);
    void read_conn(Conn& c);
    void handle_line(Conn& c, const std::string& line);
    void reply(Conn& c, const Json& resp);
    void write_conn(Conn& c);
    void close_conn(int fd);
    Json dispatch_read(const Json& req);
    std::optional<Json> check_write(const Json& req, const std::optional<std::string>& client);
    void pump_writes();
    void run_write(int fd);
    void finish_write(int fd, const Json& resp);
    void record_command(const Json& req, const std::optional<std::string>& client, const Json& resp);
    void subscribe(Conn& c, const Json& req);
    void send_snapshots(double t);
    void read_notify();
    bool read_signals();  // 끝내라는 신호면 true
    void reap();
    void housekeeping(double t);
    void step(int timeout_ms, bool serve_api);

    AppConfig cfg_;
    Supervisor& sup_;
    StoreThread& store_;
    Publisher& pub_;
    Resources& res_;
    worker::ThreadWatch& watch_;
    int loop_slot_ = -1;

    int sigfd_ = -1, notify_fd_ = -1;
    std::vector<std::pair<int, std::optional<std::string>>> listeners_;
    std::vector<std::string> client_names_;
    std::map<int, Conn> conns_;
    std::deque<int> write_fifo_;
    bool write_busy_ = false;
    std::map<int, Sub> subs_;
    int next_sub_ = 1;
    long long command_seq_ = 0;
    std::map<std::string, double> last_seen_;
    double last_house_ = 0;
    std::vector<int> last_pgids_;
    bool stopping_ = false;
    std::string access_mode_ = "operation";  // 게이트웨이가 알려 줌(set_access_mode). 저장하지 않음
};

}  // namespace pm
