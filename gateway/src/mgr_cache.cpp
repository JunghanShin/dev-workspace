#include "mgr_cache.hpp"

#include <pthread.h>
#include <unistd.h>

#include <chrono>

#include "log.hpp"

namespace gw {

namespace {
constexpr double kSilentS = 5.0;   // 이만큼 아무 줄이 없으면 끊고 다시(Manager는 주기마다 보냄)
constexpr double kRetryS = 1.0;
constexpr size_t kMaxLine = 1 << 20;
}  // namespace

ManagerCache::ManagerCache(std::string socket, double connect_s, double response_s, double fresh_s, double period_ms)
    : socket_(std::move(socket)),
      connect_s_(connect_s),
      response_s_(response_s),
      fresh_s_(fresh_s),
      period_ms_(period_ms),
      direct_(socket_, connect_s, response_s, kMaxLine) {}

ManagerCache::~ManagerCache() { stop(); }

void ManagerCache::start(worker::ThreadWatch* watch) {
    if (t_.joinable()) return;
    watch_ = watch;
    if (watch_) slot_ = watch_->add("gw.mgr");
    t_ = std::thread([this] { run(); });
}

void ManagerCache::stop() {
    if (!t_.joinable()) return;
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(sleep_mu_);
        sleep_cv_.notify_all();
    }
    t_.join();  // 기다림마다 상한(0.5 s 읽기, 1 s 연결, list 호출 응답 상한)
    if (watch_) watch_->retire(slot_);
}

void ManagerCache::sleep_s(double s) {
    std::unique_lock<std::mutex> lock(sleep_mu_);
    sleep_cv_.wait_for(lock, std::chrono::duration<double>(s), [&] { return stop_.load(); });
}

bool ManagerCache::fresh_locked(double now) const {
    return order_ok_ && updated_ >= 0 && now - updated_ < fresh_s_;
}

bool ManagerCache::workers(Json& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!fresh_locked(mono_now())) return false;
    out = Json::array();
    for (const auto& id : order_) out.push_back(values_[id]);
    return true;
}

bool ManagerCache::worker(const std::string& id, Json& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!fresh_locked(mono_now())) return false;
    auto it = values_.find(id);
    if (it == values_.end()) return false;  // 없는 워커: Manager가 not_found로 답하게
    out = it->second;
    return true;
}

bool ManagerCache::system(Json& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!fresh_locked(mono_now()) || system_.is_null()) return false;
    out = system_;
    return true;
}

Json ManagerCache::state() {
    std::lock_guard<std::mutex> lock(mu_);
    const double now = mono_now();
    Json o = Json::object();
    o["connected"] = connected_;
    o["fresh"] = fresh_locked(now);
    o["age_s"] = updated_ < 0 ? Json(nullptr) : Json(py_round(now - updated_, 2));
    o["workers"] = order_.size();
    o["events"] = events_;
    o["direct_lists"] = direct_lists_;
    return o;
}

void ManagerCache::run() {
    pthread_setname_np(pthread_self(), "gw.mgr");
    while (!stop_.load()) {
        if (watch_) watch_->beat(slot_);
        session();
        {
            std::lock_guard<std::mutex> lock(mu_);
            connected_ = false;
        }
        if (watch_) watch_->beat(slot_);
        sleep_s(kRetryS);
    }
}

void ManagerCache::session() {
    int err;
    bool timed_out;
    const int fd = connect_unix(socket_, connect_s_, err, timed_out);
    if (fd < 0) return;
    struct Guard {
        int fd;
        ~Guard() { ::close(fd); }
    } guard{fd};
    Json req = Json::object();
    req["op"] = "subscribe";
    req["topics"] = Json::array({"workers", "system"});
    req["period_ms"] = period_ms_;
    if (send_all(fd, compact(req) + "\n", mono_now() + response_s_, err) != Io::Ok) return;
    std::string buf;
    if (read_line(fd, buf, kMaxLine, mono_now() + response_s_, err) != Io::Ok) return;
    {
        auto first = parse_json(buf.substr(0, buf.find('\n')));
        buf.erase(0, buf.find('\n') + 1);
        // {"ok": true, "data": {"subscribed": [...]}}가 아니면(구독을 모르는 Manager) 그만두고 나중에 다시
        if (!first || !first->is_object() || first->value("ok", Json(false)) != Json(true)) return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        connected_ = true;
    }
    seen_.clear();
    double last_rx = mono_now();
    while (!stop_.load()) {
        if (watch_) watch_->beat(slot_);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            auto msg = parse_json(buf.substr(0, nl));
            buf.erase(0, nl + 1);
            last_rx = mono_now();
            if (msg) handle(*msg);
        }
        if (need_order_) relearn_order();
        const Io io = read_line(fd, buf, kMaxLine, mono_now() + 0.5, err);
        if (io == Io::Timeout) {
            if (mono_now() - last_rx > kSilentS) {
                GW_WARN("manager subscription silent for %.0f s; reconnecting", kSilentS);
                return;
            }
            continue;
        }
        if (io != Io::Ok) return;
    }
}

void ManagerCache::handle(const Json& msg) {
    if (!msg.is_object()) return;
    const Json ev = msg.value("event", Json(nullptr));
    auto it = msg.find("data");
    if (it == msg.end()) return;
    const Json& data = *it;
    if (ev == "worker_status") {
        if (!data.is_object() || !data.contains("id") || !data["id"].is_string()) return;
        const std::string id = data["id"].get<std::string>();
        seen_.insert(id);
        std::lock_guard<std::mutex> lock(mu_);
        values_[id] = data;
        ++events_;
        return;
    }
    if (ev != "system") return;
    // 주기 한 바퀴의 끝(Manager는 워커 전체 다음에 system을 보낸다)
    bool unknown = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        system_ = data;
        ++events_;
        const std::set<std::string> known(order_.begin(), order_.end());
        for (const auto& id : seen_)
            if (!known.count(id)) unknown = true;
        // 아는 워커를 모두 봤으면 새로움. 일부만(변화 때 바로 온 system)이면 새로움은 그대로
        if (order_ok_ && !unknown && seen_.size() == known.size()) updated_ = mono_now();
        if (!order_ok_ || unknown) need_order_ = true;
    }
    seen_.clear();
    if (on_system) on_system(data);
}

void ManagerCache::relearn_order() {
    need_order_ = false;
    Json resp;
    try {
        resp = direct_.call(Json{{"op", "list"}});
    } catch (const ManagerUnavailable& e) {
        GW_DEBUG("manager cache: list failed: %s", e.what());
        need_order_ = true;
        return;
    }
    const Json data = resp.value("data", Json(nullptr));
    if (resp.value("ok", Json(false)) != Json(true) || !data.is_array()) {
        need_order_ = true;
        return;
    }
    std::vector<std::string> order;
    std::map<std::string, Json> values;
    for (const auto& w : data) {
        if (!w.is_object() || !w.contains("id") || !w["id"].is_string()) return;  // 모르는 모양: 캐시 안 씀
        order.push_back(w["id"].get<std::string>());
        values[order.back()] = w;
    }
    std::lock_guard<std::mutex> lock(mu_);
    order_ = std::move(order);
    values_ = std::move(values);
    order_ok_ = true;
    if (!system_.is_null()) updated_ = mono_now();
    ++direct_lists_;
}

}  // namespace gw
