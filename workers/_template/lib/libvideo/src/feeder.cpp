#include "feeder.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace video {

using Clock = std::chrono::steady_clock;

Feeder::Feeder(std::unique_ptr<framebus::Consumer> consumer, int out_fd, int fps)
    : consumer_(std::move(consumer)), format_(consumer_->format()), out_(out_fd), fps_(fps < 1 ? 1 : fps) {
    fcntl(out_, F_SETFL, fcntl(out_, F_GETFL) | O_NONBLOCK);
    thread_ = std::thread([this] { run(); });
}

Feeder::~Feeder() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (out_ >= 0) close(out_);
    if (consumer_) consumer_->close();
}

std::string Feeder::error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

bool Feeder::write_all(const uint8_t* p, size_t n) {
    while (n > 0 && !stop_.load()) {
        const ssize_t w = write(out_, p, n);
        if (w > 0) {
            p += w;
            n -= static_cast<size_t>(w);
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            std::lock_guard<std::mutex> lock(mu_);
            error_ = std::string("pipe to gst-launch: ") + std::strerror(errno);
            return false;
        }
        pollfd pfd{out_, POLLOUT, 0};
        if (poll(&pfd, 1, 1000) == 0) {
            std::lock_guard<std::mutex> lock(mu_);
            error_ = "gst-launch does not read its input (1 s)";
            return false;
        }
    }
    return n == 0;
}

void Feeder::run() {
    const auto interval = std::chrono::microseconds(1000000 / fps_);
    auto due = Clock::now();
    while (!stop_.load()) {
        // 다음 보낼 시각까지 기다린 뒤 그때의 최신 프레임을 받는다
        const auto now = Clock::now();
        if (now < due) {
            std::this_thread::sleep_for(std::min<Clock::duration>(due - now, std::chrono::milliseconds(20)));
            continue;
        }
        framebus::Frame f;
        const auto r = consumer_->next(100, &f);
        if (r == framebus::Consumer::Result::Timeout) continue;
        if (r == framebus::Consumer::Result::Disconnected) {
            std::lock_guard<std::mutex> lock(mu_);
            error_ = "frame bus disconnected";
            break;
        }
        const bool ok = write_all(f.data, f.bytes);
        consumer_->release(f);
        if (!ok) break;
        fed_.fetch_add(1);
        due += interval;
        if (Clock::now() - due > interval) due = Clock::now() + interval;  // 밀렸으면 다시 맞춘다
    }
    // 파이프를 닫아 gst가 입력 끝(EOF)을 보게 한다
    if (out_ >= 0) close(out_);
    out_ = -1;
    ended_.store(true);
}

}  // namespace video
