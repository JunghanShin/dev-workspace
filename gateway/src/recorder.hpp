// 명령 기록(gw.log 스레드, docs/gateway.md 2·3장): 쓰기 요청마다 한 줄 → <데이터>/records/gateway/gateway-<부팅>.jsonl
//
//   요청 스레드는 큐에 넣기만 한다(1024, 차면 버리고 셈). 파일 쓰기는 gw.log 하나가 한다.
//   파일 상한: 하나 64 MiB(넘으면 gateway-<부팅>-<ms>.jsonl로 이름을 바꾸고 새로), 전체 512 MiB(오래된 것부터 지움).
//   logger 기록(<데이터>/records/*.jsonl)과 폴더를 나눈다: logger의 용량 정리와 /records/commands(마지막 파일 3개)에
//   섞이지 않게.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "pipeline.hpp"
#include "text.hpp"

namespace gw {

struct RecorderLimits {
    long long file_bytes = 64LL << 20;
    long long total_bytes = 512LL << 20;
    size_t queue = 1024;
};

// 부팅 id 앞 8자(logger와 같은 규칙, 못 읽으면 noboot00)
std::string boot_id8();
// 기록 시각: 2026-11-23T10:00:00.123+09:00 (logger와 같은 모양)
std::string iso_now();

class FileRecorder : public Recorder {
public:
    FileRecorder(std::string dir, std::string boot8, RecorderLimits lim = RecorderLimits{});
    ~FileRecorder() override;
    void start();                  // gw.log
    void stop(double timeout_s);   // 남은 줄을 쓰고 끝낸다(상한)
    void record(RecordFields f) override;
    Json stats();
    std::string current_path() const { return dir_ + "/gateway-" + boot8_ + ".jsonl"; }

private:
    void run();
    void write_line(const std::string& line);
    bool ensure_open();
    void rotate();
    void prune();
    void close_file();

    const std::string dir_, boot8_;
    const RecorderLimits lim_;
    std::mutex mu_;  // q_, stop_ (I/O 없음)
    std::condition_variable cv_, done_cv_;
    std::deque<RecordFields> q_;
    bool stop_ = false, finished_ = false;
    std::atomic<unsigned long long> written_{0}, dropped_{0}, errors_{0};
    std::thread t_;
    // gw.log 혼자 쓰는 것
    int fd_ = -1;
    long long size_ = 0;
    double last_sync_ = 0, last_err_log_ = -1e9;
    bool dirty_ = false;
};

}  // namespace gw
