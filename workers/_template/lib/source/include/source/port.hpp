// 포트: source 모듈이 같은 워커의 다른 모듈에 내주는 작은 인터페이스(워커 안 연결, design.md 6.6.3).
// 받는 모듈은 이 헤더만 안다(source 모듈의 다른 헤더를 include하지 않는다). 같은 포트를 구현한 다른 모듈로 바꿔 끼울 수 있다.
// 워커 간 연결은 포트가 아니라 통로(데이터 버스·스트림·명령 소켓, design.md 6.6.7)로 한다.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace source {

struct Sample {
    uint64_t seq = 0;
    int64_t ts_ms = 0;                 // 단조 시계 ms
    const std::vector<float>* values;  // 콜백 안에서만 유효
};

class SourcePort {
public:
    virtual ~SourcePort() = default;
    // 표본이 생길 때마다 부른다(워커 루프 안, 짧게 끝낼 것)
    using Listener = std::function<void(const Sample&)>;
    virtual void subscribe(Listener fn) = 0;
};

}  // namespace source
