#include "recorder.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <vector>

#include "log.hpp"
#include "sys.hpp"

namespace gw {

namespace {
constexpr double kSyncS = 5.0;  // 전원 차단 대비 fsync 주기(logger와 같음)

void mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}
}  // namespace

std::string boot_id8() {
    auto t = read_file("/proc/sys/kernel/random/boot_id", 256);
    if (!t) return "noboot00";
    std::string s;
    for (char c : strip(*t))
        if (c != '-') s += c;
    return s.size() >= 8 ? s.substr(0, 8) : "noboot00";
}

std::string iso_now() {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    tm t{};
    localtime_r(&tv.tv_sec, &t);
    char a[32], z[8];
    std::strftime(a, sizeof a, "%Y-%m-%dT%H:%M:%S", &t);
    std::strftime(z, sizeof z, "%z", &t);  // +0900 → +09:00
    std::string zone = z;
    if (zone.size() == 5) zone.insert(3, ":");
    char ms[8];
    std::snprintf(ms, sizeof ms, ".%03d", static_cast<int>(tv.tv_usec / 1000));
    return std::string(a) + ms + zone;
}

FileRecorder::FileRecorder(std::string dir, std::string boot8, RecorderLimits lim)
    : dir_(std::move(dir)), boot8_(std::move(boot8)), lim_(lim) {}

FileRecorder::~FileRecorder() { stop(2.0); }

void FileRecorder::start() {
    if (t_.joinable()) return;
    t_ = std::thread([this] { run(); });
}

void FileRecorder::stop(double timeout_s) {
    if (!t_.joinable()) return;
    std::unique_lock<std::mutex> lock(mu_);
    stop_ = true;
    cv_.notify_all();
    const bool done = done_cv_.wait_for(lock, std::chrono::duration<double>(timeout_s), [&] { return finished_; });
    lock.unlock();
    if (done) t_.join();
    else t_.detach();  // 디스크가 멈춰 끝나지 않으면 기다리지 않는다(프로세스가 끝나며 정리)
}

void FileRecorder::record(RecordFields f) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (q_.size() >= lim_.queue || stop_) {
            dropped_.fetch_add(1);
            return;
        }
        q_.push_back(std::move(f));
    }
    cv_.notify_one();
}

Json FileRecorder::stats() {
    size_t queued;
    {
        std::lock_guard<std::mutex> lock(mu_);
        queued = q_.size();
    }
    Json o = Json::object();
    o["file"] = current_path();
    o["written"] = written_.load();
    o["dropped"] = dropped_.load();
    o["errors"] = errors_.load();
    o["queued"] = queued;
    return o;
}

void FileRecorder::run() {
    pthread_setname_np(pthread_self(), "gw.log");
    while (true) {
        std::deque<RecordFields> batch;
        bool stopping;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::seconds(1), [&] { return stop_ || !q_.empty(); });
            batch.swap(q_);
            stopping = stop_;
        }
        for (const auto& f : batch) write_line(record_line(f));  // 잠금 밖
        if (dirty_ && fd_ >= 0 && (stopping || mono_now() - last_sync_ >= kSyncS)) {
            ::fdatasync(fd_);
            last_sync_ = mono_now();
            dirty_ = false;
        }
        if (stopping) {
            std::lock_guard<std::mutex> lock(mu_);
            if (q_.empty()) break;
        }
    }
    close_file();
    std::lock_guard<std::mutex> lock(mu_);
    finished_ = true;
    done_cv_.notify_all();
}

bool FileRecorder::ensure_open() {
    if (fd_ >= 0) return true;
    mkdirs(dir_);  // 데이터 폴더가 지워졌어도 다시 만든다
    const std::string path = current_path();
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd_ < 0) {
        const double now = mono_now();
        if (now - last_err_log_ > 60) {
            GW_ERROR("command log %s: %s", path.c_str(), errno_text(errno).c_str());
            last_err_log_ = now;
        }
        return false;
    }
    struct stat st{};
    size_ = ::fstat(fd_, &st) == 0 ? st.st_size : 0;  // 같은 부팅에서 다시 뜨면 이어 쓴다
    prune();
    return true;
}

void FileRecorder::write_line(const std::string& line) {
    if (!ensure_open()) {
        errors_.fetch_add(1);
        return;
    }
    size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = ::write(fd_, line.data() + off, line.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            errors_.fetch_add(1);
            const double now = mono_now();
            if (now - last_err_log_ > 60) {
                GW_ERROR("command log write: %s", errno_text(errno).c_str());
                last_err_log_ = now;
            }
            close_file();  // 다음 줄에서 다시 연다(디스크가 비면 이어짐)
            return;
        }
        off += static_cast<size_t>(n);
    }
    size_ += static_cast<long long>(line.size());
    dirty_ = true;
    written_.fetch_add(1);
    if (size_ >= lim_.file_bytes) rotate();
}

void FileRecorder::close_file() {
    if (fd_ < 0) return;
    if (dirty_) ::fdatasync(fd_);
    ::close(fd_);
    fd_ = -1;
    dirty_ = false;
}

void FileRecorder::rotate() {
    close_file();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string to = dir_ + "/gateway-" + boot8_ + "-" + std::to_string(ms) + ".jsonl";
    if (::rename(current_path().c_str(), to.c_str()) != 0) GW_WARN("command log rotate: %s", errno_text(errno).c_str());
    ensure_open();  // 새 파일 + 정리
}

void FileRecorder::prune() {
    struct F {
        std::string path;
        double mtime;
        long long size;
    };
    std::vector<F> files;
    long long total = 0;
    for (const auto& n : list_dir(dir_)) {
        if (!starts_with(n, "gateway-") || !ends_with(n, ".jsonl")) continue;
        const std::string p = dir_ + "/" + n;
        struct stat st{};
        if (::stat(p.c_str(), &st) != 0) continue;
        files.push_back({p, st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9, static_cast<long long>(st.st_size)});
        total += st.st_size;
    }
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.mtime < b.mtime; });
    const std::string cur = current_path();
    for (const auto& f : files) {
        if (total <= lim_.total_bytes) break;
        if (f.path == cur) continue;
        if (::unlink(f.path.c_str()) == 0) {
            total -= f.size;
            GW_INFO("removed old command log %s", f.path.c_str());
        }
    }
}

}  // namespace gw
