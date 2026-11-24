// 모듈 record: 기록 매니저(managers/record/design.md). 코어(PlatformRecorder, RecordFile, find)를 스레드·큐로 감싼다.
//
// 설정 config["record"]:
//   dir             기록 폴더(기본 $WORKSPACE_DATA/records)
//   manager_socket  process-manager 읽기 소켓(기본 $WORKSPACE_MANAGER_SOCKET 또는 $WORKSPACE_DATA/run/manager-logger.sock)
//   interval_s      워커 상태를 바뀌지 않아도 남기는 간격(기본 1)
//   max_file_mb, max_total_mb  파일 하나·전체 상한(기본 64, 2048)
//   fsync_s         fsync 간격(기본 5)
//   topics          주제 규칙 [{"match": "<보낸 곳>.<주제> 글롭", "interval_ms": n}](기본 [{"match": "*", "interval_ms": 1000}])
//   exclude         남기지 않을 "<보낸 곳>.<주제>" 글롭 목록
//
// 스레드(design.md 1장): record.pm(process-manager 구독, libworker ManagerSubscription) → 큐 → 코어(호스트 루프) → 쓰기 큐 → record.write(파일)
//                        명령 find → 찾기 큐 → record.find → 결과 큐 → 코어가 답함
//                        워커·매니저마다 rs.<id>(스트림 구독, libworker Subscriber) → 큐 → 코어(주제·이벤트, 3단계)
// 구독할 곳: 입력 파일(WORKSPACE_INPUTS_FILE)의 "many".streams(process-manager가 씀, docs/managers.md 5장).
//            1 s마다 파일이 바뀌었나 보고 구독을 더하고 뺀다(다시 시작하지 않음)
// 명령: find(읽기). 지표: lines, bytes, drops, files, write_errors, pm_link, disk_free_mb, finds, subs, subs_up, topics, events
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "record/platform.hpp"
#include "record/store.hpp"
#include "record/topics.hpp"
#include "worker/peer.hpp"
#include "worker/host.hpp"
#include "worker/manager_link.hpp"
#include "worker/queue.hpp"
#include "worker/thread.hpp"

namespace record {

struct Settings {
    std::string dir, manager_socket;
    double interval_s = 1.0;
    double max_file_mb = 64, max_total_mb = 2048;
    double fsync_s = 5.0;
    Json topics, exclude;  // 주제 규칙(null이면 기본)
};

// 설정 칸 + 환경 변수 기본값 → Settings(잘못되면 worker::ConfigError)
Settings read_settings(const worker::Json& config);

class Module : public worker::Module {
public:
    Module();
    ~Module() override;

    std::string name() const override { return "record"; }
    void configure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    void add_pollfds(std::vector<pollfd>& fds) override;
    void on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    void on_timer(worker::TimePoint now) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return lines_.load(); }
    bool failed(std::string* why) const override;
    void stop() override;

    const Settings& settings() const { return s_; }

private:
    struct FindReq {
        uint64_t id;
        Json args;
    };
    struct FindResult {
        uint64_t id;
        Json result;
    };

    void put(const Json& rec);  // 기록 하나 → 줄 → 쓰기 큐
    void put(const Json& rec, int64_t t_mono_ns);
    void sync_subs();           // 입력 파일의 many.streams와 구독을 맞춤
    void drain();
    void run_write(worker::WorkerThread& t);
    void run_find(worker::WorkerThread& t);

    Settings s_;
    std::unique_ptr<PlatformRecorder> platform_;
    std::unique_ptr<RecordFile> file_;  // start에서 열고 그 뒤로는 쓰기 스레드만 씀
    std::string boot_;

    worker::BoundedQueue<std::string> write_q_{4096, worker::Overflow::Reject};
    worker::BoundedQueue<FindReq> find_q_{4, worker::Overflow::Reject};
    worker::BoundedQueue<FindResult> found_q_{8, worker::Overflow::DropOldest};
    std::unique_ptr<worker::ManagerSubscription> pm_;  // process-manager 구독(libworker, 스레드 record.pm)
    std::unique_ptr<worker::ManyInputs> many_;          // 여러 워커 입력 목록(libworker)
    std::unique_ptr<worker::WorkerThread> write_thread_, find_thread_;

    std::map<uint64_t, worker::Reply> pending_;  // 코어만
    worker::Host* host_ = nullptr;
    TopicRules rules_;
    std::map<std::string, std::unique_ptr<worker::Subscriber>> subs_;  // 보낸 곳 id → 구독(코어만)
    uint64_t topics_ = 0, events_ = 0;
    uint64_t next_find_ = 1;

    std::atomic<uint64_t> lines_{0}, bytes_{0}, drops_{0}, write_errors_{0}, finds_{0};
    std::atomic<long long> disk_free_mb_{-1};
    std::atomic<bool> pm_link_{false};
    mutable std::mutex info_mu_;  // path_, last_error_
    std::string path_, last_error_;
};

}  // namespace record
