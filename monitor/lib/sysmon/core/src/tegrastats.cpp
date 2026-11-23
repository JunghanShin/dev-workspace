#include "sysmon/tegrastats.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <utility>
#include <vector>

namespace sysmon {

namespace {

// 옛 Python 판(sysmon.py)의 정규식 그대로. ECMAScript 문법이 같은 뜻으로 받는다(입력은 ASCII)
struct Patterns {
    std::regex ram{R"(\bRAM (\d+)/(\d+)MB)"};
    std::regex swap{R"(\bSWAP (\d+)/(\d+)MB)"};
    std::regex cpu{R"(\bCPU \[([^\]]*)\])"};
    std::regex core{R"((\d+)%@(\d+))"};
    std::regex emc{R"(\bEMC_FREQ (\d+)%)"};
    std::regex gpu{R"(\bGR3D_FREQ (\d+)%)"};
    std::regex gpu_mhz{R"(\bGR3D_FREQ \d+%@\[?(\d+))"};
    // NVENC 1036 / NVENC off / VIC_FREQ 0%@115 / NVDLA0_FREQ @1600 등 버전별 표기를 모두 받는다
    std::regex engine{R"(\b(NVENC|NVDEC|NVJPG|VIC|NVDLA0|NVDLA1|OFA)(?:_FREQ)? (off|\d+%@\[?(\d+)|@\[?(\d+)|(\d+)))"};
    std::regex temp{R"(\b([A-Za-z][\w]*)@(-?\d+(?:\.\d+)?)C\b)"};
    std::regex power{R"(\b((?:VDD|VIN)_\w+) (\d+)mW/\d+mW)"};
};

const Patterns& patterns() {
    static const Patterns p;
    return p;
}

long long to_int(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }

std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

}  // namespace

std::string metric_key(const std::string& name) {
    std::string out;
    bool gap = false;
    for (char ch : name) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (gap && !out.empty()) out += '_';  // 앞의 "_"는 넣지 않고, 뒤의 "_"는 마지막 gap을 버려서 뺀다
            gap = false;
            out += c;
        } else {
            gap = true;
        }
    }
    return out;
}

MetricList parse_tegrastats(const std::string& line) {
    const Patterns& p = patterns();
    MetricList m;
    std::smatch r;
    if (std::regex_search(line, r, p.ram)) {
        m.set_int("ram_used_mb", to_int(r[1]));
        m.set_int("ram_total_mb", to_int(r[2]));
    }
    if (std::regex_search(line, r, p.swap)) {
        m.set_int("swap_used_mb", to_int(r[1]));
        m.set_int("swap_total_mb", to_int(r[2]));
    }
    if (std::regex_search(line, r, p.cpu)) {
        const std::string inner = r[1];
        std::vector<std::pair<long long, long long>> cores;
        for (std::sregex_iterator it(inner.begin(), inner.end(), p.core), end; it != end; ++it)
            cores.emplace_back(to_int((*it)[1]), to_int((*it)[2]));
        // 코어별: "11%@1497" 또는 "off"(꺼진 코어는 빠진다)
        size_t i = 0, start = 0;
        while (true) {
            const size_t comma = inner.find(',', start);
            const std::string entry = strip(inner.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            std::smatch c;
            if (std::regex_match(entry, c, p.core)) {
                m.set_int("cpu" + std::to_string(i) + "_pct", to_int(c[1]));
                m.set_int("cpu" + std::to_string(i) + "_mhz", to_int(c[2]));
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
            ++i;
        }
        if (!cores.empty()) {
            long long sum = 0, max_pct = cores[0].first, max_mhz = cores[0].second;
            for (const auto& [pct, mhz] : cores) {
                sum += pct;
                max_pct = std::max(max_pct, pct);
                max_mhz = std::max(max_mhz, mhz);
            }
            m.set_int("cpu_online", static_cast<long long>(cores.size()));
            m.set_round1("cpu_avg_pct", static_cast<double>(sum) / static_cast<double>(cores.size()));
            m.set_int("cpu_max_pct", max_pct);
            m.set_int("cpu_mhz_max", max_mhz);
        }
    }
    if (std::regex_search(line, r, p.emc)) m.set_int("emc_pct", to_int(r[1]));
    if (std::regex_search(line, r, p.gpu)) m.set_int("gpu_pct", to_int(r[1]));
    if (std::regex_search(line, r, p.gpu_mhz)) m.set_int("gpu_mhz", to_int(r[1]));
    for (std::sregex_iterator it(line.begin(), line.end(), p.engine), end; it != end; ++it) {
        const auto& e = *it;
        long long v = 0;
        if (e[2] != "off") v = to_int(e[3].matched ? e[3].str() : e[4].matched ? e[4].str() : e[5].str());
        m.set_int(metric_key(e[1]) + "_mhz", v);
    }
    MetricList temps;
    for (std::sregex_iterator it(line.begin(), line.end(), p.temp), end; it != end; ++it)
        temps.set_float("temp_" + metric_key((*it)[1]) + "_c", std::strtod((*it)[2].str().c_str(), nullptr));
    // tegrastats는 센서가 없을 때 -256C를 낸다
    MetricList valid;
    for (const auto& t : temps.items())
        if (t.value > -200) valid.set_float(t.key, t.value);
    if (!valid.empty()) {
        m.update(valid);
        double mx = valid.items()[0].value;
        for (const auto& t : valid.items()) mx = std::max(mx, t.value);
        m.set_float("temp_max_c", mx);
    }
    for (std::sregex_iterator it(line.begin(), line.end(), p.power), end; it != end; ++it)
        m.set_int("power_" + metric_key((*it)[1]) + "_mw", to_int((*it)[2]));
    return m;
}

}  // namespace sysmon
