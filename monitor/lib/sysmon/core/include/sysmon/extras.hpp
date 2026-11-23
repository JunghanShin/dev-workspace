// tegrastats 밖 지표: /proc·/sys에서 (monitor/design.md 2장). libworker를 모른다.
//
//   disk_used_gb disk_total_gb   / 파일 시스템
//   net_rx_kbps net_tx_kbps      기본 경로 인터페이스의 수신·송신(직전 표본 대비, 첫 표본엔 없음)
//   fan_pct fan_rpm              팬 PWM(0~100%)·회전수(hwmon pwmfan·pwm_tach가 있을 때)
//   uptime_s                     가동 시간
// 읽지 못하는 항목은 빠진다. root는 시험용 앞 경로(가짜 /proc·/sys 나무). 보드에서는 ""
#pragma once

#include <optional>
#include <string>

#include "sysmon/metrics.hpp"

namespace sysmon {

class SystemExtras {
public:
    explicit SystemExtras(std::string root = "", std::string disk_path = "/");
    // now_s: 단조 시각(초). 네트워크 속도를 직전 표본과의 차로 낸다
    MetricList sample(double now_s);
    const std::string& fan_pwm_path() const { return fan_pwm_; }
    const std::string& fan_rpm_path() const { return fan_rpm_; }

private:
    std::string root_, disk_path_;
    std::string fan_pwm_, fan_rpm_;
    bool have_prev_ = false;
    std::string prev_iface_;
    double prev_t_ = 0;
    long long prev_rx_ = 0, prev_tx_ = 0;
};

// /proc/net/route에서 기본 경로(대상 00000000, UP)의 인터페이스. 여럿이면 metric이 가장 작은 것. 없으면 ""
std::string default_iface(const std::string& root = "");

// nvpmodel 모드 번호(/var/lib/nvpmodel/status "pmode:0002" → 2). 읽지 못하면 없음
std::optional<int> read_power_mode(const std::string& root = "");

// 파일 내용(앞뒤 공백 뺌). 열지 못하면 없음
std::optional<std::string> read_text(const std::string& path);

}  // namespace sysmon
