// 기록 파일과 찾기(managers/record/design.md 2.3·2.4). 예전 workspace-logger Recorder와 같은 동작:
//
//   파일 이름: {처음 쓴 시각 %Y%m%d-%H%M%S}_{boot_id 앞 8자}.jsonl
//   - 같은 부팅에서 다시 뜨면 그 부팅의 마지막 파일에 이어 쓴다
//   - 파일이 max_file_bytes를 넘으면 새 파일(이름순 = 시간순)
//   - 전체가 max_total_bytes를 넘으면 오래된 파일부터 지운다(쓰는 중인 파일 제외)
//   - 줄마다 flush(기록기가 죽어도 커널까지), fsync_s마다 fsync(전원 차단 대비)
//
// 쓰기 스레드 하나만 RecordFile을 쓴다(잠금 없음). 찾기(find)는 파일만 읽으므로 다른 스레드에서 돌려도 된다
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "nlohmann/json.hpp"

namespace record {

using Json = nlohmann::ordered_json;

// 이 부팅의 boot_id 앞 8자(- 뺌). 읽지 못하면 "noboot00"
std::string boot_id();
// 지금 시각을 예전 logger 모양으로: 2026-11-24T10:00:00.123+09:00
std::string now_iso();
// 보드 CLOCK_MONOTONIC(ns)
int64_t mono_ns();
// 기록 하나 → 한 줄(끝에 \n). 맨 앞에 t, t_mono_ns를 붙인다(키 순서 고정, 공백 없음, UTF-8 그대로)
std::string make_line(const Json& rec, const std::string& t_iso, int64_t t_mono_ns);

class RecordFile {
public:
    RecordFile(std::string dir, uint64_t max_file_bytes, uint64_t max_total_bytes, double fsync_s,
               std::string boot = boot_id());
    ~RecordFile();
    RecordFile(const RecordFile&) = delete;
    RecordFile& operator=(const RecordFile&) = delete;

    // 한 줄 쓰기. 실패하면 false(error에 까닭). 실패해도 다음 줄에서 다시 연다
    bool write(const std::string& line, std::string* error = nullptr);
    void sync();
    void close();
    const std::string& path() const { return path_; }
    uint64_t files_opened() const { return files_opened_; }
    uint64_t removed() const { return removed_; }

private:
    bool open(const std::string& path, std::string* error);
    void prune();
    std::string dir_, boot_, path_;
    uint64_t max_file_, max_total_;
    double fsync_s_;
    std::FILE* f_ = nullptr;
    uint64_t size_ = 0;
    double last_sync_ = 0;
    uint64_t files_opened_ = 0, removed_ = 0;
};

// 찾기. q: since/until(사람 시각 문자열, 앞에서부터 비교), since_ns/until_ns(t_mono_ns, 이 부팅 파일만),
//      types(문자열 배열), src(글롭, 줄의 src 또는 id), topic(글롭), limit(기본 1000, 최대 5000)
// 답: {"lines": [...기록...], "more": bool, "files": n}
Json find(const std::string& dir, const Json& q, const std::string& boot = boot_id());

}  // namespace record
