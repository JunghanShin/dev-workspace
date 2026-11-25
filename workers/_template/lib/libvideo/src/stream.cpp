#include "video/stream.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "feeder.hpp"
#include "video/pipeline.hpp"
#include "worker/child.hpp"

namespace video {

Stream::Stream(StreamConfig config) : c_(std::move(config)), enabled_(c_.autostart) {}

Stream::~Stream() { stop(); }

void Stream::set_enabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (!on) {
        stop();
        last_error_.clear();
        fps_ = 0;
    } else {
        retry_at_ = {};  // 다음 on_timer에서 바로 시작
    }
}

namespace {

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::chrono::milliseconds ms(double sec) { return std::chrono::milliseconds(static_cast<long>(sec * 1000)); }

}  // namespace

void Stream::start(Clock::time_point now) {
    // progress는 ffmpeg fd 3(stdin은 encode 때 영상 입력, stdout은 쓰지 않음)
    int prog[2];
    if (pipe2(prog, O_CLOEXEC) != 0) return failed(now, std::string("pipe: ") + std::strerror(errno));
    int media[2] = {-1, -1};
    if (c_.encode() && pipe2(media, O_CLOEXEC) != 0) {
        close(prog[0]);
        close(prog[1]);
        return failed(now, std::string("pipe: ") + std::strerror(errno));
    }
    if (c_.input == "framebus") {
        // 프레임 버스에 붙어 형식을 받은 뒤 그 형식으로 gst를 띄운다. 생산자(장치 워커)가 없으면 retry_sec 뒤 다시
        auto consumer = std::make_unique<framebus::Consumer>(c_.source);
        std::string e;
        int in[2] = {-1, -1};
        if (!consumer->connect(&e) || pipe2(in, O_CLOEXEC) != 0) {
            for (int fd : {prog[0], prog[1], media[0], media[1], in[0], in[1]}) {
                if (fd >= 0) close(fd);
            }
            return failed(now, e.empty() ? std::string("pipe: ") + std::strerror(errno) : e);
        }
        const framebus::Format fmt = consumer->format();
        gst_pid_ = worker::spawn(gst_command(c_, &fmt), in[0], media[1]);
        close(in[0]);
        feeder_ = std::make_unique<Feeder>(std::move(consumer), in[1], c_.fps);
    } else if (c_.encode()) {
        gst_pid_ = worker::spawn(gst_command(c_), -1, media[1]);
    }
    const pid_t pid = worker::spawn(ffmpeg_command(c_), media[0], -1, prog[1]);
    close(prog[1]);
    if (media[0] >= 0) close(media[0]);
    if (media[1] >= 0) close(media[1]);
    if (pid < 0 || (c_.encode() && gst_pid_ < 0)) {
        close(prog[0]);
        pid_t p = pid;
        worker::kill_wait(p);
        feeder_.reset();  // Feeder 스레드를 멈추고 파이프를 닫는다
        worker::kill_wait(gst_pid_);
        return failed(now, std::string("fork: ") + std::strerror(errno));
    }
    pipe_ = prog[0];
    fcntl(pipe_, F_SETFL, fcntl(pipe_, F_GETFL) | O_NONBLOCK);
    pid_ = pid;
    killed_ = false;
    buf_.clear();
    frame_ = out_time_ = -1;
    fps_ = 0;
    samples_.clear();
    last_progress_ = now;
    if (last_error_.empty()) {
        if (c_.encode()) {
            std::printf("stream %s: %s -> %s [%s] (encode %d kbps, keyframe %.1f s, intra refresh %.1f s%s)\n",
                        c_.name.c_str(), c_.source.c_str(), target_url(c_).c_str(), c_.transport.c_str(), c_.bitrate_kbps,
                        c_.keyframe_sec, c_.intra_refresh_sec, c_.overlay_time ? ", time overlay" : "");
        } else {
            std::printf("stream %s: %s -> %s [%s] (relay)\n", c_.name.c_str(), c_.source.c_str(),
                        target_url(c_).c_str(), c_.transport.c_str());
        }
    }
}

void Stream::on_readable(Clock::time_point now) {
    if (pipe_ < 0) return;
    char chunk[4096];
    for (;;) {
        const ssize_t n = read(pipe_, chunk, sizeof(chunk));
        if (n > 0) {
            buf_.append(chunk, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) close_pipe();  // ffmpeg 종료. reap은 on_timer에서
        break;                     // EAGAIN 등
    }
    // -progress는 key=value 줄을 묶음으로 낸다. 보낸 프레임 수(frame)나 송출 시각(out_time_us)이 늘면
    // 송출 중. 보낸 바이트(total_size)는 RTSP 출력에서 N/A라 쓰지 않는다
    size_t pos;
    while ((pos = buf_.find('\n')) != std::string::npos) {
        const std::string line = buf_.substr(0, pos);
        buf_.erase(0, pos + 1);
        // fps=는 ffmpeg가 시작한 뒤부터의 평균(시작할 때 기다린 몇 초가 섞여 다시 시작한 뒤 한참 낮게 나옴).
        // 대신 frame= 변화로 최근 몇 초의 실제 값을 센다(아래 note_frame)
        if (starts_with(line, "fps=")) continue;
        long long* last = nullptr;
        size_t skip = 0;
        if (starts_with(line, "frame=")) {
            last = &frame_;
            skip = 6;
        } else if (starts_with(line, "out_time_us=")) {
            last = &out_time_;
            skip = 12;
        }
        if (!last || line.size() <= skip || line[skip] < '0' || line[skip] > '9') continue;  // N/A
        const long long value = std::atoll(line.c_str() + skip);
        if (last == &frame_) note_frame(now, value);
        if (value > *last) {
            *last = value;
            last_progress_ = now;
            if (!linked_) {
                linked_ = true;
                if (!last_error_.empty()) std::printf("stream %s: up again\n", c_.name.c_str());
                last_error_.clear();
            }
        }
    }
    if (buf_.size() > 65536) buf_.clear();
}

// 송출 fps = 최근 창(kFpsWindow) 안의 프레임 수 변화 / 걸린 시간. 0.5 s보다 짧으면 아직 모름(0)
void Stream::note_frame(Clock::time_point now, long long frame) {
    if (!samples_.empty() && frame < samples_.back().second) samples_.clear();  // ffmpeg를 다시 띄움
    samples_.emplace_back(now, frame);
    while (samples_.size() > 2 && now - samples_[1].first >= kFpsWindow) samples_.pop_front();
    const double dt = std::chrono::duration<double>(samples_.back().first - samples_.front().first).count();
    fps_ = dt >= 0.5 ? static_cast<double>(samples_.back().second - samples_.front().second) / dt : 0;
}

// 묶음 중 하나라도 끝나면 다른 쪽도 정리하고 다시 시작한다
void Stream::reap(Clock::time_point now) {
    std::string why;
    int status = 0;
    if (gst_pid_ > 0 && waitpid(gst_pid_, &status, WNOHANG) == gst_pid_) {
        gst_pid_ = -1;
        why = worker::exit_text("gst-launch", status);
    }
    if (pid_ > 0 && waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        if (why.empty()) why = worker::exit_text("ffmpeg", status);
    }
    if (why.empty()) return;
    if (killed_) why = "no progress for " + std::to_string(static_cast<int>(c_.stall_sec)) + " s";
    feeder_.reset();  // Feeder 스레드를 멈추고 파이프를 닫는다
    worker::kill_wait(gst_pid_);
    worker::kill_wait(pid_);
    close_pipe();
    failed(now, why);
}

void Stream::failed(Clock::time_point now, const std::string& why) {
    linked_ = false;
    fps_ = 0;
    samples_.clear();
    retry_at_ = now + ms(c_.retry_sec);
    ++restarts_;
    // 같은 이유가 반복되면(카메라를 뺀 채로 둠) 로그는 한 번만
    if (why != last_error_) {
        std::fprintf(stderr, "stream %s: down (%s); retrying every %.1f s\n", c_.name.c_str(), why.c_str(),
                     c_.retry_sec);
        last_error_ = why;
    }
}

void Stream::on_timer(Clock::time_point now) {
    if (pid_ > 0 || gst_pid_ > 0) {
        reap(now);
        // 첫 프레임 전에는 입력을 분석한다(ffmpeg 기본 최대 5초). 그동안은 3배까지 기다린다
        const auto stall = ms(c_.stall_sec);
        const auto limit = (frame_ < 0 && out_time_ < 0) ? stall * 3 : stall;
        if (now - last_progress_ >= std::chrono::seconds(2)) fps_ = 0;  // 프레임이 안 늘면 0(멈춤 판정 전에도)
        if (pid_ > 0 && !killed_ && now - last_progress_ >= limit) {
            // 멈춤(카메라가 영상을 안 보내는데 연결은 살아 있음, 망이 막힘 등). 기다리지 않고 끝낸다
            kill(pid_, SIGKILL);
            if (gst_pid_ > 0) kill(gst_pid_, SIGKILL);
            killed_ = true;
            linked_ = false;
        }
        return;
    }
    if (enabled_ && now >= retry_at_) start(now);
}

Stream::Clock::time_point Stream::next_wakeup(Clock::time_point now) const {
    if (pid_ > 0 || gst_pid_ > 0) return now + std::chrono::milliseconds(200);  // reap·멈춤 확인
    if (!enabled_) return now + std::chrono::seconds(1);
    return retry_at_ > now ? retry_at_ : now;
}

void Stream::stop() {
    feeder_.reset();  // Feeder 스레드를 멈추고 파이프를 닫는다
    worker::kill_wait(gst_pid_);
    worker::kill_wait(pid_);
    close_pipe();
    linked_ = false;
}

void Stream::close_pipe() {
    if (pipe_ >= 0) close(pipe_);
    pipe_ = -1;
}

nlohmann::json Stream::status() const {
    nlohmann::json s = {{"name", c_.name}, {"enabled", enabled_}, {"link", linked_ ? "up" : "down"}, {"input", c_.input},
                        {"source", c_.source},   {"mode", c_.mode},                 {"url", c_.url},
                        {"transport", c_.transport}, {"restarts", restarts_},       {"fps", fps_}};
    if (target_url(c_) != c_.url) s["target"] = target_url(c_);
    if (c_.transport == "srt") s["srt_latency_ms"] = c_.srt_latency_ms;
    if (c_.encode()) {
        s["bitrate_kbps"] = c_.bitrate_kbps;
        s["overlay_time"] = c_.overlay_time;
        s["keyframe_sec"] = c_.keyframe_sec;
        s["intra_refresh_sec"] = c_.intra_refresh_sec;
    }
    if (feeder_) {
        const auto& f = feeder_->format();
        s["framebus"] = {{"width", f.width}, {"height", f.height}, {"pixel", f.pixel}, {"fed", feeder_->fed()},
                         {"skipped", feeder_->skipped()}};
        if (!feeder_->error().empty()) s["framebus"]["error"] = feeder_->error();
    }
    if (!last_error_.empty()) s["last_error"] = last_error_;
    return s;
}

}  // namespace video
