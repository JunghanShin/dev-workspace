// 데이터 버스 녹화 파일(유닛, libworker를 모름). 녹화 하나 = 폴더 하나:
//
//   <녹화 폴더>/<이름>/meta.json     형식(폭·높이·stride·픽셀 형식·슬롯 크기), 원본 워커, 만든 시각, 프레임 수·바이트·길이
//   <녹화 폴더>/<이름>/frames.bin    "FBREC1\n\0"(8바이트) 뒤에 레코드가 이어짐: 헤더 24바이트 {frame_id u64, ts_ns i64,
//                                    bytes u32, 예약 u32}(리틀 엔디언) + 데이터 bytes
//
// 녹화는 받은 그대로(번호·캡처 시각·내용) 남기고, 재생은 같은 간격으로 다시 내보낸다. 녹화 도중 끊겨도 앞부분은 읽힌다
// (meta.json을 못 썼으면 frames.bin만으로 다시 센다).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay {

struct Meta {
    uint32_t width = 0, height = 0, stride = 0, slot_bytes = 0;
    std::string pixel;   // 데이터 버스 형식 이름(GRAY8, f32x256 …)
    std::string source;  // 녹화한 워커 id
    std::string created; // ISO 시각
    uint64_t frames = 0;
    uint64_t bytes = 0;      // 데이터 바이트 합
    int64_t duration_ns = 0; // 첫 프레임 ~ 마지막 프레임 캡처 시각
};

struct Record {
    uint64_t frame_id = 0;
    int64_t ts_ns = 0;
    std::vector<uint8_t> data;
};

// 녹화 이름: 영문·숫자·-·_ 1~64자(폴더 이름이 되므로)
bool valid_name(const std::string& name);

class Writer {
public:
    Writer() = default;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // dir/<name>/을 만든다(이미 있으면 실패). meta의 형식 칸을 기록한다
    bool open(const std::string& root, const std::string& name, const Meta& meta, std::string* error);
    bool write(uint64_t frame_id, int64_t ts_ns, const uint8_t* data, size_t n);
    // meta.json을 마무리하고 닫는다
    bool close(std::string* error = nullptr);
    bool is_open() const { return f_ != nullptr; }
    const Meta& meta() const { return meta_; }

private:
    bool write_meta(std::string* error);
    std::string dir_;
    Meta meta_;
    std::FILE* f_ = nullptr;
    int64_t first_ts_ = 0;
    bool have_first_ = false;
};

class Reader {
public:
    Reader() = default;
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool open(const std::string& root, const std::string& name, std::string* error);
    const Meta& meta() const { return meta_; }
    // 다음 레코드. 끝이거나 잘린 레코드면 false
    bool next(Record* out);
    void rewind();
    void close();

private:
    Meta meta_;
    std::FILE* f_ = nullptr;
};

// 녹화 목록(이름순)과 크기
struct Info {
    std::string name;
    Meta meta;
    uint64_t file_bytes = 0;
};
std::vector<Info> list(const std::string& root);
uint64_t total_bytes(const std::string& root);
bool remove(const std::string& root, const std::string& name, std::string* error);

}  // namespace replay
