#include "source/generator.hpp"

#include <cmath>

namespace source {

Generator::Generator(GeneratorConfig cfg) { set_config(cfg); }

void Generator::set_config(GeneratorConfig cfg) {
    if (!(cfg.rate_hz >= 0.1 && cfg.rate_hz <= 1000)) throw ConfigError("rate_hz must be 0.1..1000");
    if (cfg.size < 1 || cfg.size > 4096) throw ConfigError("size must be 1..4096");
    if (!(cfg.amplitude >= 0 && cfg.amplitude <= 1e6)) throw ConfigError("amplitude must be 0..1e6");
    const bool rate_changed = cfg.rate_hz != cfg_.rate_hz;
    cfg_ = cfg;
    buf_.assign(static_cast<size_t>(cfg_.size), 0.0f);
    if (rate_changed) next_ms_ = -1;  // 새 주기로 다시 기준을 잡는다
}

bool Generator::due(int64_t now_ms) {
    const auto period = static_cast<int64_t>(1000.0 / cfg_.rate_hz);
    if (next_ms_ < 0) {
        next_ms_ = now_ms + period;
        return false;
    }
    if (now_ms < next_ms_) return false;
    next_ms_ += period;
    if (next_ms_ <= now_ms) next_ms_ = now_ms + period;  // 많이 밀렸으면 한 번만 만들고 다시 맞춘다
    return true;
}

const std::vector<float>& Generator::make() {
    ++seq_;
    const double base = static_cast<double>(seq_) * 0.1;
    for (size_t i = 0; i < buf_.size(); ++i)
        buf_[i] = static_cast<float>(cfg_.amplitude * std::sin(base + static_cast<double>(i) * 0.05) + seq_ * 0.001);
    return buf_;
}

}  // namespace source
