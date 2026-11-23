// 모듈 sysmon: tegrastats 한 줄 → 보드 상태 지표(monitor/design.md). 코어(parse_tegrastats, SystemExtras)를 감싼다.
//
// 설정 config["sysmon"]: interval_ms(기본 1000, 100 이상). 예전 형식(맨 위 interval_ms)도 받는다(main의 legacy_keys)
// 출처(지표 collector_root):
//   1 = root 수집기 <WORKSPACE_DATA>/run/tegrastats.line. 시작할 때 interval의 3배 안에 바뀌었으면 이것.
//       interval/4마다 mtime을 보고 바뀌면 읽는다. interval의 3배 넘게 안 바뀌면 장치 오류(exit 2)
//   0 = 직접 실행: 자식 tegrastats --interval <interval_ms>, stdout 파이프를 호스트 루프의 poll로 읽는다
// 첫 줄: start()에서 알아볼 수 있는 첫 줄까지 기다린다(그 뒤 호스트가 READY). 5 s + interval 넘게 알아볼 수 없는 줄만
//        오면, 또는 5 s + interval + watchdog_sec 동안 줄이 없으면 장치 오류
// 실행 중: 직접 실행에서 watchdog_sec + interval 동안 줄이 없거나 tegrastats가 끝나면 장치 오류
// 장치 오류는 failed()(시작 중이면 예외)로 호스트를 끝내고, main이 exit code를 2로 바꾼다(device_error())
// 지표는 접두어 없이 옛 이름 그대로(Metrics::raw). 줄마다 새 표본, heartbeat(1 s)는 최신 표본
#pragma once

#include <sys/types.h>

#include <optional>
#include <string>

#include "sysmon/extras.hpp"
#include "sysmon/metrics.hpp"
#include "worker/host.hpp"

namespace sysmon {

class Module : public worker::Module {
public:
    // root·tegrastats: 시험용(가짜 /proc·/sys, 가짜 tegrastats). 보드에서는 기본값
    explicit Module(std::string root = "") : root_(std::move(root)), extras_(root_) {}

    std::string name() const override { return "sysmon"; }
    void configure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    void add_pollfds(std::vector<pollfd>& fds) override;
    void on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) override;
    void on_timer(worker::TimePoint now) override;
    worker::TimePoint next_wakeup(worker::TimePoint now) const override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return count_; }
    bool failed(std::string* why) const override;
    void stop() override;

    // 장치 오류(tegrastats 없음·비정상 종료·멈춤·수집기 멈춤)로 끝났나 → exit 2
    bool device_error() const { return !fail_.empty(); }
    int interval_ms() const { return interval_ms_; }

private:
    void line(const std::string& text, worker::TimePoint now);
    void read_pipe(worker::TimePoint now);
    void check_collector(worker::TimePoint now);
    void check_silence(worker::TimePoint now);
    void fail(const std::string& why);
    void close_child();
    double interval_s() const { return interval_ms_ / 1000.0; }

    std::string root_;
    int interval_ms_ = 1000;
    double watchdog_sec_ = 3;
    // 출처
    bool collector_ = false;
    std::string collector_path_;
    long long collector_mtime_ns_ = -1;  // 마지막으로 읽은 mtime(-1: 아직)
    worker::TimePoint collector_changed_{}, next_check_{};
    pid_t pid_ = -1;
    int fd_ = -1;
    std::string buf_;
    // 표본
    SystemExtras extras_;
    std::optional<int> power_mode_;
    MetricList latest_;
    uint64_t count_ = 0;
    worker::TimePoint started_{}, last_line_{};
    std::string fail_;
};

// PATH에서 실행 파일 찾기(shutil.which). 없으면 ""
std::string which(const std::string& name);

}  // namespace sysmon
