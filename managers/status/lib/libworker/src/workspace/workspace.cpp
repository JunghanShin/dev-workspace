// 워크스페이스 연동 구현(WORKER_WORKSPACE=ON). 워크스페이스 밖에서는 이 파일 대신 src/standalone.cpp를 빌드한다.
#include "worker/workspace.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace worker {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string runtime_dir_socket(const std::string& name) {
    if (!env("XDG_RUNTIME_DIR").empty()) return env("XDG_RUNTIME_DIR") + "/" + name;
    return "/tmp/" + name;
}

}  // namespace

bool workspace_build() { return true; }

Notifier::Notifier() {
    const char* path = std::getenv("WORKSPACE_NOTIFY_SOCKET");
    if (!path || !*path || std::strlen(path) >= sizeof(addr_.sun_path)) return;
    fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    addr_.sun_family = AF_UNIX;
    std::strcpy(addr_.sun_path, path);
}

Notifier::~Notifier() {
    if (fd_ >= 0) close(fd_);
}

void Notifier::send(const std::string& msg) {
    if (fd_ < 0) return;
    (void)!sendto(fd_, msg.data(), msg.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
                  reinterpret_cast<const sockaddr*>(&addr_), sizeof(addr_));
}

std::string worker_id_from_environment() { return env("WORKSPACE_WORKER_ID"); }

std::string default_command_socket(const std::string& id) {
    if (!env("WORKSPACE_DATA").empty()) return env("WORKSPACE_DATA") + "/run/cmd/" + id + ".sock";
    return runtime_dir_socket(id + ".sock");
}

std::string default_stream_socket(const std::string& id) {
    if (!env("WORKSPACE_DATA").empty()) return env("WORKSPACE_DATA") + "/run/stream/" + id + ".sock";
    return runtime_dir_socket(id + ".stream.sock");
}

std::string default_socket(const std::string& id, const std::string& kind) {
    if (!env("WORKSPACE_DATA").empty()) return env("WORKSPACE_DATA") + "/run/" + kind + "/" + id + ".sock";
    return runtime_dir_socket(id + "." + kind + ".sock");
}

std::string workspace_data_dir() {
    if (!env("WORKSPACE_DATA").empty()) return env("WORKSPACE_DATA");
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    std::string p(buf, static_cast<size_t>(n));
    // 실행 파일 위치에서 위로 올라가며 "workers" 폴더를 찾는다(bin/, workspace/bin/, build/, build/jetson/install/bin/ 모두)
    for (int i = 0; i < 8; ++i) {
        const auto k = p.rfind('/');
        if (k == std::string::npos || k == 0) return "";
        p.resize(k);
        const auto j = p.rfind('/');
        if (j != std::string::npos && j > 0 && p.compare(j + 1, std::string::npos, "workers") == 0)
            return p.substr(0, j) + "_data";
    }
    return "";
}

}  // namespace worker
