// 지표 목록(순서 유지): heartbeat에 "키=값"으로 나간다. libworker를 모른다(워크스페이스 design.md 6.6.4 ①).
//
// 같은 키를 다시 넣으면 자리는 그대로 값만 바뀐다(옛 Python 판 dict와 같은 순서·같은 값 글자).
// 값 글자: 정수는 그대로, 실수는 Python repr과 같게(45.0, 46.937), 소수 한 자리 반올림은 "%.1f"(Python round(x, 1)과 같음)
#pragma once

#include <string>
#include <vector>

namespace sysmon {

struct Metric {
    std::string key;
    double value = 0;  // 비교·최댓값용
    std::string text;  // heartbeat에 나가는 글자
};

class MetricList {
public:
    void set_int(const std::string& key, long long v);
    void set_float(const std::string& key, double v);   // Python repr(float)
    void set_round1(const std::string& key, double v);  // Python round(v, 1)
    void update(const MetricList& other);                // dict.update
    const Metric* find(const std::string& key) const;
    bool empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }
    const std::vector<Metric>& items() const { return items_; }
    std::string str() const;  // "k=v k=v"

private:
    void put(const std::string& key, double v, std::string text);
    std::vector<Metric> items_;
};

// Python repr(float)과 같은 글자: 되읽어 같은 값이 되는 가장 짧은 소수 자리(최소 한 자리)
std::string py_float(double v);

}  // namespace sysmon
