#include "history.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>

namespace gw {

namespace {

bool digits(const std::string& s, size_t pos, size_t n, int& out) {
    if (pos + n > s.size()) return false;
    out = 0;
    for (size_t i = pos; i < pos + n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        out = out * 10 + (s[i] - '0');
    }
    return true;
}

int days_in_month(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

// "HH[:MM[:SS[.f]]]" → 초(하루 안), 소수 초는 6자리까지(나머지는 버림). pos는 다 읽은 뒤 위치
bool parse_time(const std::string& s, size_t& pos, int& h, int& mi, int& sec, int& us) {
    mi = sec = us = 0;
    if (!digits(s, pos, 2, h) || h > 23) return false;
    pos += 2;
    if (pos < s.size() && s[pos] == ':') {
        if (!digits(s, pos + 1, 2, mi) || mi > 59) return false;
        pos += 3;
        if (pos < s.size() && s[pos] == ':') {
            if (!digits(s, pos + 1, 2, sec) || sec > 59) return false;
            pos += 3;
            if (pos < s.size() && (s[pos] == '.' || s[pos] == ',')) {
                ++pos;
                size_t n = 0;
                int scale = 100000;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
                    if (n < 6) us += (s[pos] - '0') * scale, scale /= 10;
                    ++n, ++pos;
                }
                if (!n) return false;
            }
        }
    }
    return true;
}

bool is_bool_or_num(const Json& v) { return v.is_number() || v.is_boolean(); }

double num(const Json& v) {
    if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    return v.get<double>();
}

}  // namespace

bool parse_iso_ts(const std::string& s, double& out) {
    int y, mo, d;
    if (!digits(s, 0, 4, y) || s.size() < 10 || s[4] != '-' || !digits(s, 5, 2, mo) || s[7] != '-' ||
        !digits(s, 8, 2, d))
        return false;
    if (mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo) || y < 1) return false;
    size_t pos = 10;
    int h = 0, mi = 0, sec = 0, us = 0;
    bool aware = false;
    long off = 0;
    if (pos < s.size()) {
        // 날짜와 시각 사이는 아무 글자 하나(UTF-8이면 여러 바이트)
        size_t sep = 1;
        const unsigned char c = static_cast<unsigned char>(s[pos]);
        if (c >= 0xC0) sep = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        pos += sep;
        if (!parse_time(s, pos, h, mi, sec, us)) return false;
        if (pos < s.size()) {
            if (s[pos] == 'Z') {
                aware = true;
                ++pos;
            } else if (s[pos] == '+' || s[pos] == '-') {
                const int sign = s[pos] == '-' ? -1 : 1;
                ++pos;
                int oh, om = 0, os = 0, ous = 0;
                if (!digits(s, pos, 2, oh)) return false;
                pos += 2;
                if (pos < s.size()) {
                    if (s[pos] == ':') ++pos;
                    if (!digits(s, pos, 2, om)) return false;
                    pos += 2;
                    if (pos < s.size()) {
                        if (s[pos] == ':') ++pos;
                        if (!digits(s, pos, 2, os)) return false;
                        pos += 2;
                        if (pos < s.size() && s[pos] == '.') {
                            ++pos;
                            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
                            (void)ous;
                        }
                    }
                }
                if (oh > 23 || om > 59 || os > 59) return false;
                aware = true;
                off = sign * (oh * 3600L + om * 60L + os);
            }
            if (pos != s.size()) return false;
        }
    }
    tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = sec;
    time_t base;
    if (aware) {
        base = timegm(&t) - off;
    } else {
        t.tm_isdst = -1;
        base = mktime(&t);
    }
    out = static_cast<double>(base) + us / 1e6;
    return true;
}

std::string iso_local(double t) {
    // fromtimestamp: 마이크로초로 반올림(짝수 쪽) 뒤, 초 아래는 버림
    double sec = std::floor(t);
    const double us = std::nearbyint((t - sec) * 1e6);
    if (us >= 1e6) sec += 1;
    const time_t tt = static_cast<time_t>(sec);
    tm lt{};
    localtime_r(&tt, &lt);
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &lt);
    long off = lt.tm_gmtoff;
    const char sign = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    char tz[48];
    if (off % 60) std::snprintf(tz, sizeof tz, "%c%02ld:%02ld:%02ld", sign, off / 3600, (off / 60) % 60, off % 60);
    else std::snprintf(tz, sizeof tz, "%c%02ld:%02ld", sign, off / 3600, (off / 60) % 60);
    return std::string(buf) + tz;
}

bool history_row(const std::string& line, const std::string& needle, double since, const std::vector<std::string>* keys,
                 HistRow& out) {
    if (line.find(needle) == std::string::npos || line.find("\"type\":\"worker\"") == std::string::npos) return false;
    auto rec = parse_json(line);
    if (!rec || !rec->is_object()) return false;
    auto ti = rec->find("t");
    if (ti == rec->end() || !ti->is_string()) return false;
    double t;
    if (!parse_iso_ts(ti->get<std::string>(), t)) return false;
    auto mi = rec->find("metrics");
    if (t < since || mi == rec->end() || !mi->is_object()) return false;
    Json m = *mi;
    // Manager가 잰 워커 자원(자식 프로세스 포함)과 재시작 횟수도 지표처럼(res.*, restart_count)
    auto ri = rec->find("resources");
    if (ri != rec->end() && ri->is_object())
        for (auto it = ri->begin(); it != ri->end(); ++it) m["res." + it.key()] = *it;
    auto rc = rec->find("restart_count");
    if (rc != rec->end() && (rc->is_number_integer() || rc->is_boolean())) m["restart_count"] = *rc;
    if (keys && !keys->empty()) {
        Json picked = Json::object();
        for (const auto& k : *keys) {
            auto it = m.find(k);
            if (it != m.end()) picked[k] = *it;
        }
        m = std::move(picked);
    }
    out.t = t;
    out.m = Json::object();
    for (auto it = m.begin(); it != m.end(); ++it)
        if (is_bool_or_num(*it)) out.m[it.key()] = *it;
    return true;
}

std::vector<HistRow> history_reduce(std::vector<HistRow> rows, double now, double since, int points) {
    std::stable_sort(rows.begin(), rows.end(), [](const HistRow& a, const HistRow& b) { return a.t < b.t; });
    if (points <= 0 || rows.size() <= static_cast<size_t>(points)) return rows;
    const double span = (now - since) / points;
    struct Bucket {
        std::vector<std::string> order;  // 키가 처음 나온 순서
        std::map<std::string, std::pair<double, long>> acc;
    };
    std::map<long long, Bucket> buckets;
    for (const auto& r : rows) {
        Bucket& b = buckets[static_cast<long long>(std::floor((r.t - since) / span))];
        for (auto it = r.m.begin(); it != r.m.end(); ++it) {
            auto& a = b.acc[it.key()];
            if (a.second == 0) b.order.push_back(it.key());
            a.first += num(*it);
            a.second += 1;
        }
    }
    std::vector<HistRow> out;
    for (const auto& kv : buckets) {
        HistRow r;
        r.t = since + (static_cast<double>(kv.first) + 0.5) * span;
        for (const auto& k : kv.second.order) {
            const auto& a = kv.second.acc.at(k);
            r.m[k] = py_round(a.first / static_cast<double>(a.second), 2);
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<std::string> history_render(const std::vector<HistRow>& rows, const std::vector<std::string>* keys, bool csv) {
    std::vector<std::string> out;
    if (csv) {
        std::vector<std::string> cols;
        if (keys && !keys->empty()) {
            cols = *keys;
        } else {
            std::set<std::string> all;
            for (const auto& r : rows)
                for (auto it = r.m.begin(); it != r.m.end(); ++it) all.insert(it.key());
            cols.assign(all.begin(), all.end());
        }
        std::string head = "time";
        for (const auto& c : cols) head += "," + c;
        out.push_back(head);
        for (const auto& r : rows) {
            std::string line = iso_local(r.t);
            for (const auto& c : cols) {
                auto it = r.m.find(c);
                line += "," + (it == r.m.end() || it->is_null() ? std::string() : py_str(*it));
            }
            out.push_back(line);
        }
        return out;
    }
    for (const auto& r : rows) {
        Json o = Json::object();
        o["t"] = iso_local(r.t);
        o["m"] = r.m;
        out.push_back(compact(o, true));
    }
    return out;
}

}  // namespace gw
