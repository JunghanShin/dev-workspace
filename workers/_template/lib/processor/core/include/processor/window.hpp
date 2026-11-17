// 유닛: 최근 N개 값의 평균·최소·최대. 예제의 "받은 데이터를 처리하는 자리"(실제 워커는 영상 처리, 상태 계산 등).
// ① 기능 코어: libworker를 모른다.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>

namespace processor {

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Window {
public:
    explicit Window(int size = 50);
    void set_size(int size);  // 2..10000, 아니면 ConfigError. 줄이면 오래된 값부터 버림
    int size() const { return size_; }
    void add(double v);
    void reset();
    size_t count() const { return values_.size(); }
    uint64_t total() const { return total_; }  // 지금까지 받은 수
    double avg() const;
    double min() const;
    double max() const;

private:
    int size_ = 50;
    std::deque<double> values_;
    double sum_ = 0;
    uint64_t total_ = 0;
};

}  // namespace processor
