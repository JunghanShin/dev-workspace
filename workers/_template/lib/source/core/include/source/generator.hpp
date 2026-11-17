// 유닛: 주기마다 숫자 배열(표본)을 만든다. 예제의 "장비에서 데이터를 받는 자리"(실제 워커는 카메라 프레임·센서 값).
// ① 기능 코어: libworker·소켓·스레드를 모른다. 시각은 인자로 받는다(시험에서 가짜 시계).
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace source {

// 코어 자신의 설정 오류(모듈이 worker::ConfigError로 바꾼다)
struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct GeneratorConfig {
    double rate_hz = 10;     // 초당 표본 수
    int size = 256;          // 표본 하나의 값 개수
    double amplitude = 1.0;  // 값의 크기(사인파 진폭)
};

class Generator {
public:
    explicit Generator(GeneratorConfig cfg = {});
    void set_config(GeneratorConfig cfg);  // 잘못된 값이면 ConfigError
    const GeneratorConfig& config() const { return cfg_; }

    // 다음 표본을 만들 때가 됐으면 true(now_ms: 단조 시계 ms). 처음 호출은 기준만 잡는다
    bool due(int64_t now_ms);
    // 표본 하나를 만든다(번호 증가). 값: amplitude * sin(위상) + 번호 * 0.001
    const std::vector<float>& make();
    int64_t next_due_ms() const { return next_ms_; }
    uint64_t seq() const { return seq_; }
    void reset() { seq_ = 0; }

private:
    GeneratorConfig cfg_;
    int64_t next_ms_ = -1;
    uint64_t seq_ = 0;
    std::vector<float> buf_;
};

}  // namespace source
