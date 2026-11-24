#include "worker/peer.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "worker/client.hpp"
#include "worker/workspace.hpp"

namespace worker {

using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr int kWaitMs = 100;  // 스레드 대기 상한(이보다 오래 막히지 않고 beat)

int connect_unix(const std::string& path) {
    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path)) return -1;
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

// ---- Subscriber ---------------------------------------------------------------------------

Subscriber::Subscriber(ThreadWatch& watch, std::string peer_id, Options opt)
    : peer_(std::move(peer_id)),
      path_(opt.socket.empty() ? default_stream_socket(peer_) : opt.socket),
      opt_(std::move(opt)),
      queue_(opt_.queue, Overflow::DropOldest),
      thread_(opt_.thread_name, watch, [this](WorkerThread& t) { run(t); }) {}

Subscriber::~Subscriber() { stop(); }

void Subscriber::start() { thread_.start(); }

void Subscriber::stop() {
    thread_.request_stop();
    thread_.join();
}

void Subscriber::run(WorkerThread& t) {
    bool ever = false;
    while (!t.stopping()) {
        const int fd = connect_unix(path_);
        if (fd < 0) {
            // 상대가 아직 없다(꺼짐·다시 시작 중). 기다렸다 다시
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(opt_.retry_sec * 1000));
            while (!t.stopping() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kWaitMs));
                t.beat();
            }
            continue;
        }
        if (ever) ++reconnects_;
        ever = true;
        connected_ = true;
        std::string buf;
        while (!t.stopping()) {
            pollfd p{fd, POLLIN, 0};
            const int r = poll(&p, 1, kWaitMs);
            t.beat();
            if (r < 0 && errno != EINTR) break;
            if (r <= 0) continue;
            char tmp[8192];
            const ssize_t n = recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) break;
            buf.append(tmp, static_cast<size_t>(n));
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                const std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                Json j = Json::parse(line, nullptr, false);
                if (!j.is_object()) continue;
                Update u;
                u.kf = j.value("kf", false);
                if (j.contains("topics") && j["topics"].is_object()) u.topics = j["topics"];
                if (j.contains("events") && j["events"].is_array()) u.events = j["events"];
                ++received_;
                queue_.push(std::move(u));
            }
            if (buf.size() > (1u << 20)) buf.clear();  // 줄바꿈 없는 비정상 입력
        }
        ::close(fd);
        connected_ = false;
        Update down;
        down.down = true;
        queue_.push(std::move(down));
    }
}

// ---- CommandClient ------------------------------------------------------------------------

CommandClient::CommandClient(ThreadWatch& watch, std::string peer_id, Options opt)
    : peer_(std::move(peer_id)),
      path_(opt.socket.empty() ? default_command_socket(peer_) : opt.socket),
      opt_(std::move(opt)),
      requests_(opt_.queue, Overflow::Reject),
      replies_(opt_.queue, Overflow::DropOldest),
      thread_(opt_.thread_name, watch, [this](WorkerThread& t) { run(t); }) {}

CommandClient::~CommandClient() { stop(); }

void CommandClient::start() { thread_.start(); }

void CommandClient::stop() {
    thread_.request_stop();
    requests_.close();
    thread_.join();
}

uint64_t CommandClient::send(const std::string& cmd, const Json& args) {
    const uint64_t id = next_id_++;
    return requests_.push(Request{id, cmd, args}) ? id : 0;
}

void CommandClient::run(WorkerThread& t) {
    while (!t.stopping()) {
        auto r = requests_.pop_for(std::chrono::milliseconds(kWaitMs));
        t.beat();
        if (!r) continue;
        // 응답을 기다리는 동안 beat가 멈추므로, timeout은 thread_stall_sec보다 짧게 둔다(기본 2 s)
        Json resp = send_command(path_, r->cmd, r->args, std::chrono::milliseconds(opt_.timeout_ms));
        const auto code = resp.value("error", Json::object()).value("code", std::string());
        reachable_ = code != "connect" && code != "timeout";
        replies_.push(Reply{r->id, r->cmd, std::move(resp)});
        t.beat();
    }
}

}  // namespace worker
