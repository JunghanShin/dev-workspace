// tegrastats 한 줄 → 지표 (monitor/design.md 2장). libworker를 모른다.
//
// 표기는 JetPack 버전마다 다르다. 받는 꼴(옛 Python 판 정규식 그대로):
//   RAM 3456/7620MB  SWAP 0/3810MB                          ram_used_mb ram_total_mb swap_used_mb swap_total_mb
//   CPU [11%@1497,off,2%@729,...]                           cpu<N>_pct cpu<N>_mhz(꺼진 코어 빠짐), cpu_online cpu_avg_pct
//                                                           cpu_max_pct cpu_mhz_max
//   EMC_FREQ 3%  /  EMC_FREQ 0%@2133                        emc_pct
//   GR3D_FREQ 0%  /  GR3D_FREQ 0%@[305]  /  GR3D_FREQ 0%@305  gpu_pct gpu_mhz
//   NVENC 1036 / NVENC off / VIC_FREQ 0%@115 / NVDLA0_FREQ @1600 / OFA_FREQ @[408]
//                                                           nvenc nvdec nvjpg vic nvdla0 nvdla1 ofa _mhz(off = 0)
//   cpu@46.937C soc2@44.281C tj@47.125C  /  AO@38C GPU@-256C  temp_<부위>_c(-256C = 센서 없음, 뺌), temp_max_c
//   VDD_IN 4524mW/4524mW  VIN_SYS_5V0 2212mW/2212mW         power_<레일>_mw(소문자)
// 없는 항목은 빠진다. 알아볼 항목이 하나도 없으면 빈 목록
#pragma once

#include <string>

#include "sysmon/metrics.hpp"

namespace sysmon {

MetricList parse_tegrastats(const std::string& line);

// 이름 → 지표 키 조각: 소문자, 영숫자 밖은 "_" 하나로, 앞뒤 "_" 뺌 ("VDD_GPU_SOC" → "vdd_gpu_soc")
std::string metric_key(const std::string& name);

}  // namespace sysmon
