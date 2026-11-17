#include "video/module.hpp"

#include <sstream>

namespace video {

void StreamsModule::configure(const worker::Json& c) {
    worker::check_keys(c, {"streams"});
    const StreamConfig defaults = defaults_ ? defaults_() : StreamConfig{};
    try {
        streams_.configure(parse_streams(c.contains("streams") ? c["streams"] : worker::Json(), defaults));
    } catch (const worker::ConfigError& e) {
        throw worker::ConfigError(std::string("config.stream: ") + e.what());
    } catch (const std::invalid_argument& e) {
        throw worker::ConfigError(std::string("config.stream: ") + e.what());
    }
}

bool StreamsModule::handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) {
    if (cmd != "start" && cmd != "stop") return false;
    worker::Json r;
    streams_.handle(cmd == "start" ? "stream_start" : "stream_stop", args, &r);
    reply(r);
    return true;
}

void StreamsModule::metrics(worker::Metrics& m) const {
    // Streams가 만든 "stream_<이름>_link=1 ..."(이미 모듈 접두어 "stream_"이 붙어 있음)
    std::istringstream in(streams_.metrics());
    std::string kv;
    while (in >> kv) {
        const auto eq = kv.find('=');
        if (eq != std::string::npos) m.set_text(kv.substr(0, eq), kv.substr(eq + 1));
    }
}

}  // namespace video
