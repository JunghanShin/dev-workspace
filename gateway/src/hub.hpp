// 텔레메트리 스트림 중계 (design.md 9.7, Python workspace_rest/stream.py): 워커 스트림 소켓 → 화면 SSE.
//
//     워커 PUB ──(Unix 소켓, JSON 한 줄씩)──▶ Upstream(워커당 스레드 gw.up.<워커>) ──▶ Subscriber(화면마다) ──SSE──▶ 화면
//
// - 워커마다 위쪽 연결은 하나. 화면이 몇 개든 워커 쪽 부담은 같다
// - Upstream이 최신 상태(UpstreamState)를 가져 새 화면에는 바로 기준 상태(kf)를 준다
// - 화면마다 hz. 그 사이 변경은 Subscriber(SseMerge)가 주제별로 합친다(이벤트는 모두). 큐가 아니라 합친 상태 하나라 메모리 상한이 있다
// - 화면이 느리면 그 화면의 쓰기만 시간 초과로 끊긴다(SseSession을 도는 HTTP 스레드). 워커와 다른 화면은 영향 없음
// - 워커가 다시 시작하면 다시 붙고 새 기준 상태. 끊긴 동안은 {"stream":"down"}을 한 번
// - 화면이 모두 떠나고 idle_close_s가 지나면 Upstream이 스스로 정리
// - 화면 하나가 여러 워커를 한 연결로(/streams): MuxSession이 워커마다 Subscriber를 두고 HTTP 스레드 하나로 돈다
//
// 잠금 더: Subscriber::mu_를 놓은 뒤 Waker::mu_(겹치지 않음).
//
// 잠금(모두 짧고 잠금 안에서 I/O 없음): Hub::mu_(워커 → Upstream 표) → Upstream::mu_(구독자·상태) → Subscriber::mu_(합친 상태).
// 이 순서로만 겹쳐 잡는다.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "stream_state.hpp"

namespace gw {

// 여러 Subscriber를 스레드 하나가 기다릴 때(/streams, MuxSession): offer마다 세대를 올리고 깨운다
class Waker {
public:
    void poke();
    unsigned long long gen();
    // 세대가 seen에서 바뀌거나 timeout_s가 지날 때까지
    void wait(unsigned long long seen, double timeout_s);

private:
    std::mutex mu_;
    std::condition_variable cv_;
    unsigned long long gen_ = 0;
};

class Subscriber {
public:
    explicit Subscriber(double hz, std::shared_ptr<Waker> waker = nullptr)
        : min_interval_(1.0 / hz), waker_(std::move(waker)) {}
    void offer(const Json& msg, bool keyframe);
    // 합친 메시지 또는 없음(timeout_s 안에 온 것이 없음)
    std::optional<Json> take(double timeout_s);
    void close();
    double min_interval() const { return min_interval_; }

private:
    const double min_interval_;
    const std::shared_ptr<Waker> waker_;  // 없으면 cv_만
    std::mutex mu_;
    std::condition_variable cv_;
    SseMerge merge_;
    bool closed_ = false;
};

// 워커 스트림 소켓을 연다: 연결된 fd 또는 -1(why에 이유). Manager 조회(소켓 위치)도 여기서
using SourceOpener = std::function<int(const std::string& worker_id, std::string& why)>;

struct StreamTiming {
    double retry_s = 1.0;        // 다시 붙는 간격
    double idle_close_s = 5.0;   // 화면이 모두 떠난 뒤 위쪽 연결을 닫기까지
    size_t max_line = 4u << 20;  // 워커 메시지 한 줄 상한
};

class Hub;

class Upstream {
public:
    Upstream(std::string worker_id, Hub& hub, SourceOpener open, StreamTiming timing);
    ~Upstream();
    Upstream(const Upstream&) = delete;
    Upstream& operator=(const Upstream&) = delete;

    void start();
    // 정리하기로 정한 Upstream이면 false(Hub가 새로 만든다)
    bool subscribe(const std::shared_ptr<Subscriber>& sub);
    void unsubscribe(const std::shared_ptr<Subscriber>& sub);
    void stop();
    void join();

    const std::string& id() const { return id_; }
    bool running() const { return running_.load(); }
    bool up() const;
    size_t subscribers() const;
    double beat_age(double now) const;  // 마지막 beat 이후 초

private:
    void run();
    bool idle_locked(double now) const;
    bool retire();
    void set_up(bool up);
    void broadcast(const Json& msg, bool keyframe, std::vector<std::shared_ptr<Subscriber>> subs);
    void pump(int fd);
    void beat();
    void sleep_retry();

    const std::string id_;
    Hub& hub_;
    SourceOpener open_;
    const StreamTiming timing_;

    mutable std::mutex mu_;  // 아래 상태. 잠금 안에서는 계산·Subscriber::offer만
    std::set<std::shared_ptr<Subscriber>> subs_;
    UpstreamState state_;
    bool up_ = false;
    double idle_since_ = -1;  // 화면이 모두 떠난 시각(-1 = 화면 있음)
    bool dead_ = false;       // 정리하기로 정함(더 받지 않음)

    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> beat_{0};
    std::mutex wait_mu_;
    std::condition_variable wait_cv_;  // 다시 붙기 전 대기(stop이 깨움)
    std::thread thread_;
};

class Hub {
public:
    explicit Hub(SourceOpener open, StreamTiming timing = {});
    ~Hub();
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    std::pair<std::shared_ptr<Upstream>, std::shared_ptr<Subscriber>> subscribe(const std::string& worker_id, double hz,
                                                                                std::shared_ptr<Waker> waker = nullptr);
    // 끝난 Upstream 스레드를 거둔다(잠금 밖에서 join)
    void reap();
    void stop_all();
    std::map<std::string, size_t> stats();
    // beat가 limit_s보다 오래 없는 Upstream 이름(쉼표로). 없으면 ""
    std::string stalled(double limit_s);

private:
    friend class Upstream;
    void gone(Upstream* up);  // Upstream 스레드가 끝날 때

    SourceOpener open_;
    const StreamTiming timing_;
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Upstream>> ups_;
    std::vector<std::shared_ptr<Upstream>> done_;  // 정리 중·끝난 것(reap이 join)
};

// SSE 응답 본문(HTTP 스레드가 step을 되풀이). 쓰기는 write가(시간 상한은 HTTP 쪽)
class SseBody {
public:
    virtual ~SseBody() = default;
    // false면 끝(쓰기 실패·정지)
    virtual bool step(const std::function<bool(const std::string&)>& write, const std::atomic<bool>& stopping) = 0;
    virtual void finish() = 0;  // 구독 해제(한 번만)
};

// 화면 하나·워커 하나(/workers/{id}/stream)
class SseSession : public SseBody {
public:
    SseSession(std::shared_ptr<Upstream> up, std::shared_ptr<Subscriber> sub, double ping_s = 15.0);
    ~SseSession() override;
    // 한 번에 하나를 보내거나 기다린다(최대 약 1 s + hz 간격)
    bool step(const std::function<bool(const std::string&)>& write, const std::atomic<bool>& stopping) override;
    void finish() override;

private:
    std::shared_ptr<Upstream> up_;
    std::shared_ptr<Subscriber> sub_;
    const double ping_s_;
    bool retry_sent_ = false;
    double last_ = -1e18;   // 마지막 data 전송
    double last_write_;     // 마지막 쓰기(ping 판단)
    std::atomic<bool> finished_{false};
};

// 화면 하나·워커 여럿(/streams, docs/stream-mux.md). HTTP 스레드 하나가 워커마다의 Subscriber를 한 고리에서 돈다
//   - 워커마다 hz·합친 상태(Subscriber/SseMerge)는 따로 → 메모리 상한은 워커 수 × 하나짜리와 같음
//   - 줄마다 "worker"를 붙임(mux_line). 꺼진 워커는 {"worker", "down": true} 한 번, 다시 오면 기준 상태부터
//   - 기다림은 Waker 하나(어느 워커든 offer하면 깸). 한 step에서 보낼 때가 된 워커 줄을 모두 한 번에 씀
class MuxSession : public SseBody {
public:
    struct Feed {
        std::string id;
        std::shared_ptr<Upstream> up;
        std::shared_ptr<Subscriber> sub;
    };
    MuxSession(std::vector<Feed> feeds, std::shared_ptr<Waker> waker, double ping_s = 15.0);
    ~MuxSession() override;
    // 보낼 것을 모두 보내거나 기다린다(최대 0.2 s, 정지 요청을 봄)
    bool step(const std::function<bool(const std::string&)>& write, const std::atomic<bool>& stopping) override;
    void finish() override;

private:
    struct Entry {
        Feed f;
        double last = -1e18;  // 이 워커의 마지막 data 전송
        bool down = false;    // down 줄을 보냄(다시 올라올 때까지 더 안 보냄)
    };
    std::vector<Entry> es_;
    std::shared_ptr<Waker> waker_;
    const double ping_s_;
    bool retry_sent_ = false;
    double last_write_;
    std::atomic<bool> finished_{false};
};

}  // namespace gw
