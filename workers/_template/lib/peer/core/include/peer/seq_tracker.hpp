// 유닛: 받은 레코드 번호로 받은 수·빠진 수를 센다(상대가 다시 시작해 번호가 처음으로 돌아가면 새로 센다).
// ① 기능 코어: libworker를 모른다.
#pragma once

#include <cstdint>

namespace peer {

class SeqTracker {
public:
    // 레코드 하나를 받았다. 빠진 개수(앞 번호와의 틈)를 돌려준다
    uint64_t on(uint64_t seq) {
        uint64_t gap = 0;
        if (have_ && seq > last_ + 1) gap = seq - last_ - 1;
        if (have_ && seq <= last_) ++restarts_;  // 번호가 되돌아감: 상대가 다시 시작
        last_ = seq;
        have_ = true;
        ++received_;
        lost_ += gap;
        return gap;
    }
    void reset() { *this = SeqTracker{}; }
    uint64_t received() const { return received_; }
    uint64_t lost() const { return lost_; }
    uint64_t last() const { return last_; }
    uint64_t restarts() const { return restarts_; }

private:
    bool have_ = false;
    uint64_t last_ = 0, received_ = 0, lost_ = 0, restarts_ = 0;
};

}  // namespace peer
