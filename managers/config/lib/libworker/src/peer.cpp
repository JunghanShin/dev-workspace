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
#include "worker/input_check.hpp"
#include <sys/stat.h>
#include <cstdlib>
#include <fstream>
#include <algorithm>
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
                input_check::observe(peer_, u.topics);  // 받은 칸 실행 중 검사(막지 않음, docs/input-check.md)
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
      thread_(opt_.thread_name, watch, [this](WorkerThread& t) { run(t); }) {
    if (!opt_.socket.empty()) opt_.via_entry = false;  // 소켓을 직접 줌: 그곳으로
    if (const char* pf = std::getenv("WORKSPACE_PEERS_FILE"); pf && *pf && opt_.via_entry) peers_file_ = pf;
}

CommandClient::~CommandClient() { stop(); }

std::string CommandClient::entry() {
    if (peers_file_.empty()) return "";
    struct stat st{};
    if (::stat(peers_file_.c_str(), &st) != 0) {
        peers_mtime_ns_ = -1;
        entry_.clear();
        return entry_;
    }
    const long long m = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    if (m == peers_mtime_ns_) return entry_;
    peers_mtime_ns_ = m;
    entry_.clear();
    try {
        std::ifstream in(peers_file_);
        const Json doc = Json::parse(in);
        const Json e = doc.value("entry", Json());
        if (e.is_object() && e.value("id", Json()).is_string()) entry_ = e["id"].get<std::string>();
    } catch (const std::exception&) {
        peers_mtime_ns_ = -1;  // 쓰는 중이었을 수 있음: 다음에 다시
    }
    return entry_;
}

void CommandClient::start() { thread_.start(); }

void CommandClient::stop() {
    thread_.request_stop();
    requests_.close();
    thread_.join();
}

uint64_t CommandClient::send(const std::string& cmd, const Json& args) {
    const uint64_t id = next_id_++;
    return requests_.push(Request{id, cmd, args, Json()}) ? id : 0;
}

uint64_t CommandClient::send(const std::string& cmd, const Json& args, const Json& envelope) {
    const uint64_t id = next_id_++;
    return requests_.push(Request{id, cmd, args, envelope}) ? id : 0;
}

void CommandClient::run(WorkerThread& t) {
    while (!t.stopping()) {
        auto r = requests_.pop_for(std::chrono::milliseconds(kWaitMs));
        t.beat();
        if (!r) continue;
        // 응답을 기다리는 동안 beat가 멈추므로, timeout은 thread_stall_sec보다 짧게 둔다(기본 2 s)
        Json resp;
        const auto tick = [&t] { t.beat(); };
        const std::string via = r->cmd == "get_status" ? std::string() : entry();
        if (!via.empty() && via != peer_) {
            // 명령 입구로 감쌈. 입구 상한(2.8 s)보다 길게 기다려 응답이 늘 입구의 답으로 오게
            const Json req = {{"cmd", "mission.send"}, {"args", {{"target", peer_}, {"cmd", r->cmd}, {"args", r->args}}}};
            resp = send_request(default_command_socket(via), req, std::chrono::milliseconds(std::max(opt_.timeout_ms, 3000)), tick);
            ++via_count_;
        } else if (r->envelope.is_object() && !r->envelope.empty()) {
            Json req = r->envelope;
            req["cmd"] = r->cmd;
            req["args"] = r->args;
            resp = send_request(path_, req, std::chrono::milliseconds(opt_.timeout_ms), tick);
        } else {
            resp = send_request(path_, Json{{"cmd", r->cmd}, {"args", r->args}}, std::chrono::milliseconds(opt_.timeout_ms), tick);
        }
        const auto code = resp.value("error", Json::object()).value("code", std::string());
        reachable_ = code != "connect" && code != "timeout";
        replies_.push(Reply{r->id, r->cmd, std::move(resp)});
        t.beat();
    }
}

}  // namespace worker
