// 영상 경로를 단계별 명령으로 만든다(순수 함수, 프로세스는 다루지 않음).
//
//   relay:   ffmpeg [입력: RTSP 수신] → -c copy → [출력: 송출]
//   encode:  gst-launch [입력: RTSP 수신 → 디페이 | 표준 입력 원본 프레임] → [처리: (NVDEC) → 그리기 → NVENC] → stdout
//            → ffmpeg [파이프 입력] → -c copy → [출력: 송출]
#pragma once

#include <string>
#include <vector>

#include "framebus/framebus.hpp"
#include "video/stream.hpp"

namespace video {

// 입력 단계(gst). rtsp: RTSP 수신 → 디페이 → 압축 영상. framebus: 표준 입력(원본 프레임, raw 형식) → NVMM I420
std::vector<std::string> gst_input_stage(const StreamConfig& c, const framebus::Format* raw = nullptr);
// 처리 단계(gst): (rtsp면 NVDEC) → (시각 표시) → NVENC H.264
std::vector<std::string> gst_encode_stage(const StreamConfig& c);
// encode 때 gst-launch 전체 명령(입력 + 처리 + stdout). framebus면 raw에 프레임 형식
std::vector<std::string> gst_command(const StreamConfig& c, const framebus::Format* raw = nullptr);
// 출력 단계(ffmpeg) 인자
std::vector<std::string> ffmpeg_output_stage(const StreamConfig& c);
// ffmpeg 전체 명령. relay는 RTSP를 직접 받고, encode는 파이프(stdin)로 받는다. 진행은 fd 3
std::vector<std::string> ffmpeg_command(const StreamConfig& c);
// 실제 송출 주소. srt면 url(rtsp://호스트:포트/경로)에서 srt://호스트:srt_port?streamid=publish:경로...
std::string target_url(const StreamConfig& c);

}  // namespace video
