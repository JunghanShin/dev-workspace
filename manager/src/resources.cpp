#include "resources.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
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
        return out;
    }
    if (DIR* d = ::opendir("/proc")) {
        std::string data;
        char path[300];
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            std::snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
            if (!read_small(path, data)) continue;
            // comm에 공백·괄호가 있을 수 있어 마지막 ')' 뒤부터 센다. rest[0]이 3번째 칸(state)
            const size_t p = data.rfind(')');
            if (p == std::string::npos || p + 2 > data.size()) continue;
            std::istringstream in(data.substr(p + 2));
            std::vector<std::string> rest;
            std::string tok;
            while (rest.size() < 22 && in >> tok) rest.push_back(tok);
            if (rest.size() < 22) continue;
            auto it = totals.find(std::atoi(rest[2].c_str()));  // 5: pgrp
            if (it == totals.end()) continue;
            it->second.ticks += std::atoll(rest[11].c_str()) + std::atoll(rest[12].c_str());  // 14 utime, 15 stime
            it->second.rss += std::atoll(rest[21].c_str());                                   // 24 rss
            it->second.procs += 1;
            it->second.threads += std::atoll(rest[17].c_str());                               // 20 num_threads
        }
        ::closedir(d);
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
