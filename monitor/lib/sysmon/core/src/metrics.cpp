#include "sysmon/metrics.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sysmon {

void MetricList::put(const std::string& key, double v, std::string text) {
    for (auto& m : items_) {
        if (m.key == key) {
            m.value = v;
            m.text = std::move(text);
            return;
        }
    }
    items_.push_back({key, v, std::move(text)});
}

void MetricList::set_int(const std::string& key, long long v) { put(key, static_cast<double>(v), std::to_string(v)); }

void MetricList::set_float(const std::string& key, double v) { put(key, v, py_float(v)); }

void MetricList::set_round1(const std::string& key, double v) {
    // glibc printf는 이진 값을 정확히 반올림(딱 절반이면 짝수 쪽) = Python round(v, 1)의 글자
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    put(key, std::strtod(buf, nullptr), buf);
}

void MetricList::update(const MetricList& other) {
    for (const auto& m : other.items_) put(m.key, m.value, m.text);
}

const Metric* MetricList::find(const std::string& key) const {
    for (const auto& m : items_)
        if (m.key == key) return &m;
    return nullptr;
}

std::string MetricList::str() const {
    std::string out;
    for (const auto& m : items_) out += (out.empty() ? "" : " ") + m.key + "=" + m.text;
    return out;
}

std::string py_float(double v) {
    char buf[64];
    if (!std::isfinite(v)) return v != v ? "nan" : (v > 0 ? "inf" : "-inf");
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*f", p, v);
        if (std::strtod(buf, nullptr) == v) return buf;
    }
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

}  // namespace sysmon
