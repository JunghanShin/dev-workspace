#include "worker/runtime.hpp"

#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace worker {

namespace {

volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_reload = 0;

void on_term(int) { g_stop = 1; }
void on_hup(int) { g_reload = 1; }

}  // namespace

void arm_watchdog(double seconds) {
    itimerval t{};
    t.it_value.tv_sec = static_cast<time_t>(seconds);
    t.it_value.tv_usec = static_cast<suseconds_t>((seconds - static_cast<double>(t.it_value.tv_sec)) * 1e6);
    setitimer(ITIMER_REAL, &t, nullptr);
}

void install_signals() {
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, on_hup);  // 설정 다시 읽기(바로 적용, design.md 6.3 live_keys)
    signal(SIGPIPE, SIG_IGN);
}

bool stop_requested() { return g_stop != 0; }

bool take_reload_request() {
    if (!g_reload) return false;
    g_reload = 0;
    return true;
}

}  // namespace worker
