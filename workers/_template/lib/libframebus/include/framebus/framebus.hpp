// 프레임(데이터) 버스: 같은 보드의 워커 사이에서 큰 데이터를 복사 없이 넘긴다 (design.md 6.7).
// 영상 프레임(폭·높이·픽셀 형식)과 영상이 아닌 데이터(Format::raw: 최대 크기 + 형식 이름, 예: 센서 배열)를 같은 방식으로 다룬다.
//
//   생산자(장치 워커) ── 공유 메모리 슬롯 K개(memfd) ── 소비자(영상 워커 등)
//              └──── 알림 소켓(Unix SOCK_SEQPACKET): 접속 시 memfd + 형식 1회, 프레임마다 {slot, 번호, 시각} ────┘
//
// 규칙
// - 생산자는 기다리지 않는다. 빈 슬롯이 없으면 그 프레임을 버린다(drop). 알림도 논블로킹, 못 보내면 그 소비자는 건너뛴다
// - 소비자는 둘 중 고른다: Latest(최신 우선, 밀린 것은 바로 반납 — 영상 송출처럼 지연이 중요할 때)
//   / InOrder(순서대로 전부 — 데이터 처리처럼 빠짐없이 받아야 할 때. 생산자 슬롯이 큐 역할, 넘치면 생산자가 새 것을 버림)
// - 슬롯은 참조 수로 관리한다. 소비자가 끊기면 그 소비자의 참조를 모두 푼다
// - 버퍼는 지금은 CPU 공유 메모리(memfd). 나중에 GPU 버퍼(NvBufSurface)로 바꿔도 이 인터페이스는 같다
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace framebus {

struct Format {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;        // 한 줄 바이트
    std::string pixel = "GRAY8"; // GStreamer 형식 이름(GRAY8, GRAY16_LE, RGB, ...)
    uint32_t frame_bytes() const { return stride * height; }
    // 영상이 아닌 데이터: 레코드 하나의 최대 크기와 형식 이름(소비자가 해석에 씀, 예: "f32x256", "json"). 15자 이하
    static Format raw(uint32_t max_bytes, std::string type = "RAW") {
        Format f;
        f.width = max_bytes;
        f.height = 1;
        f.stride = max_bytes;
        f.pixel = std::move(type);
        return f;
    }
};

struct Stats {
    uint64_t published = 0;  // 알린 프레임
    uint64_t dropped = 0;    // 빈 슬롯이 없어 버린 프레임
    uint64_t skipped = 0;    // 소비자 소켓이 차서 그 소비자에게 못 보낸 알림
    int consumers = 0;
};

// ---- 생산자 ---------------------------------------------------------------------------
// 쓰기: auto s = p.acquire(); if (s) { memcpy(s->data, ...) 또는 장치가 바로 씀; p.publish(*s, frame_id, ts_ns); }
// acquire·publish는 한 스레드(장치 스레드)에서 부른다. 접속·반납 처리는 내부 스레드가 한다
class Producer {
public:
    struct Slot {
        int index = -1;
        uint8_t* data = nullptr;
        size_t bytes = 0;
    };

    // path: 알림 소켓 경로(기존 파일은 지움). slots: 슬롯 수 K
    Producer(std::string path, Format format, int slots);
    ~Producer();
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    // 소켓·메모리를 만들고 내부 스레드를 시작한다. 실패하면 false와 error
    bool start(std::string* error);
    void stop();

    // 쓸 수 있는 빈 슬롯. 없으면 nullopt(그 프레임은 버린다, dropped 증가)
    std::optional<Slot> acquire();
    // 다 쓴 슬롯을 소비자들에게 알린다. 받을 소비자가 없으면 슬롯은 바로 빈다
    void publish(const Slot& slot, uint64_t frame_id, int64_t ts_mono_ns);
    // 실제로 쓴 크기가 슬롯보다 작을 때(크기가 바뀌는 데이터 레코드). bytes는 슬롯 크기 이하
    void publish(const Slot& slot, uint64_t frame_id, int64_t ts_mono_ns, size_t bytes);
    // 쓰다가 그만둔 슬롯(장치 오류 등)을 되돌린다
    void abandon(const Slot& slot);

    Stats stats() const;
    const Format& format() const { return format_; }
    const std::string& path() const { return path_; }

private:
    struct Consumer {
        int fd = -1;
        std::vector<int> held;  // 슬롯별 이 소비자가 들고 있는 수
    };
    void serve();
    void accept_one();
    void drop_consumer(size_t i);

    std::string path_;
    Format format_;
    int slots_;
    size_t slot_bytes_ = 0;
    int listen_fd_ = -1;
    int mem_fd_ = -1;
    int wake_[2] = {-1, -1};
    uint8_t* base_ = nullptr;
    std::thread thread_;
    mutable std::mutex mu_;
    std::vector<int> refs_;      // 슬롯별 참조 수(소비자가 들고 있는 수)
    std::vector<bool> writing_;  // 생산자가 쓰는 중
    std::vector<Consumer> consumers_;
    Stats stats_;
    bool running_ = false;
};

// ---- 소비자 ---------------------------------------------------------------------------
struct Frame {
    int slot = -1;
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    uint64_t frame_id = 0;
    int64_t ts_mono_ns = 0;
};

// 쓰기: c.connect(); 루프에서 auto f = c.next(timeout_ms); if (f) { 쓰고; c.release(*f); }
// 한 스레드에서만 쓴다. 생산자가 끝나면 next()가 false(연결 끊김)를 알린다 → 다시 connect
class Consumer {
public:
    enum class Mode { Latest, InOrder };
    explicit Consumer(std::string path, Mode mode = Mode::Latest);
    ~Consumer();
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;

    bool connect(std::string* error);
    void close();
    bool connected() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    const Format& format() const { return format_; }

    enum class Result { Frame, Timeout, Disconnected };
    // Latest: 최신 프레임 하나(밀린 알림은 반납). InOrder: 받은 순서대로 하나(밀린 것은 다음 next에서)
    Result next(int timeout_ms, Frame* out);
    size_t pending() const { return pending_.size(); }  // InOrder에서 아직 꺼내지 않은 알림
    void release(const Frame& f);
    // 최신 우선으로 건너뛴 프레임. next()를 도는 스레드가 아닌 곳(상태 보고 등)에서 읽어도 된다
    uint64_t skipped() const { return skipped_.load(std::memory_order_relaxed); }

private:
    bool send_release(int slot);
    struct Note {
        int slot;
        uint64_t frame_id;
        int64_t ts_mono_ns;
        uint32_t bytes;
    };
    bool fill(Frame* out);
    std::string path_;
    Mode mode_ = Mode::Latest;
    std::deque<Note> pending_;
    int fd_ = -1;
    uint8_t* base_ = nullptr;
    size_t map_bytes_ = 0;
    size_t slot_bytes_ = 0;
    int slots_ = 0;
    Format format_;
    std::atomic<uint64_t> skipped_{0};
};

}  // namespace framebus
