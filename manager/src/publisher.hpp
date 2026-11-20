// pm.pub: 구독(subscribe) 송신 스레드 (design.md 6.2, docs/process-manager.md 2.1).
//
// 루프는 구독 연결을 넘기고(add), 이벤트(offer)와 주기 전체 상태(offer_to)를 큐로 보낸다. 이 스레드가
// 구독자마다 non-blocking으로 보낸다. 루프는 송신을 기다리지 않는다.
//
// - 구독자마다 "보낼 것"은 키(이벤트 종류 + 워커 id)마다 최신 한 줄만 둔다. 보내는 중이라 못 보낸 것은 덮어쓴다
//   (느린 구독자에게는 최신값만, Python 판과 같음)
// - 보낼 버퍼가 subscriber_timeout_s 동안 다 나가지 않으면(안 읽는 구독자) 그 구독자만 끊는다
// - 끊긴 구독자 id는 dropped 큐로 루프에 알린다(상태 system.clients)
#pragma once

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "worker/queue.hpp"
#include "worker/thread.hpp"

namespace pm {

class Publisher {
public:
    struct Item {
        std::string topic, key, line;  // line은 줄바꿈 포함
    };

    Publisher(worker::ThreadWatch& watch, double subscriber_timeout_s);
    ~Publisher();

    void start();
    void stop();

    // ---- 루프 스레드에서 ----
    // fd 소유권을 넘긴다. first는 첫 줄(구독 응답). 큐가 차면 false(루프가 fd를 닫음)
    bool add(int id, int fd, std::set<std::string> topics, std::string first);
    void offer(Item item);                               // 그 topic을 구독한 모두에게
    void offer_to(std::vector<int> ids, std::vector<Item> items);  // 주기 전체 상태
    int dropped_fd() const { return dropped_.wake_fd(); }
    std::vector<int> take_dropped();

private:
    struct Msg {
        enum class Kind { Add, Offer, OfferTo } kind = Kind::Offer;
        int id = 0, fd = -1;
        std::set<std::string> topics;
        std::string first;
        std::vector<int> ids;
        std::vector<Item> items;
    };
    struct Sub {
        int id = 0, fd = -1;
        std::set<std::string> topics;
        std::vector<std::pair<std::string, std::string>> pending;  // (key, line) 들어온 순서
        std::unordered_map<std::string, size_t> index;             // key → pending 자리
        std::string out;                                           // 보내는 중
        double stuck_since = 0;                                    // out이 남기 시작한 시각(0 = 비었음)
        bool read_eof = false;
    };
    void run(worker::WorkerThread& t);
    void apply(Msg& m);
    static void put(Sub& s, const Item& it);
    bool flush(Sub& s, double now);  // false = 끊어야 함

    const double timeout_s_;
    worker::BoundedQueue<Msg> queue_;
    worker::BoundedQueue<int> dropped_;
    worker::WorkerThread thread_;
    std::map<int, Sub> subs_;  // 이 스레드만
};

}  // namespace pm
