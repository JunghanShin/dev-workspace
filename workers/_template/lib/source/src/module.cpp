#include "source/module.hpp"

#include <sys/wait.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "worker/child.hpp"
#include "worker/workspace.hpp"

namespace source {

using worker::Json;
using Clock = std::chrono::steady_clock;

namespace {

int64_t ms(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

std::vector<std::string> split(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

}  // namespace

Config Module::parse(const Json& c, const std::string& worker_id) {
    worker::check_keys(c, {"rate_hz", "size", "amplitude", "bus_slots", "bus_socket", "helper_cmd"});
    Config cfg;
    worker::read(c, "rate_hz", cfg.gen.rate_hz);
    worker::read(c, "size", cfg.gen.size);
    worker::read(c, "amplitude", cfg.gen.amplitude);
    worker::read(c, "bus_slots", cfg.bus_slots);
    worker::read(c, "bus_socket", cfg.bus_socket);
    worker::read(c, "helper_cmd", cfg.helper_cmd);
    worker::require(cfg.bus_slots >= 2 && cfg.bus_slots <= 64, "config.source.bus_slots must be 2..64");
    try {
        Generator probe(cfg.gen);  // 코어가 값을 검사한다
        (void)probe;
    } catch (const ConfigError& e) {
        throw worker::ConfigError(std::string("config.source: ") + e.what());  // 코어 오류 → 설정 오류(exit 1)
    }
    if (cfg.bus_socket.empty()) cfg.bus_socket = worker::default_socket(worker_id, "frames");
    return cfg;
}

void Module::configure(const Json& c) {
    cfg_ = parse(c, host() ? host()->id() : "source");
    std::lock_guard<worker::Mutex> lock(gen_mu_);
    gen_.set_config(cfg_.gen);
}

// 바로 적용(SIGHUP): 생성 주기·크기 값은 바로. 슬롯 수·소켓·자식 프로세스는 다시 시작해야 하므로 false
bool Module::reconfigure(const Json& c) {
    const Config next = parse(c, host() ? host()->id() : "source");
    if (next.gen.size != cfg_.gen.size || next.bus_slots != cfg_.bus_slots || next.bus_socket != cfg_.bus_socket ||
        next.helper_cmd != cfg_.helper_cmd)
        return false;
    cfg_ = next;
    std::lock_guard<worker::Mutex> lock(gen_mu_);
    gen_.set_config(cfg_.gen);
    return true;
}

void Module::start(worker::Host& host) {
    std::string error;
    // 데이터 버스: 다른 워커가 이 워커의 표본을 복사 없이 받는다(영상이 아닌 데이터 형식, design.md 6.7)
    const auto bytes = static_cast<uint32_t>(cfg_.gen.size * sizeof(float));
    bus_ = std::make_unique<framebus::Producer>(cfg_.bus_socket, framebus::Format::raw(bytes, "f32x" + std::to_string(cfg_.gen.size)),
                                                cfg_.bus_slots);
    if (!bus_->start(&error)) throw std::runtime_error("data bus: " + error);
    // 스트림(상태 발행): 화면과 다른 워커가 구독한다
    worker::Publisher::Options po;
    po.thread_name = "source.pub";
    pub_ = std::make_unique<worker::Publisher>(host.watch(), po);
    if (!pub_->open(host.stream_socket(), &error)) throw std::runtime_error("stream: " + error);

    // 입력 단계(제공 도구): 큐 64개, 넘치면 오래된 것 버림. 설정 "stages": {"source.gen": {...}}로 바꿀 수 있다
    worker::StageOptions so;
    so.queue = 64;
    host.input<Made>("source.gen", [this](worker::Input<Made>::Context& in) { make_once(in); }, [this](Made& m) { take(m); }, so);

    if (!cfg_.helper_cmd.empty()) {
        helper_ = worker::spawn(split(cfg_.helper_cmd));
        if (helper_ < 0) std::fprintf(stderr, "source: helper start failed: %s\n", cfg_.helper_cmd.c_str());
    }
    std::printf("source: %.1f Hz x %d values -> data bus %s\n", cfg_.gen.rate_hz, cfg_.gen.size, cfg_.bus_socket.c_str());
}

// source.gen 스레드: 때가 되면 표본 하나를 만들어 데이터 버스에 쓰고 워커 루프로 넘긴다. wait_ms 안에 돌아온다
void Module::make_once(worker::Input<Made>::Context& in) {
    const auto now = Clock::now();
    Made m;
    int64_t next_ms;
    bool due;
    {
        std::lock_guard<worker::Mutex> lock(gen_mu_);
        due = gen_.due(ms(now));
        if (due) {
            m.values = gen_.make();
            m.seq = gen_.seq();
            m.ts_ms = ms(now);
        }
        next_ms = gen_.next_due_ms();
    }
    if (due) {
        // 데이터 버스에 쓴다. 빈 슬롯이 없으면 그 표본은 버린다(생산자는 기다리지 않는다, dropped 지표)
        if (auto slot = bus_->acquire()) {
            const size_t n = std::min(slot->bytes, m.values.size() * sizeof(float));
            std::memcpy(slot->data, m.values.data(), n);
            bus_->publish(*slot, m.seq, m.ts_ms * 1000000, n);
        }
        in.push(std::move(m));
    }
    const int64_t wait = std::max<int64_t>(1, std::min<int64_t>(in.wait_ms(), next_ms - ms(Clock::now())));
    std::this_thread::sleep_for(std::chrono::milliseconds(wait));  // 시간 제한 있는 대기
}

// 워커 루프: 표본 하나 처리(호스트가 처리 시간 예산 안에서 부른다)
void Module::take(Made& m) {
    seq_ = m.seq;
    double sum = 0;
    for (float v : m.values) sum += v;
    last_mean_ = m.values.empty() ? 0.0f : static_cast<float>(sum / m.values.size());
    last_ts_ns_ = m.ts_ms * 1000000;
    const Sample s{m.seq, m.ts_ms, &m.values};
    for (const auto& fn : listeners_) fn(s);  // 워커 안 연결(포트)
    stream_dirty_ = true;
}

void Module::on_timer(worker::TimePoint now) {
    publish_stream(now);
    check_helper(now);
}

worker::TimePoint Module::next_wakeup(worker::TimePoint now) const { return now + std::chrono::milliseconds(200); }

// 주제 "source" 하나. 구독자가 있을 때만, 바뀌었거나 새 구독자가 붙었을 때(kf), 5초마다 kf (design.md 6.6.2, 9.7)
void Module::publish_stream(worker::TimePoint now) {
    if (!pub_ || !pub_->has_subscribers()) return;
    const bool kf = pub_->keyframe_requested() || now >= next_kf_;
    if (!kf && !stream_dirty_) return;
    if (kf) next_kf_ = now + std::chrono::seconds(5);
    stream_dirty_ = false;
    double rate;
    {
        std::lock_guard<worker::Mutex> lock(gen_mu_);
        rate = gen_.config().rate_hz;
    }
    const double t = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    pub_->publish(Json{{"seq", ++stream_seq_}, {"kf", kf}, {"t", t},
                       {"topics", {{"source", {{"t_mono_ns", last_ts_ns_}, {"seq", seq_}, {"mean", last_mean_}, {"rate_hz", rate}}}}},
                       {"events", Json::array()}, {"events_next", 0}}.dump());
}

// 자식 프로세스 감시: 죽으면 2초 뒤 다시 띄운다(워커는 계속)
void Module::check_helper(worker::TimePoint now) {
    if (cfg_.helper_cmd.empty()) return;
    if (helper_ > 0) {
        int st = 0;
        if (waitpid(helper_, &st, WNOHANG) == helper_) {
            std::fprintf(stderr, "source: %s\n", worker::exit_text("helper", st).c_str());
            helper_ = -1;
            helper_retry_ = now + std::chrono::seconds(2);
        }
    } else if (now >= helper_retry_) {
        helper_ = worker::spawn(split(cfg_.helper_cmd));
        ++helper_restarts_;
        helper_retry_ = now + std::chrono::seconds(2);
    }
}

bool Module::handle(const std::string& cmd, const Json& args, worker::Reply reply) {
    if (cmd == "set_rate") {
        if (!args.contains("hz") || !args["hz"].is_number()) {
            reply(worker::error_reply("bad_request", "args.hz must be a number"));
            return true;
        }
        std::lock_guard<worker::Mutex> lock(gen_mu_);
        GeneratorConfig g = gen_.config();
        g.rate_hz = args["hz"].get<double>();
        try {
            gen_.set_config(g);
        } catch (const ConfigError& e) {
            reply(worker::error_reply("bad_request", e.what()));
            return true;
        }
        cfg_.gen = g;
        reply(worker::ok_reply({{"rate_hz", g.rate_hz}}));  // 바로 적용, 설정 파일은 그대로
        return true;
    }
    if (cmd == "reset") {
        std::lock_guard<worker::Mutex> lock(gen_mu_);
        gen_.reset();
        reply(worker::ok_reply({{"seq", 0}}));
        return true;
    }
    return false;  // 모르는 명령(호스트가 bad_request)
}

Json Module::status() const {
    const auto bs = bus_ ? bus_->stats() : framebus::Stats{};
    return {{"seq", seq_},
            {"rate_hz", cfg_.gen.rate_hz},
            {"size", cfg_.gen.size},
            {"amplitude", cfg_.gen.amplitude},
            {"mean", last_mean_},
            {"bus", {{"socket", cfg_.bus_socket}, {"consumers", bs.consumers}, {"published", bs.published}, {"dropped", bs.dropped}}},
            {"helper", {{"cmd", cfg_.helper_cmd}, {"running", helper_ > 0}, {"restarts", helper_restarts_}}}};
}

void Module::metrics(worker::Metrics& m) const {
    const auto bs = bus_ ? bus_->stats() : framebus::Stats{};
    m.set("seq", seq_);
    m.set("rate_hz", cfg_.gen.rate_hz, 1);
    m.set("mean", last_mean_, 3);
    m.set("bus_consumers", static_cast<long long>(bs.consumers));
    m.set("drops", bs.dropped);  // 공통 이름: _drops
    m.set("link", true);         // 공통 이름: _link(실제 워커는 장치 연결 여부)
    if (!cfg_.helper_cmd.empty()) m.set("helper_up", helper_ > 0);
}

bool Module::failed(std::string* why) const {
    // source.gen 단계의 실패는 호스트가 본다
    if (pub_ && pub_->thread().failed()) {
        *why = pub_->thread().name() + ": " + pub_->thread().error();
        return true;
    }
    return false;
}

// 안전 상태(기본 안전망): 끝나기 전에 한 번. 실제 워커는 장치를 멈추는 명령 등. 예제는 기록만
void Module::safe_state() { std::printf("source: safe state (example: nothing to stop)\n"); }

void Module::stop() {
    // source.gen 단계는 호스트가 이 stop 전에 정지했다
    if (pub_) pub_->close();
    if (bus_) bus_->stop();
    if (helper_ > 0) worker::kill_wait(helper_);
}

}  // namespace source
