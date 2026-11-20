#include "log.hpp"

#include <sys/time.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace pm {

namespace {
std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::mutex g_mu;  // 여러 스레드가 한 줄씩 섞이지 않게
}  // namespace

void set_log_level(const std::string& name) {
    Level l = Level::Info;
    if (name == "DEBUG") l = Level::Debug;
    else if (name == "WARNING" || name == "WARN") l = Level::Warning;
    else if (name == "ERROR") l = Level::Error;
    g_level.store(static_cast<int>(l));
}

void log(Level level, const char* fmt, ...) {
    if (static_cast<int>(level) < g_level.load()) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    timeval tv{};
    gettimeofday(&tv, nullptr);
    tm t{};
    localtime_r(&tv.tv_sec, &t);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &t);
    const char* name = level == Level::Debug ? "DEBUG" : level == Level::Info ? "INFO"
                     : level == Level::Warning ? "WARNING" : "ERROR";
    std::lock_guard<std::mutex> lock(g_mu);
    std::fprintf(stderr, "%s,%03d %s workspace.manager: %s\n", ts, static_cast<int>(tv.tv_usec / 1000), name, msg);
    std::fflush(stderr);
}

}  // namespace pm
