#include "processor/module.hpp"

namespace processor {

using worker::Json;

namespace {

int parse_window(const Json& c) {
    worker::check_keys(c, {"window"});
    int w = 50;
    worker::read(c, "window", w);
    try {
        Window probe(w);
        (void)probe;
    } catch (const ConfigError& e) {
        throw worker::ConfigError(std::string("config.processor: ") + e.what());
    }
    return w;
}

}  // namespace

void Module::configure(const Json& c) { win_.set_size(parse_window(c)); }

bool Module::reconfigure(const Json& c) {
    win_.set_size(parse_window(c));  // 창 크기는 다시 시작 없이 바꿀 수 있다
    return true;
}

void Module::start(worker::Host&) { attach(); }

void Module::attach() {
    // 포트 구독은 start에서(설정이 끝난 뒤). 콜백은 워커 루프 안에서 불린다: 짧게 끝낸다
    src_.subscribe([this](const source::Sample& s) { on_sample(s); });
}

void Module::on_sample(const source::Sample& s) {
    double sum = 0;
    for (float v : *s.values) sum += v;
    win_.add(s.values->empty() ? 0.0 : sum / static_cast<double>(s.values->size()));
    last_seq_ = s.seq;
}

bool Module::handle(const std::string& cmd, const Json&, worker::Reply reply) {
    if (cmd == "reset") {
        win_.reset();
        reply(worker::ok_reply({{"count", 0}}));
        return true;
    }
    return false;
}

Json Module::status() const {
    return {{"window", win_.size()}, {"count", win_.count()}, {"total", win_.total()}, {"last_seq", last_seq_},
            {"avg", win_.avg()}, {"min", win_.min()}, {"max", win_.max()}};
}

void Module::metrics(worker::Metrics& m) const {
    m.set("avg", win_.avg(), 3);
    m.set("min", win_.min(), 3);
    m.set("max", win_.max(), 3);
    m.set("total", win_.total());
}

}  // namespace processor
