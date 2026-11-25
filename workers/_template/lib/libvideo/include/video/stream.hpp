// 영상 송출 모듈 (libvideo). 장치 워커(siyi, ximea 등)가 송출을 자식 프로세스로 띄우고 감시하는 데 쓴다.
//
//   장치 워커 ─┬─ 제어·캡처(자기 프로세스)
//             └─ Streams ─┬─ Stream "main" ── 자식: [gst-launch →] ffmpeg ── 미디어 서버
//                         └─ Stream "proc" ── 자식 ...
//
// - 입력: rtsp(카메라 RTSP 서버) 또는 framebus(같은 보드 프레임 버스, 원본 프레임 → NVENC)
// - 처리: relay(그대로) 또는 encode(NVDEC/원본 → (시각 표시) → NVENC). 저지연 설정은 pipeline.cpp
// - 자식이 끝나거나 진행이 멈추면 그 송출만 retry_sec 뒤 다시 시작한다. 장치 워커의 제어 루프는 영향이 없다
// - 시작·진행 확인·종료는 논블로킹. 장치 워커의 poll 루프에 붙인다(fd, on_readable, on_timer, next_wakeup)
// - 장치 워커 설정의 "streams": [{...}, ...]가 송출 목록. 키는 아래 StreamConfig와 같다
#pragma once

#include <poll.h>
#include <sys/types.h>

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace video {

class Feeder;

struct StreamConfig {
    std::string name = "main";       // 송출 이름(장치 워커 안에서 고유). 명령·지표·로그에 쓴다
    bool autostart = true;           // 장치 워커가 시작할 때 이 송출도 시작(false면 stream_start 명령으로)

    // 입력. "rtsp": 카메라 RTSP. "framebus": 프레임 버스 소비자(source = 알림 소켓 경로, encode만)
    std::string input = "rtsp";
    std::string source;              // 입력 주소(장치 워커가 기본값을 준다)
    std::string source_codec = "h264";
    int fps = 30;

    // 처리. "relay": 그대로. "encode": NVENC로 다시 압축
    std::string mode = "relay";
    int bitrate_kbps = 1500;
    double keyframe_sec = 2;
    double intra_refresh_sec = 0.5;
    bool overlay_time = false;

    // 출력
    std::string url;                 // 필수. rtsp://호스트:포트/경로
    std::string transport = "rtsp_udp";  // rtsp_udp | rtsp_tcp | srt
    int pkt_size = 1200;
    int srt_port = 8890;
    int srt_latency_ms = 300;

    double retry_sec = 2;
    double stall_sec = 5;
    std::string ffmpeg = "ffmpeg";
    std::string gst_launch = "gst-launch-1.0";

    bool encode() const { return mode == "encode"; }
};

// 장치 워커 설정의 "streams"(배열)를 읽는다. 각 항목은 defaults 위에 덮어쓴다(장치 워커가 입력·주소 기본값을 줌).
// 알 수 없는 키, 잘못된 타입·범위, 이름 중복은 worker::ConfigError
std::vector<StreamConfig> parse_streams(const nlohmann::json& streams, const StreamConfig& defaults);

class Stream {
public:
    using Clock = std::chrono::steady_clock;

    explicit Stream(StreamConfig config);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    const StreamConfig& config() const { return c_; }
    int fd() const { return pipe_; }  // ffmpeg progress 파이프. 없으면 -1
    void on_readable(Clock::time_point now);
    void on_timer(Clock::time_point now);
    Clock::time_point next_wakeup(Clock::time_point now) const;
    void stop();  // 자식 정리(SIGTERM, 1초 뒤 SIGKILL)

    // 켜기·끄기(바로 적용, 설정 파일은 그대로). 끄면 자식을 정리하고 다시 시작하지 않는다
    void set_enabled(bool on);
    bool enabled() const { return enabled_; }
    bool linked() const { return linked_; }
    double fps() const { return fps_; }
    static constexpr std::chrono::seconds kFpsWindow{3};
    unsigned long restarts() const { return restarts_; }
    nlohmann::json status() const;
    // 끊김·복구 이벤트(libvideo 0.4): [{severity, code: stream_down|stream_up, text, detail}]. 꺼내면 비움
    std::vector<nlohmann::json> take_events();

private:
    void start(Clock::time_point now);
    void reap(Clock::time_point now);
    void failed(Clock::time_point now, const std::string& why);
    void close_pipe();

    StreamConfig c_;
    void note_frame(Clock::time_point now, long long frame);
    bool enabled_;
    pid_t pid_ = -1;      // ffmpeg
    pid_t gst_pid_ = -1;  // gst-launch(encode)
    std::unique_ptr<Feeder> feeder_;  // 입력 framebus
    int pipe_ = -1;
    std::string buf_;
    long long frame_ = -1;
    long long out_time_ = -1;
    double fps_ = 0;  // 최근 kFpsWindow 동안 보낸 프레임 수로(ffmpeg의 fps=는 시작부터 평균이라 쓰지 않음)
    std::deque<std::pair<Clock::time_point, long long>> samples_;  // (받은 시각, frame)
    bool linked_ = false;
    bool killed_ = false;
    Clock::time_point last_progress_{};
    Clock::time_point retry_at_{};
    unsigned long restarts_ = 0;
    std::string last_error_;
    std::vector<nlohmann::json> events_;
    Clock::time_point down_since_{};  // 끊긴 때(이어 붙을 때 끊긴 시간)
};

// 장치 워커가 가진 송출 전체. poll 루프에 붙이고, 명령(stream_start, stream_stop)을 처리한다
class Streams {
public:
    using Clock = std::chrono::steady_clock;

    void configure(const std::vector<StreamConfig>& configs);
    size_t size() const { return streams_.size(); }

    void add_pollfds(std::vector<pollfd>& fds) const;  // 각 송출의 progress 파이프
    void on_poll(const std::vector<pollfd>& fds, Clock::time_point now);
    void on_timer(Clock::time_point now);
    Clock::time_point next_wakeup(Clock::time_point now) const;
    void stop();

    nlohmann::json status() const;  // [{name, link, enabled, fps, ...}, ...]
    std::string metrics() const;    // heartbeat: "stream_main_link=1 stream_main_fps=30.0 ..."
    // stream_start / stream_stop {"name"}. 처리한 명령이면 true(reply에 응답)
    bool handle(const std::string& cmd, const nlohmann::json& args, nlohmann::json* reply);
    std::vector<nlohmann::json> take_events();  // 모든 송출의 끊김·복구 이벤트

private:
    std::vector<std::unique_ptr<Stream>> streams_;
};

}  // namespace video
