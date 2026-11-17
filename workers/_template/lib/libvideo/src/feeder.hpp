// 프레임 버스 입력 (design.md 2장 입력 framebus). 프레임 버스에서 최신 프레임을 받아 gst-launch 표준 입력으로 넘긴다.
//
//   프레임 버스(생산자: 장치 워커) ──▶ Feeder 스레드 ──파이프──▶ gst-launch(fdsrc → rawvideoparse → nvvidconv → NVENC)
//
// - fps 간격으로 하나씩 보낸다. 그 사이 온 프레임은 최신 것만 남기고 버린다(최신 우선, 지연이 쌓이지 않게)
// - 파이프가 막히면(gst 멈춤) 1초 뒤 포기하고 끝낸다 → 파이프가 닫혀 gst가 끝나고 Stream이 다시 시작한다
// - 프레임 버스가 끊기면(장치 워커 재시작) 끝낸다 → 같은 방식으로 다시 연결
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "framebus/framebus.hpp"

namespace video {

class Feeder {
public:
    // out_fd: gst 표준 입력 파이프 쓰기 쪽(이 객체가 닫는다)
    Feeder(std::unique_ptr<framebus::Consumer> consumer, int out_fd, int fps);
    ~Feeder();
    Feeder(const Feeder&) = delete;
    Feeder& operator=(const Feeder&) = delete;

    bool ended() const { return ended_.load(); }
    uint64_t fed() const { return fed_.load(); }
    uint64_t skipped() const { return consumer_ ? consumer_->skipped() : 0; }
    std::string error() const;
    const framebus::Format& format() const { return format_; }

private:
    void run();
    bool write_all(const uint8_t* p, size_t n);

    std::unique_ptr<framebus::Consumer> consumer_;
    framebus::Format format_;
    int out_ = -1;
    int fps_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> ended_{false};
    std::atomic<uint64_t> fed_{0};
    mutable std::mutex mu_;
    std::string error_;
};

}  // namespace video
