#include "worker/manager_link.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

namespace worker {

namespace {

constexpr int kWaitMs = 100;  // 스레드 대기 상한(이보다 오래 막히지 않고 beat)

std::string env_str(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

double mono_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int connect_unix(const std::string& path, std::string* why) {
    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path)) {
        *why = "socket path too long";
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *why = std::strerror(errno);
        return -1;
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        *why = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

void nap(WorkerThread& t, double seconds) {
    const double until = mono_s() + seconds;
    while (!t.stopping() && mono_s() < until) {
        t.beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(kWaitMs));
    }
}

}  // namespace

// ---- ManyInputs ---------------------------------------------------------------------------

ManyInputs::ManyInputs(std::string path, std::string self_id)
    : path_(path.empty() ? env_str("WORKSPACE_INPUTS_FILE") : std::move(path)), self_(std::move(self_id)) {}

bool ManyInputs::refresh() {
    if (path_.empty()) return false;
    struct stat st {};
    const long long mt = ::stat(path_.c_str(), &st) == 0
                             ? static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec
                             : -2;
    if (mt == mtime_ns_) return false;
    mtime_ns_ = mt;
    std::map<std::string, std::vector<std::string>> next;
    if (mt >= 0) {
        std::ifstream in(path_);
        const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
        if (doc.is_object() && doc.contains("many") && doc["many"].is_object())
            for (const auto& [name, list] : doc["many"].items()) {
                std::vector<std::string> ids;
                if (list.is_array())
                    for (const auto& x : list)
                        if (x.is_object() && x.contains("worker") && x["worker"].is_string() && x["worker"] != self_)
                            ids.push_back(x["worker"].get<std::string>());
                std::sort(ids.begin(), ids.end());
                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                next[name] = std::move(ids);
            }
    }
    if (next == lists_) return false;
    lists_ = std::move(next);
    return true;
}

std::vector<std::string> ManyInputs::workers(const std::string& input) const {
    auto it = lists_.find(input);
    return it == lists_.end() ? std::vector<std::string>{} : it->second;
}

// ---- ManagerSubscription ------------------------------------------------------------------

ManagerSubscription::ManagerSubscription(ThreadWatch& watch, Options opt)
    : opt_(std::move(opt)), queue_(opt_.queue, Overflow::DropOldest),
      thread_(opt_.thread_name, watch, [this](WorkerThread& t) { run(t); }) {
    if (opt_.socket.empty()) opt_.socket = env_str("WORKSPACE_MANAGER_SOCKET");
    if (opt_.socket.empty() && !env_str("WORKSPACE_DATA").empty()) opt_.socket = env_str("WORKSPACE_DATA") + "/run/manager.sock";
}

ManagerSubscription::~ManagerSubscription() { stop(); }

void ManagerSubscription::start() { thread_.start(); }

void ManagerSubscription::stop() {
    thread_.request_stop();
    thread_.join();
}

void ManagerSubscription::run(WorkerThread& t) {
    const double read_timeout = opt_.read_timeout_sec > 0 ? opt_.read_timeout_sec : std::max(5.0, opt_.period_ms * 5 / 1000.0);
    while (!t.stopping()) {
        std::string why;
        const int fd = opt_.socket.empty() ? -1 : connect_unix(opt_.socket, &why);
        if (fd < 0) {
            queue_.push(Event{Event::Down, nlohmann::ordered_json(), opt_.socket.empty() ? "no manager socket" : why});
            nap(t, opt_.retry_sec);
            continue;
        }
        nlohmann::ordered_json sub = nlohmann::ordered_json::object();
        sub["op"] = "subscribe";
        sub["topics"] = opt_.topics;
        sub["period_ms"] = opt_.period_ms;
        const std::string req = sub.dump() + "\n";
        (void)!::send(fd, req.data(), req.size(), MSG_NOSIGNAL);

        std::string buf, reason = "manager closed the subscription";
        bool first = true;
        double last_rx = mono_s();
        char chunk[65536];
        while (!t.stopping()) {
            t.beat();
            pollfd p{fd, POLLIN, 0};
            const int n = ::poll(&p, 1, kWaitMs);
            if (n < 0 && errno != EINTR) {
                reason = std::strerror(errno);
                break;
            }
            if (n <= 0) {
                if (mono_s() - last_rx > read_timeout) {
                    reason = "timed out";
                    break;
                }
                continue;
            }
            const ssize_t r = ::recv(fd, chunk, sizeof chunk, 0);
            if (r <= 0) break;
            last_rx = mono_s();
            buf.append(chunk, static_cast<size_t>(r));
            size_t pos;
            bool stop_session = false;
            while ((pos = buf.find('\n')) != std::string::npos) {
                const std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                nlohmann::ordered_json msg = nlohmann::ordered_json::parse(line, nullptr, false);
                if (first) {
                    first = false;
                    if (!msg.is_object() || !msg.value("ok", false)) {
                        reason = "subscribe refused: " + (msg.is_object() && msg.contains("error") ? msg["error"].dump() : line);
                        stop_session = true;
                        break;
                    }
                    connected_ = true;
                    queue_.push(Event{Event::Connected, nlohmann::ordered_json(), ""});
                    continue;
                }
                if (msg.is_discarded()) continue;  // 깨진 줄은 건너뜀
                queue_.push(Event{Event::Line, std::move(msg), ""});
            }
            if (stop_session) break;
            if (buf.size() > (1u << 22)) buf.clear();  // 줄바꿈 없는 비정상 입력
        }
        ::close(fd);
        connected_ = false;
        if (t.stopping()) break;
        queue_.push(Event{Event::Down, nlohmann::ordered_json(), reason});
        nap(t, opt_.retry_sec);
    }
}

}  // namespace worker
