#include "sysmon/extras.hpp"

#include <dirent.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace sysmon {

namespace {

bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// 글자 전체가 정수(앞뒤 공백·부호 허용, Python int()처럼)일 때만
bool parse_int(const std::string& s, long long& out, int base = 10) {
    const char* b = s.c_str();
    while (std::isspace(static_cast<unsigned char>(*b))) ++b;
    if (!*b) return false;
    char* e = nullptr;
    errno = 0;
    const long long v = std::strtoll(b, &e, base);
    if (e == b || errno == ERANGE) return false;
    while (std::isspace(static_cast<unsigned char>(*e))) ++e;
    if (*e) return false;
    out = v;
    return true;
}

bool exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

}  // namespace

std::optional<std::string> read_text(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

SystemExtras::SystemExtras(std::string root, std::string disk_path)
    : root_(std::move(root)), disk_path_(std::move(disk_path)) {
    const std::string dir = root_ + "/sys/class/hwmon";
    std::vector<std::string> hws;
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.compare(0, 5, "hwmon") == 0) hws.push_back(dir + "/" + n);
        }
        closedir(d);
    }
    std::sort(hws.begin(), hws.end());
    for (const auto& hw : hws) {
        const auto name = read_text(hw + "/name");
        if (!name) continue;
        if (*name == "pwmfan" && exists(hw + "/pwm1")) fan_pwm_ = hw + "/pwm1";
        else if (*name == "pwm_tach" && exists(hw + "/rpm")) fan_rpm_ = hw + "/rpm";
    }
}

std::string default_iface(const std::string& root) {
    std::ifstream in(root + "/proc/net/route");
    if (!in) return "";
    std::string line, best;
    long long best_metric = 0;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {  // 머리줄
            first = false;
            continue;
        }
        std::istringstream ls(line);
        std::vector<std::string> r;
        for (std::string w; ls >> w;) r.push_back(w);
        long long flags = 0, metric = 0;
        if (r.size() > 6 && r[1] == "00000000" && parse_int(r[3], flags, 16) && (flags & 1) &&  // 기본 경로, UP
            parse_int(r[6], metric)) {
            if (best.empty() || metric < best_metric) {
                best = r[0];
                best_metric = metric;
            }
        }
    }
    return best;
}

MetricList SystemExtras::sample(double now_s) {
    MetricList m;
    struct statvfs st {};
    if (statvfs(disk_path_.c_str(), &st) == 0) {
        const double frsize = static_cast<double>(st.f_frsize);
        m.set_round1("disk_total_gb", static_cast<double>(st.f_blocks) * frsize / 1e9);
        m.set_round1("disk_used_gb", static_cast<double>(st.f_blocks - st.f_bfree) * frsize / 1e9);
    }
    const std::string iface = default_iface(root_);
    if (!iface.empty()) {
        const auto rx = read_text(root_ + "/sys/class/net/" + iface + "/statistics/rx_bytes");
        const auto tx = read_text(root_ + "/sys/class/net/" + iface + "/statistics/tx_bytes");
        long long rxv = 0, txv = 0;
        if (rx && tx && !rx->empty() && !tx->empty() && parse_int(*rx, rxv) && parse_int(*tx, txv)) {
            if (have_prev_ && prev_iface_ == iface && now_s > prev_t_) {
                const double dt = now_s - prev_t_;
                m.set_round1("net_rx_kbps", static_cast<double>((rxv - prev_rx_) * 8) / 1000.0 / dt);
                m.set_round1("net_tx_kbps", static_cast<double>((txv - prev_tx_) * 8) / 1000.0 / dt);
            }
            have_prev_ = true;
            prev_iface_ = iface;
            prev_t_ = now_s;
            prev_rx_ = rxv;
            prev_tx_ = txv;
        }
    }
    if (!fan_pwm_.empty()) {
        const auto v = read_text(fan_pwm_);
        long long pwm = 0;
        if (v && all_digits(*v) && parse_int(*v, pwm))
            m.set_int("fan_pct", static_cast<long long>(std::nearbyint(static_cast<double>(pwm * 100) / 255.0)));
    }
    if (!fan_rpm_.empty()) {
        const auto v = read_text(fan_rpm_);
        long long rpm = 0;
        if (v && all_digits(!v->empty() && (*v)[0] == '-' ? v->substr(1) : *v) && parse_int(*v, rpm)) m.set_int("fan_rpm", rpm);
    }
    if (const auto up = read_text(root_ + "/proc/uptime")) {
        std::istringstream us(*up);
        std::string first;
        if (us >> first) {
            char* e = nullptr;
            const double s = std::strtod(first.c_str(), &e);
            if (e != first.c_str() && *e == '\0') m.set_int("uptime_s", static_cast<long long>(s));
        }
    }
    return m;
}

std::optional<int> read_power_mode(const std::string& root) {
    const auto s = read_text(root + "/var/lib/nvpmodel/status");
    if (!s) return std::nullopt;
    const auto colon = s->rfind(':');
    long long v = 0;
    if (!parse_int(colon == std::string::npos ? *s : s->substr(colon + 1), v)) return std::nullopt;
    return static_cast<int>(v);
}

}  // namespace sysmon
