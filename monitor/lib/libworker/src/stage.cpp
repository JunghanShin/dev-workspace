#include "worker/stage.hpp"

#include "worker/config.hpp"

namespace worker {

const char* overflow_name(Overflow o) {
    switch (o) {
        case Overflow::DropOldest: return "drop_oldest";
        case Overflow::Reject: return "reject";
        case Overflow::DropNewest: return "drop_newest";
    }
    return "?";
}

StageOptions parse_stage_options(const Json& j, const std::string& name, StageOptions base) {
    const std::string at = "config.stages." + name;
    require(j.is_object(), at + " must be an object");
    check_keys(j, {"queue", "overflow", "wait_ms", "priority", "rt_priority", "nice", "cpu"});
    int queue = static_cast<int>(base.queue);
    read(j, "queue", queue);
    require(queue >= 1 && queue <= 1000000, at + ".queue must be 1..1000000");
    base.queue = static_cast<size_t>(queue);
    if (j.contains("overflow")) {
        require(j["overflow"].is_string(), at + ".overflow must be a string");
        const std::string o = j["overflow"].get<std::string>();
        if (o == "drop_oldest") base.overflow = Overflow::DropOldest;
        else if (o == "reject") base.overflow = Overflow::Reject;
        else if (o == "drop_newest") base.overflow = Overflow::DropNewest;
        else throw ConfigError(at + ".overflow must be drop_oldest, reject or drop_newest");
    }
    read(j, "wait_ms", base.wait_ms);
    require(base.wait_ms >= 1 && base.wait_ms <= 10000, at + ".wait_ms must be 1..10000");
    read(j, "priority", base.thread.priority);
    require(base.thread.priority == "normal" || base.thread.priority == "realtime",
            at + ".priority must be normal or realtime");
    read(j, "rt_priority", base.thread.rt_priority);
    require(base.thread.rt_priority >= 1 && base.thread.rt_priority <= 99, at + ".rt_priority must be 1..99");
    read(j, "nice", base.thread.nice);
    require(base.thread.nice >= -20 && base.thread.nice <= 19, at + ".nice must be -20..19");
    if (j.contains("cpu")) {
        require(j["cpu"].is_array(), at + ".cpu must be a list of CPU numbers");
        base.thread.cpus.clear();
        for (const auto& c : j["cpu"]) {
            require(c.is_number_integer() && c.get<int>() >= 0 && c.get<int>() < 1024, at + ".cpu items must be CPU numbers");
            base.thread.cpus.push_back(c.get<int>());
        }
    }
    return base;
}

}  // namespace worker
