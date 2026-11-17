#include "video/pipeline.hpp"

#include <algorithm>
#include <cctype>

namespace video {

namespace {

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

void append(std::vector<std::string>& out, const std::vector<std::string>& more) {
    out.insert(out.end(), more.begin(), more.end());
}

int frames(const StreamConfig& c, double sec) { return static_cast<int>(sec * c.fps + 0.5); }

}  // namespace

// GStreamer raw 형식 이름: GRAY8 → gray8, GRAY16_LE → gray16-le
static std::string gst_format(const std::string& pixel) {
    std::string s;
    for (char ch : pixel) s += ch == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::vector<std::string> gst_input_stage(const StreamConfig& c, const framebus::Format* raw) {
    if (c.input == "framebus") {
        // 프레임 버스 → (Feeder) → 표준 입력. 한 덩어리 = 한 프레임. nvvidconv가 GPU 메모리(NVMM) I420으로 올린다
        // (GRAY8 → NVMM NV12는 보드에서 협상이 안 됨, I420은 됨: 실측 2026-11-12)
        const std::string w = raw ? std::to_string(raw->width) : "<width>";
        const std::string h = raw ? std::to_string(raw->height) : "<height>";
        const std::string bytes = raw ? std::to_string(raw->frame_bytes()) : "<frame bytes>";
        return {"fdsrc", "fd=0", "blocksize=" + bytes, "!", "rawvideoparse", "width=" + w, "height=" + h,
                "format=" + gst_format(raw ? raw->pixel : "GRAY8"), "framerate=" + std::to_string(c.fps) + "/1", "!",
                "nvvidconv", "!", "video/x-raw(memory:NVMM),format=I420", "!"};
    }
    const bool h265 = c.source_codec == "h265";
    // 카메라 → 보드는 같은 내부망이라 TCP(손실 없음). latency=0: 지터 버퍼를 두지 않는다
    return {"rtspsrc", "location=" + c.source, "protocols=tcp", "latency=0", "!",
            h265 ? "rtph265depay" : "rtph264depay", "!",
            std::string(h265 ? "video/x-h265" : "video/x-h264") + ",stream-format=byte-stream,alignment=au", "!"};
}

std::vector<std::string> gst_encode_stage(const StreamConfig& c) {
    // rtsp는 압축 영상이라 NVDEC로 푼다. framebus는 입력 단계가 이미 NVMM 원본 프레임
    std::vector<std::string> s;
    if (c.input != "framebus") s = {"nvv4l2decoder", "!"};
    if (c.overlay_time) {
        // 시각은 보드 시계(NTP). 글자는 CPU 메모리에서 그리므로 그 구간만 RGBA로 내렸다 올린다
        append(s, {"nvvidconv", "!", "video/x-raw,format=RGBA", "!", "clockoverlay", "time-format=%H:%M:%S",
                   "halignment=left", "valignment=bottom", "font-desc=Sans 20", "shaded-background=true", "!",
                   "nvvidconv", "!", "video/x-raw(memory:NVMM),format=NV12", "!"});
    }
    // 저지연: 최고 클럭(maxperf), 프레임을 쌓지 않는 순서(poc-type=2), 브라우저 WebRTC가 받는 Constrained
    // Baseline. CBR·UltraFast 프리셋은 기본값. vbv-size는 건드리지 않는다: 줄이면 인코더 지연이 약 0.1 s →
    // 0.9 s로 늘어난다(보드 실측, 2026-11-06). 키프레임 keyframe_sec, 인트라 리프레시 intra_refresh_sec에 한 바퀴
    const std::string gop = std::to_string(std::max(1, frames(c, c.keyframe_sec)));
    append(s, {"nvv4l2h264enc", "bitrate=" + std::to_string(c.bitrate_kbps * 1000), "maxperf-enable=true",
               "poc-type=2", "profile=1", "insert-sps-pps=true", "idrinterval=" + gop, "iframeinterval=" + gop});
    const int refresh = frames(c, c.intra_refresh_sec);
    if (refresh > 0) s.push_back("SliceIntraRefreshInterval=" + std::to_string(refresh));
    append(s, {"!", "video/x-h264,stream-format=byte-stream", "!"});
    return s;
}

std::vector<std::string> gst_command(const StreamConfig& c, const framebus::Format* raw) {
    std::vector<std::string> cmd = {c.gst_launch, "-q"};
    append(cmd, gst_input_stage(c, raw));
    append(cmd, gst_encode_stage(c));
    append(cmd, {"fdsink", "fd=1"});
    return cmd;
}

std::vector<std::string> ffmpeg_output_stage(const StreamConfig& c) {
    const std::string target = target_url(c);
    std::vector<std::string> s;
    if (starts_with(target, "rtmp")) {
        s = {"-f", "flv"};
    } else if (starts_with(target, "rtsp")) {
        if (c.transport == "rtsp_udp") {
            s = {"-f", "rtsp", "-rtsp_transport", "udp", "-pkt_size", std::to_string(c.pkt_size)};
        } else {
            s = {"-f", "rtsp", "-rtsp_transport", "tcp"};
        }
    } else if (starts_with(target, "srt://") || starts_with(target, "udp://")) {
        // MPEG-TS는 패킷을 모아 두지 않고 바로 내보낸다
        s = {"-f", "mpegts", "-flush_packets", "1", "-muxdelay", "0", "-muxpreload", "0"};
    }
    s.push_back(target);
    return s;
}

std::vector<std::string> ffmpeg_command(const StreamConfig& c) {
    std::vector<std::string> cmd = {c.ffmpeg, "-hide_banner", "-loglevel", "warning", "-nostdin", "-nostats"};
    if (c.encode()) {
        // gst-launch가 파이프로 주는 H.264(타임스탬프 없음)에 입력 fps로 시각을 붙인다. 받은 시각(wallclock)으로
        // 붙이면 파이프 읽기가 몰릴 때 DTS가 거꾸로 가 경고가 쏟아진다(실측). nobuffer·작은 probesize: 시작할 때
        // 분석하느라 모아 두지 않는다(SPS·PPS는 키프레임마다 들어 있음)
        append(cmd, {"-fflags", "nobuffer", "-probesize", "65536", "-analyzeduration", "0", "-f", "h264",
                     "-framerate", std::to_string(c.fps), "-i", "pipe:0"});
    } else {
        cmd.insert(cmd.end(), {"-fflags", "nobuffer"});
        if (starts_with(c.source, "rtsp")) append(cmd, {"-rtsp_transport", "tcp"});
        append(cmd, {"-i", c.source});
    }
    append(cmd, {"-map", "0:v", "-c", "copy", "-progress", "pipe:3"});
    append(cmd, ffmpeg_output_stage(c));
    return cmd;
}

// MediaMTX SRT 형식. SRT 패킷은 MPEG-TS(188바이트) 배수여야 한다. latency·pkt_size 단위는 ffmpeg 기준(µs, 바이트)
std::string target_url(const StreamConfig& c) {
    if (c.transport != "srt" || !starts_with(c.url, "rtsp://")) return c.url;
    const std::string rest = c.url.substr(7);
    const auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? "" : rest.substr(slash + 1);
    const auto at = hostport.rfind('@');
    if (at != std::string::npos) hostport = hostport.substr(at + 1);
    const auto colon = hostport.rfind(':');
    const std::string host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
    const int ts = (c.pkt_size / 188) * 188;
    return "srt://" + host + ":" + std::to_string(c.srt_port) + "?streamid=publish:" + path +
           "&latency=" + std::to_string(c.srt_latency_ms * 1000) + "&pkt_size=" + std::to_string(ts);
}

}  // namespace video
