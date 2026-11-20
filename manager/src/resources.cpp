#include "resources.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "log.hpp"

namespace pm {

namespace {

double mono_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool read_small(const char* path, std::string& out) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[1024];
    const ssize_t n = ::read(fd, buf, sizeof buf);
    ::close(fd);
    if (n <= 0) return false;
    out.assign(buf, static_cast<size_t>(n));
    return true;
}

double round1(double x) { return std::round(x * 10.0) / 10.0; }

struct Stat {
    int pgrp = 0;
    long long ticks = 0, rss = 0, threads = 0;
};

// /proc/<pid>/stat 한 줄. comm에 공백·괄호가 있을 수 있어 마지막 ')' 뒤부터 센다(그 뒤 첫 칸이 3번째 칸 state).
// 문자열 스트림을 쓰지 않고 칸을 건너뛰며 숫자만 읽는다(1초마다 여러 번 부르므로)
bool read_stat(int pid, Stat& st) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
    std::string data;
    if (!read_small(path, data)) return false;
    const size_t p = data.rfind(')');
    if (p == std::string::npos || p + 2 >= data.size()) return false;
    const char* s = data.c_str() + p + 2;
    long long f[22] = {};  // f[0] = 3번째 칸
    for (int i = 0; i < 22; ++i) {
        while (*s == ' ') ++s;
        if (!*s) return false;
        char* end = nullptr;
        f[i] = std::strtoll(s, &end, 10);
        if (end == s) {  // 숫자가 아닌 칸(state 글자): 건너뜀
            while (*s && *s != ' ') ++s;
        } else {
            s = end;
        }
    }
    st.pgrp = static_cast<int>(f[2]);  // 5: pgrp
    st.ticks = f[11] + f[12];          // 14 utime, 15 stime
    st.threads = f[17];                // 20 num_threads
    st.rss = f[21];                    // 24 rss
    return true;
}

}  // namespace

std::map<int, Json> ResourceSampler::sample(const std::vector<int>& pgids, double now) {
    static const long clk = ::sysconf(_SC_CLK_TCK);
    static const long page = ::sysconf(_SC_PAGESIZE);
    struct Acc {
        long long ticks = 0, rss = 0, procs = 0, threads = 0;
    };
    std::map<int, Acc> totals;
    for (int g : pgids) totals[g];
    std::map<int, Json> out;
    if (totals.empty()) {
        prev_.clear();
        members_.clear();
        return out;
    }
    // 빠진 그룹은 잊고, 새 그룹이 있으면 전체 훑기
    bool full = now - last_full_ >= kFullScanS;
    for (auto it = members_.begin(); it != members_.end();) it = totals.count(it->first) ? std::next(it) : members_.erase(it);
    for (const auto& kv : totals)
        if (!members_.count(kv.first)) full = true;

    Stat st;
    if (!full) {
        // 기억한 pid만 읽는다. 하나라도 사라졌거나 그룹을 옮겼으면 이번에 전체 훑기로 바꾼다
        for (auto& [g, pids] : members_) {
            Acc& a = totals[g];
            for (int pid : pids) {
                if (!read_stat(pid, st) || st.pgrp != g) {
                    full = true;
                    break;
                }
                a.ticks += st.ticks;
                a.rss += st.rss;
                a.procs += 1;
                a.threads += st.threads;
            }
            if (full) break;
        }
    }
    if (full) {
        for (auto& kv : totals) kv.second = Acc{};
        members_.clear();
        for (const auto& kv : totals) members_[kv.first];
        if (DIR* d = ::opendir("/proc")) {
            while (dirent* e = ::readdir(d)) {
                if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
                const int pid = std::atoi(e->d_name);
                if (!read_stat(pid, st)) continue;
                auto it = totals.find(st.pgrp);
                if (it == totals.end()) continue;
                it->second.ticks += st.ticks;
                it->second.rss += st.rss;
                it->second.procs += 1;
                it->second.threads += st.threads;
                members_[st.pgrp].push_back(pid);
            }
            ::closedir(d);
        }
        last_full_ = now;
        ++full_scans_;
    }
    std::map<int, std::pair<long long, double>> next;
    for (const auto& [g, a] : totals) {
        Json cpu = nullptr;
        auto pv = prev_.find(g);
        if (pv != prev_.end() && now > pv->second.second)
            cpu = round1(static_cast<double>(std::max(a.ticks - pv->second.first, 0LL)) / clk / (now - pv->second.second) * 100.0);
        next[g] = {a.ticks, now};
        Json u = Json::object();
        u["cpu_pct"] = cpu;
        u["rss_mb"] = round1(static_cast<double>(a.rss) * page / (1024.0 * 1024.0));
        u["procs"] = a.procs;
        u["threads"] = a.threads;
        out[g] = u;
    }
    prev_ = std::move(next);
    return out;
}

Resources::Resources(worker::ThreadWatch& watch)
    : watch_(watch), thread_("pm.res", watch, [this](worker::WorkerThread& t) { run(t); }) {
    efd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

Resources::~Resources() {
    stop();
    if (efd_ >= 0) ::close(efd_);
}

void Resources::start() { thread_.start(); }

void Resources::stop() {
    thread_.request_stop();
    thread_.join();
}

void Resources::set_pgids(std::vector<int> pgids) {
    std::lock_guard<std::mutex> lock(mu_);
    pgids_ = std::move(pgids);
}

bool Resources::take(std::map<int, Json>& out) {
    uint64_t v;
    [[maybe_unused]] ssize_t n = ::read(efd_, &v, sizeof v);
    std::lock_guard<std::mutex> lock(mu_);
    if (!fresh_) return false;
    out = std::move(latest_);
    latest_.clear();
    fresh_ = false;
    return true;
}

void Resources::run(worker::WorkerThread& t) {
    ResourceSampler sampler;
    double next = mono_now();
    while (!t.stopping()) {
        t.beat();
        const double now = mono_now();
        if (now < next) {
            // 짧게 나눠 자며 beat(멈춤 감지 3 s보다 훨씬 짧게)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(100, static_cast<int>((next - now) * 1000) + 1)));
            continue;
        }
        next = now + 1.0;
        std::vector<int> pgids;
        {
            std::lock_guard<std::mutex> lock(mu_);
            pgids = pgids_;
        }
        auto got = sampler.sample(pgids, mono_now());
        {
            std::lock_guard<std::mutex> lock(mu_);
            latest_ = std::move(got);
            fresh_ = true;
        }
        const uint64_t one = 1;
        [[maybe_unused]] ssize_t n = ::write(efd_, &one, sizeof one);
        // 루프나 다른 스레드가 멈춤 상한(3 s)을 넘으면 기록하고 끝낸다 → 시작 셸이 워크스페이스를 내린다(6.3)
        std::string who;
        if (!watch_.all_alive(worker::ThreadWatch::Clock::now(), &who)) {
            PM_ERROR("thread stalled: %s; exiting", who.c_str());
            ::_exit(4);
        }
    }
}

}  // namespace pm
