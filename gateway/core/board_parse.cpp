#include "board_parse.hpp"

namespace gw {

namespace {

long long to_ll(const std::string& s) {
    long long v;
    if (!py_int(s, v)) throw std::invalid_argument("invalid literal for int() with base 10: '" + s + "'");
    return v;
}

double to_d(const std::string& s) {
    double v;
    if (!py_float(s, v)) throw std::invalid_argument("could not convert string to float: '" + s + "'");
    return v;
}

}  // namespace

Json parse_ps(const std::string& out, long long n) {
    Json procs = Json::array();
    const auto lines = splitlines(out);
    for (size_t i = 0; i < lines.size() && static_cast<long long>(i) < n; ++i) {
        const auto parts = split_ws(lines[i]);
        if (parts.size() < 5) continue;
        std::string name;
        for (size_t k = 1; k + 3 < parts.size(); ++k) name += (k > 1 ? " " : "") + parts[k];
        Json p = Json::object();
        p["pid"] = to_ll(parts[0]);
        p["name"] = name;
        p["cpu"] = to_d(parts[parts.size() - 3]);
        p["mem"] = to_d(parts[parts.size() - 2]);
        p["rss_kb"] = to_ll(parts[parts.size() - 1]);
        procs.push_back(p);
    }
    return procs;
}

Json parse_nvpmodel(const std::string& text) {
    // ^< POWER_MODEL ID=(\d+) NAME=(\S+) >
    Json modes = Json::object();
    const std::string head = "< POWER_MODEL ID=";
    for (const auto& line : splitlines(text)) {
        if (!starts_with(line, head)) continue;
        size_t i = head.size(), j = i;
        while (j < line.size() && line[j] >= '0' && line[j] <= '9') ++j;
        if (j == i || line.compare(j, 6, " NAME=") != 0) continue;
        size_t k = j + 6, e = k;
        while (e < line.size() && !(line[e] == ' ' || (line[e] >= 0x09 && line[e] <= 0x0d))) ++e;
        if (e == k || line.compare(e, 2, " >") != 0) continue;
        modes[line.substr(i, j - i)] = line.substr(k, e - k);
    }
    return modes;
}

bool parse_proc_stat(const std::string& data, std::string& comm, std::vector<std::string>& fields) {
    const size_t a = data.find('('), b = data.rfind(')');
    if (a == std::string::npos || b == std::string::npos || b < a) return false;
    comm = data.substr(a + 1, b - a - 1);
    fields = b + 2 <= data.size() ? split_ws(data.substr(b + 2)) : std::vector<std::string>{};
    return true;
}

std::string policy_name(long long p) {
    switch (p) {
        case 0: return "normal";
        case 1: return "realtime(fifo)";
        case 2: return "realtime(rr)";
        case 3: return "batch";
        case 5: return "idle";
        default: return std::to_string(p);
    }
}

void BuildState::apply_line(const std::string& line) {
    if (starts_with(line, "##PROGRESS ")) {
        // line.split(" ", 2)
        const size_t s1 = line.find(' ');
        const size_t s2 = line.find(' ', s1 + 1);
        const std::string pct = line.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
        long long v;
        if (py_int(pct, v)) st["pct"] = v;
        st["step"] = s2 == std::string::npos ? "" : line.substr(s2 + 1);
        log.push_back("\xC2\xB7 " + st["step"].get<std::string>());  // "· "
    } else if (starts_with(line, "##RESULT ")) {
        auto j = parse_json(line.substr(9));
        if (j) st["result"] = *j;
    } else if (starts_with(line, "##ERROR ")) {
        st["error"] = line.substr(8);
    } else if (!strip(line).empty()) {
        log.push_back(line);
    }
    if (log.size() > kBuildLogLines) log.erase(log.begin(), log.end() - static_cast<long>(kBuildLogLines));
}

void BuildState::finish(int code, double now) {
    st["finished"] = now;
    const std::string state = st.value("state", "");
    if (state == "cancelled") return;
    if (code == 0 && st.contains("result") && py_truthy(st["result"])) {
        st["state"] = "done";
    } else {
        st["state"] = "failed";
        if (!st.contains("error") || !py_truthy(st["error"])) st["error"] = "exit " + std::to_string(code);
    }
}

}  // namespace gw
