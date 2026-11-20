#include "replay/module.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "worker/config.hpp"
#include "worker/workspace.hpp"

namespace replay {

using worker::Json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

Config Module::parse(const Json& c, const std::string& worker_id) {
    worker::check_keys(c, {"dir", "max_total_mb", "max_record_sec", "bus_slots", "bus_socket"});
    Config cfg;
    worker::read(c, "dir", cfg.dir);
    worker::read(c, "max_total_mb", cfg.max_total_mb);
    worker::read(c, "max_record_sec", cfg.max_record_sec);
    worker::read(c, "bus_slots", cfg.bus_slots);
    worker::read(c, "bus_socket", cfg.bus_socket);
    worker::require(cfg.max_total_mb >= 1, "config.replay.max_total_mb must be >= 1");
    worker::require(cfg.max_record_sec >= 1, "config.replay.max_record_sec must be >= 1");
    worker::require(cfg.bus_slots >= 2 && cfg.bus_slots <= 64, "config.replay.bus_slots must be 2..64");
    if (cfg.dir.empty()) {
        const std::string data = worker::workspace_data_dir();
        if (!data.empty()) {
            cfg.dir = data + "/records/framebus";
        } else {
            const char* x = std::getenv("XDG_RUNTIME_DIR");
            cfg.dir = std::string(x && *x ? x : "/tmp") + "/" + worker_id + "-records";
        }
    }
    if (cfg.bus_socket.empty()) cfg.bus_socket = worker::default_socket(worker_id, "frames");
    return cfg;
}

void Module::configure(const Json& c) { cfg_ = parse(c, host() ? host()->id() : "replay"); }

void Module::start(worker::Host& host) {
    rec_thread_ = std::make_unique<worker::WorkerThread>("replay.rec", host.watch(),
                                                         [this](worker::WorkerThread& t) { rec_body(t); });
    play_thread_ = std::make_unique<worker::WorkerThread>("replay.play", host.watch(),
                                                          [this](worker::WorkerThread& t) { play_body(t); });
    rec_thread_->start();
    play_thread_->start();
}

// ---------------- 녹화 ----------------

void Module::rec_body(worker::WorkerThread& t) {
    while (!t.stopping()) {
        t.beat();
        std::unique_ptr<RecJob> job;
        {
            std::lock_guard<std::mutex> lock(mu_);
            job = std::move(rec_job_);
        }
        if (!job) {
            std::this_thread::sleep_for(50ms);
            continue;
        }
        if (!rec_cancel_.load()) run_record(t, *job);
        rec_link_ = false;
        rec_active_ = false;
    }
}

void Module::run_record(worker::WorkerThread& t, const RecJob& job) {
    framebus::Consumer c(job.socket, framebus::Consumer::Mode::InOrder);
    Writer w;
    std::string err;
    const auto end = Clock::now() + std::chrono::duration<double>(job.seconds);
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> lock(mu_);
        rec_error_ = e;
    };
    while (!t.stopping() && !rec_cancel_.load() && Clock::now() < end) {
        t.beat();
        if (!c.connected()) {
            rec_link_ = false;
            if (!c.connect(&err)) {  // 생산자가 아직 없으면 기다리며 다시 붙음
                std::this_thread::sleep_for(200ms);
                continue;
            }
            rec_link_ = true;
            if (!w.is_open()) {
                Meta m;
                const auto& f = c.format();
                m.width = f.width;
                m.height = f.height;
                m.stride = f.stride;
                m.slot_bytes = f.frame_bytes();
                m.pixel = f.pixel;
                m.source = job.label;
                if (!w.open(job.dir, job.name, m, &err)) {
                    fail(err);
                    break;
                }
            }
        }
        framebus::Frame fr;
        if (c.next(100, &fr) != framebus::Consumer::Result::Frame) continue;
        const bool ok = w.write(fr.frame_id, fr.ts_mono_ns, fr.data, fr.bytes);
        const size_t bytes = fr.bytes;
        c.release(fr);
        if (!ok) {
            fail("write failed (disk full?)");
            break;
        }
        ++rec_frames_;
        rec_bytes_ += bytes;
        if (static_cast<double>(job.already + rec_bytes_.load()) >= job.max_bytes) {
            fail("recording folder is full (max_total_mb): stopped");
            break;
        }
    }
    if (w.is_open() && !w.close(&err)) fail(err);
}

Json Module::record_start(const Json& args) {
    if (rec_active_.load()) return worker::error_reply("busy", "already recording " + rec_name_);
    const std::string name = args.value("name", std::string());
    if (!valid_name(name)) return worker::error_reply("bad_request", "name: a-z A-Z 0-9 - _ (1~64)");
    std::string socket = args.value("socket", std::string());
    const std::string source = args.value("source", std::string());
    if (socket.empty()) {
        if (source.empty()) return worker::error_reply("bad_request", "source (worker id) or socket is required");
        socket = worker::default_socket(source, "frames");
    }
    double seconds = cfg_.max_record_sec;
    if (args.contains("seconds")) {
        if (!args["seconds"].is_number() || args["seconds"].get<double>() <= 0)
            return worker::error_reply("bad_request", "seconds must be > 0");
        seconds = std::min(args["seconds"].get<double>(), cfg_.max_record_sec);
    }
    if (!rec_thread_) return worker::error_reply("unavailable", "not started");
    const uint64_t already = total_bytes(cfg_.dir);
    if (static_cast<double>(already) / 1e6 >= cfg_.max_total_mb)
        return worker::error_reply("unavailable", "recording folder is full (max_total_mb)");
    for (const auto& i : list(cfg_.dir))
        if (i.name == name) return worker::error_reply("bad_request", "recording exists: " + name);

    auto job = std::make_unique<RecJob>();
    job->name = name;
    job->socket = socket;
    job->label = source.empty() ? socket : source;
    job->dir = cfg_.dir;
    job->seconds = seconds;
    job->max_bytes = cfg_.max_total_mb * 1e6;
    job->already = already;
    rec_frames_ = rec_bytes_ = 0;
    rec_link_ = false;
    rec_cancel_ = false;
    rec_active_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        rec_name_ = name;
        rec_source_ = job->label;
        rec_error_.clear();
        rec_job_ = std::move(job);
    }
    return worker::ok_reply({{"recording", name}, {"socket", socket}, {"seconds", seconds}});
}

void Module::stop_record() {
    rec_cancel_ = true;
    const auto end = Clock::now() + 1s;  // 녹화 루프는 0.2초 안에 취소를 본다
    while (rec_active_.load() && Clock::now() < end) std::this_thread::sleep_for(10ms);
}

// ---------------- 재생 ----------------

void Module::play_body(worker::WorkerThread& t) {
    while (!t.stopping()) {
        t.beat();
        std::unique_ptr<PlayJob> job;
        {
            std::lock_guard<std::mutex> lock(mu_);
            job = std::move(play_job_);
        }
        if (!job) {
            std::this_thread::sleep_for(50ms);
            continue;
        }
        if (!play_cancel_.load()) run_play(t, *job);
        job.reset();  // 데이터 버스를 닫는다(받는 쪽은 끊김을 본다)
        play_consumers_ = 0;
        play_active_ = false;
    }
}

void Module::run_play(worker::WorkerThread& t, PlayJob& job) {
    Record rec;
    framebus::Producer& bus = *job.bus;
    auto stopping = [&] { return t.stopping() || play_cancel_.load(); };
    for (;;) {
        bool first = true;
        int64_t ts0 = 0;
        Clock::time_point wall0;
        while (!stopping() && job.reader->next(&rec)) {
            if (first) {
                ts0 = rec.ts_ns;
                wall0 = Clock::now();
                first = false;
            }
            // 원래 간격(배속)대로. 기다리는 동안도 beat(긴 간격이어도 멈춤으로 보이지 않게)
            const auto due = wall0 + std::chrono::nanoseconds(static_cast<int64_t>((rec.ts_ns - ts0) / job.speed));
            while (!stopping() && Clock::now() < due) {
                t.beat();
                std::this_thread::sleep_until(std::min(due, Clock::now() + 50ms));
            }
            if (stopping()) break;
            // 빈 슬롯이 없으면(받는 쪽이 느림) 잠깐 기다렸다가, 그래도 없으면 그 프레임은 버린다(생산자 규칙)
            std::optional<framebus::Producer::Slot> slot;
            for (int i = 0; i < 20 && !(slot = bus.acquire()) && !stopping(); ++i) std::this_thread::sleep_for(2ms);
            if (slot) {
                const size_t n = std::min(slot->bytes, rec.data.size());
                std::memcpy(slot->data, rec.data.data(), n);
                bus.publish(*slot, rec.frame_id,
                            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(),
                            n);
                ++play_frames_;
            }
            play_consumers_ = bus.stats().consumers;
            t.beat();
        }
        if (stopping() || !job.loop) break;
        job.reader->rewind();
        ++play_loops_;
    }
    // 받는 쪽이 마지막 프레임까지 가져가도록 잠깐(최대 1초) 기다린 뒤 닫는다: 슬롯을 전부 다시 잡을 수 있으면 다 반납된 것
    std::vector<framebus::Producer::Slot> held;
    const auto end = Clock::now() + 1s;
    while (!stopping() && static_cast<int>(held.size()) < job.slots && Clock::now() < end) {
        t.beat();
        if (auto s = bus.acquire()) held.push_back(*s);
        else std::this_thread::sleep_for(10ms);
    }
}

Json Module::play_start(const Json& args) {
    if (play_active_.load()) return worker::error_reply("busy", "already playing " + play_name_);
    const std::string name = args.value("name", std::string());
    const double speed = args.contains("speed") && args["speed"].is_number() ? args["speed"].get<double>() : 1.0;
    const bool loop = args.value("loop", false);
    if (!(speed > 0 && speed <= 100)) return worker::error_reply("bad_request", "speed must be 0 < speed <= 100");
    auto job = std::make_unique<PlayJob>();
    job->reader = std::make_shared<Reader>();
    std::string err;
    if (!job->reader->open(cfg_.dir, name, &err)) return worker::error_reply("bad_request", err);
    if (!play_thread_) return worker::error_reply("unavailable", "not started");
    const Meta m = job->reader->meta();
    framebus::Format f;
    f.width = m.width;
    f.height = m.height;
    f.stride = m.stride;
    f.pixel = m.pixel;
    if (f.frame_bytes() == 0 || m.slot_bytes > f.frame_bytes())
        f = framebus::Format::raw(m.slot_bytes ? m.slot_bytes : 1, m.pixel);
    job->bus = std::make_unique<framebus::Producer>(cfg_.bus_socket, f, cfg_.bus_slots);
    if (!job->bus->start(&err)) return worker::error_reply("unavailable", "data bus: " + err);
    job->speed = speed;
    job->loop = loop;
    job->slots = cfg_.bus_slots;
    play_speed_ = speed;
    play_loop_ = loop;
    play_total_ = m.frames;
    play_frames_ = play_loops_ = 0;
    play_cancel_ = false;
    play_active_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        play_name_ = name;
        play_error_.clear();
        play_job_ = std::move(job);
    }
    return worker::ok_reply({{"playing", name}, {"frames", m.frames}, {"speed", speed}, {"loop", loop},
                             {"socket", cfg_.bus_socket}});
}

void Module::stop_play() {
    play_cancel_ = true;
    const auto end = Clock::now() + 1s;
    while (play_active_.load() && Clock::now() < end) std::this_thread::sleep_for(10ms);
}

// ---------------- 명령·상태 ----------------

bool Module::handle(const std::string& cmd, const Json& args, worker::Reply reply) {
    if (cmd == "record_start") reply(record_start(args));
    else if (cmd == "record_stop") {
        const bool was = rec_active_.load();
        stop_record();
        reply(worker::ok_reply({{"stopped", was}, {"frames", rec_frames_.load()}}));
    } else if (cmd == "play_start") reply(play_start(args));
    else if (cmd == "play_stop") {
        const bool was = play_active_.load();
        stop_play();
        reply(worker::ok_reply({{"stopped", was}}));
    } else if (cmd == "list") {
        Json items = Json::array();
        for (const auto& i : list(cfg_.dir))
            items.push_back({{"name", i.name}, {"source", i.meta.source}, {"created", i.meta.created},
                             {"frames", i.meta.frames}, {"seconds", i.meta.duration_ns / 1e9}, {"pixel", i.meta.pixel},
                             {"width", i.meta.width}, {"height", i.meta.height}, {"mb", i.file_bytes / 1e6}});
        reply(worker::ok_reply({{"recordings", items}, {"total_mb", total_bytes(cfg_.dir) / 1e6},
                                {"max_total_mb", cfg_.max_total_mb}, {"dir", cfg_.dir}}));
    } else if (cmd == "delete") {
        const std::string name = args.value("name", std::string());
        bool busy;
        {
            std::lock_guard<std::mutex> lock(mu_);
            busy = (rec_active_.load() && name == rec_name_) || (play_active_.load() && name == play_name_);
        }
        if (busy) {
            reply(worker::error_reply("busy", "in use: " + name));
        } else {
            std::string err;
            if (remove(cfg_.dir, name, &err)) reply(worker::ok_reply({{"deleted", name}}));
            else reply(worker::error_reply("bad_request", err));
        }
    } else {
        return false;
    }
    return true;
}

Json Module::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    Json s = {{"dir", cfg_.dir}, {"bus_socket", cfg_.bus_socket}};
    s["recording"] = {{"active", rec_active_.load()}, {"name", rec_name_}, {"source", rec_source_},
                      {"link", rec_link_.load()}, {"frames", rec_frames_.load()}, {"mb", rec_bytes_.load() / 1e6},
                      {"error", rec_error_}};
    s["playing"] = {{"active", play_active_.load()}, {"name", play_name_}, {"frames", play_frames_.load()},
                    {"total", play_total_}, {"loops", play_loops_.load()}, {"speed", play_speed_}, {"loop", play_loop_},
                    {"consumers", play_consumers_.load()}, {"error", play_error_}};
    return s;
}

void Module::metrics(worker::Metrics& m) const {
    m.set("recording", rec_active_.load());
    m.set("rec_frames", rec_frames_.load());
    m.set("playing", play_active_.load());
    m.set("play_frames", play_frames_.load());
}

bool Module::failed(std::string* why) const {
    for (const worker::WorkerThread* t : {rec_thread_.get(), play_thread_.get()}) {
        if (t && t->failed()) {
            *why = t->name() + ": " + t->error();
            return true;
        }
    }
    return false;
}

void Module::stop() {
    rec_cancel_ = true;
    play_cancel_ = true;
    for (auto* t : {rec_thread_.get(), play_thread_.get()})
        if (t) t->request_stop();
    for (auto* t : {rec_thread_.get(), play_thread_.get()})
        if (t) t->join();
}

}  // namespace replay
