#include "video/tap.hpp"

#include <cmath>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "worker/child.hpp"
#include "worker/config.hpp"
#include "worker/workspace.hpp"

namespace video {

using worker::read;
using worker::require;

namespace {

std::chrono::milliseconds ms(double sec) { return std::chrono::milliseconds(static_cast<long>(sec * 1000)); }

long long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 알림 소켓 폴더(없으면 만든다. 워크스페이스에서는 <데이터>/run/frames)
bool make_dirs(const std::string& dir) {
    if (dir.empty()) return true;
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!cur.empty() && mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        if (i < dir.size()) cur += dir[i];
    }
    return true;
}

}  // namespace

TapConfig parse_tap(const nlohmann::json& t, const TapConfig& defaults) {
    TapConfig c = defaults;
    if (t.is_null()) return c;
    require(t.is_object(), "config.tap must be an object");
    worker::check_keys(t, {"enabled", "source", "source_codec", "width", "height", "pixel", "fps", "slots", "socket",
                           "retry_sec", "stall_sec", "gst_launch"});
    read(t, "enabled", c.enabled);
    read(t, "source", c.source);
    read(t, "source_codec", c.source_codec);
    read(t, "width", c.width);
    read(t, "height", c.height);
    read(t, "pixel", c.pixel);
    read(t, "fps", c.fps);
    read(t, "slots", c.slots);
    read(t, "socket", c.socket);
    read(t, "retry_sec", c.retry_sec);
    read(t, "stall_sec", c.stall_sec);
    read(t, "gst_launch", c.gst_launch);
    require(c.source_codec == "h264" || c.source_codec == "h265", "config.tap.source_codec must be h264 or h265");
    require(c.width >= 16 && c.width <= 7680 && c.height >= 16 && c.height <= 4320, "config.tap.width/height out of range");
    require(c.pixel == "BGRx" || c.pixel == "RGBA" || c.pixel == "GRAY8", "config.tap.pixel must be BGRx, RGBA or GRAY8");
    require(c.fps >= 1 && c.fps <= 240, "config.tap.fps must be 1..240");
    require(c.slots >= 2 && c.slots <= 16, "config.tap.slots must be 2..16");
    require(c.retry_sec >= 0.2 && c.stall_sec >= 1, "config.tap.retry_sec >= 0.2, stall_sec >= 1");
    require(!c.enabled || !c.source.empty(), "config.tap.source is empty");
    return c;
}

std::vector<std::string> gst_tap_command(const TapConfig& c) {
    const bool h265 = c.source_codec == "h265";
    // 카메라 → 보드는 같은 내부망이라 TCP, 지터 버퍼 없음(송출과 같음). NVDEC로 풀고 nvvidconv로 크기·형식을 맞춰
    // CPU 메모리 원본으로 내린다. sync=false: 받는 대로 내보냄(시계에 맞춰 기다리지 않음)
    return {c.gst_launch, "-q", "rtspsrc", "location=" + c.source, "protocols=tcp", "latency=0", "!",
            h265 ? "rtph265depay" : "rtph264depay", "!",
            std::string(h265 ? "video/x-h265" : "video/x-h264") + ",stream-format=byte-stream,alignment=au", "!",
            "nvv4l2decoder", "!", "nvvidconv", "!",
            "video/x-raw,format=" + c.pixel + ",width=" + std::to_string(c.width) + ",height=" + std::to_string(c.height),
            "!", "fdsink", "fd=1", "sync=false"};
}

Tap::Tap(TapConfig config) : c_(std::move(config)) {}

Tap::~Tap() { stop(); }

bool Tap::open(std::string* error) {
    if (!c_.enabled) return true;
    const auto slash = c_.socket.rfind('/');
    if (slash != std::string::npos && slash > 0 && !make_dirs(c_.socket.substr(0, slash))) {
        if (error) *error = "cannot create " + c_.socket.substr(0, slash);
        return false;
    }
    framebus::Format f;
    f.width = static_cast<uint32_t>(c_.width);
    f.height = static_cast<uint32_t>(c_.height);
    f.stride = static_cast<uint32_t>(c_.width * c_.bytes_per_pixel());
    f.pixel = c_.pixel;
    bus_ = std::make_unique<framebus::Producer>(c_.socket, f, c_.slots);
    if (!bus_->start(error)) {
        bus_.reset();
        return false;
    }
    std::printf("tap: %s -> frame bus %s (%dx%d %s)\n", c_.source.c_str(), c_.socket.c_str(), c_.width, c_.height,
                c_.pixel.c_str());
    std::fflush(stdout);
    return true;
}

// 받기 스레드: 파이프에서 한 프레임(frame_bytes)씩 슬롯에 바로 읽어 넣고 알린다. 빈 슬롯이 없으면 버릴 곳에 읽는다
void Tap::read_loop(int fd) {
    const size_t bytes = bus_->format().frame_bytes();
    std::vector<uint8_t> scratch(bytes);
    uint64_t id = 0;
    while (!stop_reader_.load()) {
        auto slot = bus_->acquire();
        uint8_t* dst = slot ? slot->data : scratch.data();
        size_t got = 0;
        bool ok = true;
        while (got < bytes && !stop_reader_.load()) {
            pollfd pfd{fd, POLLIN, 0};
            const int r = poll(&pfd, 1, 200);
            if (r == 0) continue;
            if (r < 0 && errno == EINTR) continue;
            const ssize_t n = ::read(fd, dst + got, bytes - got);
            if (n > 0) {
                got += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            ok = false;  // 자식 끝(EOF) 또는 오류
            break;
        }
        if (!ok || got < bytes) {
            if (slot) bus_->abandon(*slot);
            break;
        }
        const long long ts = now_ns();
        if (slot) bus_->publish(*slot, ++id, ts);
        frames_.fetch_add(1);
        last_frame_ns_.store(ts);
    }
    ::close(fd);
    reader_done_.store(true);
}

void Tap::start(Clock::time_point now) {
    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) return failed(now, std::string("pipe: ") + std::strerror(errno));
    pid_ = worker::spawn(gst_tap_command(c_), -1, out[1]);
    ::close(out[1]);
    if (pid_ < 0) {
        ::close(out[0]);
        return failed(now, std::string("fork: ") + std::strerror(errno));
    }
    stop_reader_.store(false);
    reader_done_.store(false);
    last_frame_ns_.store(0);
    started_ = now;
    reader_ = std::thread([this, fd = out[0]] { read_loop(fd); });
}

void Tap::end_child() {
    stop_reader_.store(true);
    if (pid_ > 0) kill(pid_, SIGTERM);
    if (reader_.joinable()) reader_.join();
    worker::kill_wait(pid_);
}

std::vector<nlohmann::json> Tap::take_events() {
    std::vector<nlohmann::json> out;
    out.swap(events_);
    return out;
}

void Tap::failed(Clock::time_point now, const std::string& why) {
    linked_ = false;
    fps_ = 0;
    retry_at_ = now + ms(c_.retry_sec);
    ++restarts_;
    std::lock_guard<std::mutex> lock(mu_);
    if (why != last_error_) {
        std::fprintf(stderr, "tap: down (%s); retrying every %.1f s\n", why.c_str(), c_.retry_sec);
        if (last_error_.empty()) down_since_ = now;
        events_.push_back({{"severity", "warning"}, {"code", "tap_down"}, {"text", "tap down: " + why},
                           {"detail", {{"reason", why}, {"restarts", restarts_}}}});
        if (events_.size() > 32) events_.erase(events_.begin());
        last_error_ = why;
    }
}

void Tap::on_timer(Clock::time_point now) {
    if (!bus_) return;
    if (pid_ > 0) {
        int status = 0;
        std::string why;
        if (waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;
            why = worker::exit_text("gst-launch", status);
        } else if (reader_done_.load()) {
            why = "gst-launch output ended";
        } else {
            // 첫 프레임 전에는 RTSP 연결·풀기 준비가 있어 세 배까지 기다린다
            const long long last = last_frame_ns_.load();
            const auto since = last ? std::chrono::nanoseconds(now_ns() - last) : now - started_;
            const auto limit = last ? ms(c_.stall_sec) : ms(c_.stall_sec) * 3;
            if (since >= limit) why = "no frame for " + std::to_string(static_cast<int>(c_.stall_sec)) + " s";
        }
        if (!why.empty()) {
            end_child();
            return failed(now, why);
        }
        if (last_frame_ns_.load() && !linked_) {
            linked_ = true;
            std::lock_guard<std::mutex> lock(mu_);
            if (!last_error_.empty()) {
                std::printf("tap: up again\n");
                const double down_s = std::chrono::duration<double>(now - down_since_).count();
                events_.push_back({{"severity", "notice"}, {"code", "tap_up"},
                                   {"text", "tap up again after " + std::to_string(static_cast<int>(down_s + 0.5)) + " s"},
                                   {"detail", {{"down_s", std::round(down_s * 10) / 10}, {"restarts", restarts_}}}});
            }
            last_error_.clear();
        }
        if (now - fps_at_ >= std::chrono::seconds(1)) {
            const unsigned long long f = frames_.load();
            const double dt = std::chrono::duration<double>(now - fps_at_).count();
            if (fps_at_ != Clock::time_point{}) fps_ = static_cast<double>(f - fps_frames_) / dt;
            fps_frames_ = f;
            fps_at_ = now;
        }
        return;
    }
    if (now >= retry_at_) start(now);
}

Tap::Clock::time_point Tap::next_wakeup(Clock::time_point now) const {
    if (!bus_) return now + std::chrono::seconds(1);
    if (pid_ > 0) return now + std::chrono::milliseconds(200);
    return retry_at_ > now ? retry_at_ : now;
}

void Tap::stop() {
    end_child();
    if (bus_) bus_->stop();
    bus_.reset();
    linked_ = false;
}

framebus::Stats Tap::bus_stats() const { return bus_ ? bus_->stats() : framebus::Stats{}; }

nlohmann::json Tap::status() const {
    nlohmann::json s = {{"enabled", c_.enabled}, {"link", linked_ ? "up" : "down"}, {"source", c_.source},
                        {"socket", c_.socket},   {"width", c_.width},              {"height", c_.height},
                        {"pixel", c_.pixel},     {"fps", fps_},                    {"frames", frames_.load()},
                        {"restarts", restarts_}};
    const auto st = bus_stats();
    s["bus"] = {{"published", st.published}, {"dropped", st.dropped}, {"consumers", st.consumers}};
    std::lock_guard<std::mutex> lock(mu_);
    if (!last_error_.empty()) s["last_error"] = last_error_;
    return s;
}

void TapModule::configure(const worker::Json& config) {
    const TapConfig defaults = defaults_ ? defaults_() : TapConfig{};
    try {
        cfg_ = parse_tap(config, defaults);
    } catch (const worker::ConfigError& e) {
        throw worker::ConfigError(std::string("config.tap: ") + e.what());
    }
}

void TapModule::start(worker::Host& host) {
    if (cfg_.socket.empty()) cfg_.socket = worker::default_socket(host.id(), "frames");
    tap_ = std::make_unique<Tap>(cfg_);
    std::string error;
    if (!tap_->open(&error)) throw std::runtime_error("tap frame bus: " + error);
}

worker::Json TapModule::status() const { return tap_ ? tap_->status() : worker::Json{{"enabled", cfg_.enabled}}; }

void TapModule::metrics(worker::Metrics& m) const {
    if (!tap_ || !cfg_.enabled) return;
    const auto st = tap_->bus_stats();
    m.set("link", tap_->linked());
    m.set("fps", tap_->fps(), 1);
    m.set("frames", static_cast<uint64_t>(tap_->frames()));
    m.set("dropped", st.dropped);
    m.set("consumers", st.consumers);
    m.set("restarts", static_cast<uint64_t>(tap_->restarts()));
}

}  // namespace video
