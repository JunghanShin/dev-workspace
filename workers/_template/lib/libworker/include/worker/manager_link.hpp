// 매니저용 도구(워크스페이스 docs/managers.md, libworker 0.13). 워커도 쓸 수 있다.
//
//   ManyInputs           여러 워커 입력 목록: 입력 파일(WORKSPACE_INPUTS_FILE)의 "many" 칸
//                        {"many": {"<입력>": [{"worker": "<id>"}, ...]}}. process-manager가 워커가 늘거나 줄면
//                        파일만 다시 쓰므로(다시 시작하지 않음) refresh()를 1 s쯤마다 불러 바뀌었나 본다
//   ManagerSubscription  process-manager 구독(Manager API subscribe): 스레드 하나 + 크기 제한 큐. 끊기면 retry_sec마다
//                        다시 붙는다. 받은 줄은 키 순서를 지킨 JSON(ordered_json)으로 넘긴다(기록이 받은 모양 그대로 쓰게)
//
// 둘 다 워크스페이스가 없으면(파일·소켓 없음) 빈 목록·끊김으로 조용히 돈다(단독 실행)
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "worker/queue.hpp"
#include "worker/thread.hpp"

namespace worker {

class ManyInputs {
public:
    // path가 비면 환경 변수 WORKSPACE_INPUTS_FILE. self_id: 목록에서 뺄 자기 id
    explicit ManyInputs(std::string path = "", std::string self_id = "");
    // 파일이 바뀌었으면 다시 읽는다. 목록이 하나라도 바뀌었으면 true
    bool refresh();
    // 입력의 워커 id들(이름순, 자기 제외)
    std::vector<std::string> workers(const std::string& input) const;
    const std::string& path() const { return path_; }

private:
    std::string path_, self_;
    long long mtime_ns_ = -1;
    std::map<std::string, std::vector<std::string>> lists_;
};

class ManagerSubscription {
public:
    struct Options {
        std::string socket;                                  // 비우면 $WORKSPACE_MANAGER_SOCKET, 그것도 없으면 $WORKSPACE_DATA/run/manager.sock
        std::vector<std::string> topics = {"workers", "system"};
        int period_ms = 1000;                                // 바뀌지 않아도 이 주기로 전체가 온다
        double retry_sec = 2.0;                              // 끊긴 뒤 다시 붙는 간격
        double read_timeout_sec = 0;                         // 이만큼 조용하면 끊긴 것(0이면 max(5, 주기 5배))
        size_t queue = 256;                                  // 넘치면 오래된 것부터 버림(다음 주기에 전체가 다시 옴)
        std::string thread_name = "pm.sub";
    };
    struct Event {
        enum Kind { Connected, Line, Down } kind = Line;
        nlohmann::ordered_json msg;  // Line: {"event": ..., "data": ...}
        std::string reason;          // Down: 까닭(연결 실패면 시도마다 하나)
    };

    ManagerSubscription(ThreadWatch& watch, Options opt);
    ~ManagerSubscription();
    ManagerSubscription(const ManagerSubscription&) = delete;
    ManagerSubscription& operator=(const ManagerSubscription&) = delete;

    void start();
    void stop();
    int wake_fd() const { return queue_.wake_fd(); }
    std::optional<Event> next() { return queue_.try_pop(); }
    bool connected() const { return connected_.load(); }
    const std::string& socket() const { return opt_.socket; }
    WorkerThread& thread() { return thread_; }

private:
    void run(WorkerThread& t);
    Options opt_;
    BoundedQueue<Event> queue_;
    WorkerThread thread_;
    std::atomic<bool> connected_{false};
};

}  // namespace worker
