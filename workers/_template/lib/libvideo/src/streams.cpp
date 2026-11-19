#include <cstdio>
#include <set>

#include "video/stream.hpp"
#include "worker/config.hpp"

namespace video {

using worker::read;
using worker::require;

std::vector<StreamConfig> parse_streams(const nlohmann::json& streams, const StreamConfig& defaults) {
    std::vector<StreamConfig> out;
    if (streams.is_null()) return out;
    require(streams.is_array(), "config.streams must be an array of objects");
    std::set<std::string> names;
    for (const auto& s : streams) {
        require(s.is_object(), "config.streams entries must be objects");
        worker::check_keys(s, {"name", "autostart", "input", "source", "source_codec", "fps", "mode", "bitrate_kbps",
                               "keyframe_sec", "intra_refresh_sec", "overlay_time", "url", "transport", "pkt_size",
                               "srt_port", "srt_latency_ms", "retry_sec", "stall_sec", "ffmpeg", "gst_launch"});
        StreamConfig c = defaults;
        read(s, "name", c.name);
        read(s, "autostart", c.autostart);
        read(s, "input", c.input);
        read(s, "source", c.source);
        read(s, "source_codec", c.source_codec);
        read(s, "fps", c.fps);
        read(s, "mode", c.mode);
        read(s, "bitrate_kbps", c.bitrate_kbps);
        read(s, "keyframe_sec", c.keyframe_sec);
        read(s, "intra_refresh_sec", c.intra_refresh_sec);
        read(s, "overlay_time", c.overlay_time);
        read(s, "url", c.url);
        read(s, "transport", c.transport);
        read(s, "pkt_size", c.pkt_size);
        read(s, "srt_port", c.srt_port);
        read(s, "srt_latency_ms", c.srt_latency_ms);
        read(s, "retry_sec", c.retry_sec);
        read(s, "stall_sec", c.stall_sec);
        read(s, "ffmpeg", c.ffmpeg);
        read(s, "gst_launch", c.gst_launch);

        const std::string p = "config.streams[" + c.name + "].";
        require(!c.name.empty() && c.name.find_first_of(" =\t\n") == std::string::npos, "config.streams: bad name");
        require(names.insert(c.name).second, "config.streams: duplicate name " + c.name);
        require(c.input == "rtsp" || c.input == "framebus", p + "input must be rtsp or framebus");
        require(c.input != "framebus" || c.encode(), p + "input framebus needs mode encode (raw frames)");
        require(!c.source.empty(), p + "source is required");
        require(c.source_codec == "h264" || c.source_codec == "h265", p + "source_codec must be h264 or h265");
        require(c.fps >= 1 && c.fps <= 120, p + "fps must be 1..120");
        require(c.mode == "relay" || c.mode == "encode", p + "mode must be relay or encode");
        require(c.encode() || !c.overlay_time, p + "overlay_time needs mode encode");
        require(c.bitrate_kbps >= 100 && c.bitrate_kbps <= 50000, p + "bitrate_kbps must be 100..50000");
        require(c.keyframe_sec >= 0.2 && c.keyframe_sec <= 10, p + "keyframe_sec must be 0.2..10");
        require(c.intra_refresh_sec >= 0 && c.intra_refresh_sec <= c.keyframe_sec,
                p + "intra_refresh_sec must be 0..keyframe_sec");
        require(!c.url.empty(), p + "url is required");
        require(c.transport == "rtsp_udp" || c.transport == "rtsp_tcp" || c.transport == "srt",
                p + "transport must be rtsp_udp, rtsp_tcp or srt");
        require(c.pkt_size >= 500 && c.pkt_size <= 1472, p + "pkt_size must be 500..1472");
        require(c.srt_port > 0 && c.srt_port < 65536, p + "srt_port out of range");
        require(c.srt_latency_ms >= 20 && c.srt_latency_ms <= 5000, p + "srt_latency_ms must be 20..5000");
        require(c.retry_sec >= 0.5, p + "retry_sec must be >= 0.5");
        require(c.stall_sec >= 1, p + "stall_sec must be >= 1");
        out.push_back(c);
    }
    return out;
}

void Streams::configure(const std::vector<StreamConfig>& configs) {
    streams_.clear();
    for (const auto& c : configs) streams_.push_back(std::make_unique<Stream>(c));
}

void Streams::add_pollfds(std::vector<pollfd>& fds) const {
    for (const auto& s : streams_) {
        if (s->fd() >= 0) fds.push_back({s->fd(), POLLIN, 0});
    }
}

void Streams::on_poll(const std::vector<pollfd>& fds, Clock::time_point now) {
    for (const auto& p : fds) {
        if (!p.revents) continue;
        for (const auto& s : streams_) {
            if (s->fd() == p.fd) s->on_readable(now);
        }
    }
}

void Streams::on_timer(Clock::time_point now) {
    for (const auto& s : streams_) s->on_timer(now);
}

Streams::Clock::time_point Streams::next_wakeup(Clock::time_point now) const {
    auto t = now + std::chrono::seconds(1);
    for (const auto& s : streams_) t = std::min(t, s->next_wakeup(now));
    return t;
}

void Streams::stop() {
    for (const auto& s : streams_) s->stop();
}

nlohmann::json Streams::status() const {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& s : streams_) a.push_back(s->status());
    return a;
}

std::string Streams::metrics() const {
    std::string m;
    for (const auto& s : streams_) {
        char fps[32];
        std::snprintf(fps, sizeof fps, "%.1f", s->fps());
        const std::string n = s->config().name;
        // 꺼 둔 송출은 _link를 내지 않는다(연결 지표는 "붙어 있어야 하는데 끊김"만 뜻하게: system-check가 끊김으로 경고하지 않음)
        if (s->enabled()) m += (m.empty() ? "" : " ") + std::string("stream_") + n + "_link=" + (s->linked() ? "1" : "0");
        m += (m.empty() ? "" : " ") + std::string("stream_") + n + "_fps=" + fps + " stream_" + n + "_restarts=" +
             std::to_string(s->restarts());
    }
    return m;
}

bool Streams::handle(const std::string& cmd, const nlohmann::json& args, nlohmann::json* reply) {
    if (cmd != "stream_start" && cmd != "stream_stop") return false;
    const std::string name = args.is_object() && args.contains("name") && args["name"].is_string()
                                 ? args["name"].get<std::string>()
                                 : "";
    for (const auto& s : streams_) {
        if (s->config().name != name) continue;
        s->set_enabled(cmd == "stream_start");
        std::printf("stream %s: %s by command\n", name.c_str(), cmd == "stream_start" ? "started" : "stopped");
        *reply = {{"ok", true}, {"data", s->status()}};
        return true;
    }
    *reply = {{"ok", false}, {"error", {{"code", "bad_request"}, {"message", "unknown stream: " + name}}}};
    return true;
}

}  // namespace video
