#include "worker/client.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace worker {

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

Json error(const char* code, const std::string& message) {
    return Json{{"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}

}  // namespace

Json send_command(const std::string& path, const std::string& cmd, const Json& args,
                  std::chrono::milliseconds timeout) {
    return send_request(path, Json{{"cmd", cmd}, {"args", args}}, timeout);
}

Json manager_call(const Json& request, std::chrono::milliseconds timeout) {
    const char* path = std::getenv("WORKSPACE_MANAGER_SOCKET");
    if (!path || !*path) return error("no_manager_api", "WORKSPACE_MANAGER_SOCKET not set (worker type has no \"manager_api\": true)");
    return send_request(path, request, timeout);
}

Json send_request(const std::string& path, const Json& request, std::chrono::milliseconds timeout) {
    return send_request(path, request, timeout, std::function<void()>());
}

Json send_request(const std::string& path, const Json& request, std::chrono::milliseconds timeout,
                  const std::function<void()>& tick) {
    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path)) return error("connect", "socket path too long: " + path);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return error("connect", std::strerror(errno));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string why = std::strerror(errno);
        close(fd);
        return error("connect", path + ": " + why);
    }
    const std::string line = request.dump() + "\n";
    if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(line.size())) {
        close(fd);
        return error("connect", "send failed");
    }
    std::string in;
    const auto end = Clock::now() + timeout;
    while (in.find('\n') == std::string::npos) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
        if (left <= 0) {
            close(fd);
            return error("timeout", "no reply");
        }
        pollfd p{fd, POLLIN, 0};
        const int wait = tick ? static_cast<int>(std::min<long long>(left, 200)) : static_cast<int>(left);
        if (poll(&p, 1, wait) <= 0) {
            if (tick) tick();
            continue;
        }
        char buf[4096];
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        in.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    try {
        return Json::parse(in.substr(0, in.find('\n')));
    } catch (const std::exception&) {
        return error("bad_reply", in);
    }
}

}  // namespace worker
