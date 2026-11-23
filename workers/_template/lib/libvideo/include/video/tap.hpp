// 프레임 탭 (libvideo): 카메라 영상(RTSP)을 풀어 원본 프레임을 같은 보드의 프레임 버스로 낸다. 인식 같은 영상 처리
// 워커가 프레임 버스에서 받아 간다(워크스페이스 design.md 6.7). 영상 장치 워커가 자기 영상을 내줄 때 붙인다.
//
//   장치 워커 ─┬─ Streams(송출, 따로)
//             └─ Tap ── 자식: gst-launch(RTSP → NVDEC → 원본 BGRx) ── 파이프 ── 받기 스레드 ── framebus::Producer
//
// - 송출과 따로 띄운다(영상을 한 번 더 풀지만 서로 영향이 없다: 탭이 죽거나 멈춰도 송출은 그대로)
// - 받기 스레드는 프레임을 프레임 버스 슬롯에 바로 읽어 넣는다(복사 한 번). 빈 슬롯이 없으면 그 프레임은 버린다
// - 프레임 시각(ts_mono_ns)은 파이프에서 다 받은 순간의 CLOCK_MONOTONIC(카메라가 찍은 순간보다 영상 지연만큼 늦다)
// - 자식이 끝나거나 stall_sec 동안 프레임이 없으면 retry_sec 뒤 다시 띄운다. 프레임 버스는 그대로 둬서 소비자는 끊기지 않는다
// - 설정 칸 "tap": {"enabled", "source", "source_codec", "width", "height", "pixel", "fps", "slots", "socket", ...}
//   지표 tap_link, tap_fps, tap_frames, tap_dropped, tap_consumers, tap_restarts
#pragma once

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "framebus/framebus.hpp"
#include "nlohmann/json.hpp"
#include "worker/host.hpp"

namespace video {

struct TapConfig {
    bool enabled = false;            // false면 띄우지 않음(설정에 칸이 없을 때도)
    std::string source;              // 카메라 RTSP(장치 워커가 기본값을 준다)
    std::string source_codec = "h264";
    int width = 1280;                // 내보낼 크기(카메라 영상을 이 크기로 맞춤)
    int height = 720;
    std::string pixel = "BGRx";      // BGRx | RGBA | GRAY8 (한 픽셀 4·4·1바이트. BGRx는 numpy[:, :, :3]이 그대로 BGR)
    int fps = 30;
    int slots = 4;                   // 프레임 버스 슬롯 수
    std::string socket;              // 프레임 버스 알림 소켓. 비우면 worker::default_socket(워커 id, "frames")
    double retry_sec = 2;
    double stall_sec = 5;
    std::string gst_launch = "gst-launch-1.0";

    int bytes_per_pixel() const { return pixel == "GRAY8" ? 1 : 4; }
};

// 설정 칸 "tap"을 defaults 위에 읽는다. 알 수 없는 키·잘못된 값은 worker::ConfigError
TapConfig parse_tap(const nlohmann::json& tap, const TapConfig& defaults);
// gst-launch 명령(RTSP → NVDEC → nvvidconv → 원본 → 표준 출력)
std::vector<std::string> gst_tap_command(const TapConfig& c);

class Tap {
public:
    using Clock = std::chrono::steady_clock;

    explicit Tap(TapConfig config);
    ~Tap();
    Tap(const Tap&) = delete;
    Tap& operator=(const Tap&) = delete;

    // 프레임 버스를 연다(enabled일 때). 실패하면 false와 error
    bool open(std::string* error);
    void on_timer(Clock::time_point now);
    Clock::time_point next_wakeup(Clock::time_point now) const;
    void stop();  // 자식·받기 스레드 정리, 프레임 버스 닫기

    const TapConfig& config() const { return c_; }
    bool linked() const { return linked_; }
    double fps() const { return fps_; }
    unsigned long long frames() const { return frames_.load(); }
    unsigned long restarts() const { return restarts_; }
    framebus::Stats bus_stats() const;
    nlohmann::json status() const;

private:
    void start(Clock::time_point now);
    void end_child();
    void failed(Clock::time_point now, const std::string& why);
    void read_loop(int fd);

    TapConfig c_;
    std::unique_ptr<framebus::Producer> bus_;
    pid_t pid_ = -1;
    std::thread reader_;
    std::atomic<bool> stop_reader_{false};
    std::atomic<unsigned long long> frames_{0};
    std::atomic<long long> last_frame_ns_{0};  // 마지막 프레임 시각(steady, ns). 받기 스레드가 씀
    std::atomic<bool> reader_done_{false};
    bool linked_ = false;
    double fps_ = 0;
    unsigned long long fps_frames_ = 0;
    Clock::time_point fps_at_{};
    Clock::time_point started_{};
    Clock::time_point retry_at_{};
    unsigned long restarts_ = 0;
    std::string last_error_;
    mutable std::mutex mu_;
};

// 워커 모듈 "tap". 장치 워커가 붙인다: host.add<video::TapModule>([&cam] { return cam.tap_defaults(); });
class TapModule : public worker::Module {
public:
    using Defaults = std::function<TapConfig()>;
    explicit TapModule(Defaults defaults = {}) : defaults_(std::move(defaults)) {}

    std::string name() const override { return "tap"; }
    void configure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    void on_timer(worker::TimePoint now) override {
        if (tap_) tap_->on_timer(now);
    }
    worker::TimePoint next_wakeup(worker::TimePoint now) const override {
        return tap_ ? tap_->next_wakeup(now) : now + std::chrono::seconds(1);
    }
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    void stop() override {
        if (tap_) tap_->stop();
    }

private:
    Defaults defaults_;
    TapConfig cfg_;
    std::unique_ptr<Tap> tap_;
};

}  // namespace video
