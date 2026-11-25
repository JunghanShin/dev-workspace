// 스트림 한 줄(include/worker/stream.hpp)
#include "worker/stream.hpp"

#include <chrono>

#include "worker/host.hpp"

namespace worker {

std::string stream_line(Host* host, uint64_t seq, bool kf, Json topics, Json events) {
    if (!events.is_array()) events = Json::array();
    if (host)
        for (auto& e : host->take_events()) events.push_back(std::move(e));
    if (!topics.is_object()) topics = Json::object();
    const double t = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    return Json{{"seq", seq}, {"kf", kf}, {"t", t}, {"topics", std::move(topics)}, {"events", std::move(events)}, {"events_next", 0}}
        .dump();
}

}  // namespace worker
