// replay 모듈: 데이터 버스 녹화·재생(워크스페이스 design.md 6.7, 장비 없이 개발 6.6.8 보조). ② 워커 실행 층.
//
//   녹화: replay.record_start {source: <워커 id> 또는 socket, name, seconds?} → 스레드 "replay.rec"가 그 워커의 데이터 버스를
//         순서대로 전부(InOrder) 받아 녹화 폴더에 쓴다(유닛 Writer). replay.record_stop
//   재생: replay.play_start {name, speed=1, loop=false} → 스레드 "replay.play"가 녹화를 원래 시간 간격(배속)대로 이 워커의
//         데이터 버스(default_socket(id, "frames"))로 다시 낸다. 받는 워커는 설정 peer를 이 워커 id로 바꾸면 실제 장치 대신
//         녹화본을 받는다. replay.play_stop
//   관리: replay.list, replay.delete {name}. 녹화 폴더 전체 용량 상한(max_total_mb)을 넘으면 녹화를 멈춘다
//
//   스레드: replay.rec·replay.play 둘 다 start()에서 한 번 만들어 워커가 끝날 때까지 돈다(스레드 등록은 시작 때만,
//   libworker ThreadWatch 규칙). 일이 없으면 쉬며 beat하고, 명령이 넘긴 일(Job)을 받아 처리한 뒤 다시 쉰다.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "framebus/framebus.hpp"
#include "replay/recording.hpp"
#include "worker/host.hpp"

namespace replay {

struct Config {
    std::string dir;              // 녹화 폴더. 비우면 <데이터>/records/framebus(워크스페이스 밖이면 $XDG_RUNTIME_DIR·/tmp 아래)
    double max_total_mb = 4096;   // 녹화 폴더 전체 상한
    double max_record_sec = 600;  // 녹화 하나 최대 길이(seconds를 안 주면 이 값)
    int bus_slots = 8;            // 재생 데이터 버스 슬롯 수
    std::string bus_socket;       // 재생 데이터 버스 소켓. 비우면 default_socket(id, "frames")
};

class Module : public worker::Module {
public:
    std::string name() const override { return "replay"; }
    void configure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return rec_frames_.load() + play_frames_.load(); }
    bool failed(std::string* why) const override;
    void stop() override;

    static Config parse(const worker::Json& c, const std::string& worker_id);

private:
    struct RecJob {
        std::string name, socket, label, dir;
        double seconds = 0, max_bytes = 0;
        uint64_t already = 0;
    };
    struct PlayJob {
        std::shared_ptr<Reader> reader;
        std::unique_ptr<framebus::Producer> bus;
        double speed = 1;
        bool loop = false;
        int slots = 8;
    };

    worker::Json record_start(const worker::Json& args);
    worker::Json play_start(const worker::Json& args);
    void stop_record();  // 끝날 때까지(최대 1초) 기다린다
    void stop_play();
    void rec_body(worker::WorkerThread& t);
    void play_body(worker::WorkerThread& t);
    void run_record(worker::WorkerThread& t, const RecJob& job);
    void run_play(worker::WorkerThread& t, PlayJob& job);

    Config cfg_;
    std::unique_ptr<worker::WorkerThread> rec_thread_, play_thread_;
    mutable std::mutex mu_;  // 아래 일(Job)·이름·오류 글자(스레드 ↔ 워커 루프)

    // 녹화(replay.rec)
    std::unique_ptr<RecJob> rec_job_;  // 워커 루프가 넣고 스레드가 가져간다
    std::string rec_name_, rec_source_, rec_error_;
    std::atomic<bool> rec_active_{false}, rec_cancel_{false}, rec_link_{false};
    std::atomic<uint64_t> rec_frames_{0}, rec_bytes_{0};

    // 재생(replay.play)
    std::unique_ptr<PlayJob> play_job_;
    std::string play_name_, play_error_;
    double play_speed_ = 1;
    bool play_loop_ = false;
    uint64_t play_total_ = 0;
    std::atomic<bool> play_active_{false}, play_cancel_{false};
    std::atomic<uint64_t> play_frames_{0}, play_loops_{0};
    std::atomic<int> play_consumers_{0};
};

}  // namespace replay
