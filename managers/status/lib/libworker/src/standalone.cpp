// 워크스페이스 없이 쓰는 구현(WORKER_WORKSPACE=OFF). heartbeat를 보내지 않고, 소켓은 $XDG_RUNTIME_DIR 또는 /tmp.
#include <cstdlib>

#include "worker/workspace.hpp"

namespace worker {

namespace {

std::string runtime_dir_socket(const std::string& name) {
    const char* d = std::getenv("XDG_RUNTIME_DIR");
    return (d && *d ? std::string(d) : std::string("/tmp")) + "/" + name;
}

}  // namespace

bool workspace_build() { return false; }

Notifier::Notifier() = default;
Notifier::~Notifier() = default;
void Notifier::send(const std::string&) {}

std::string worker_id_from_environment() { return ""; }
std::string default_command_socket(const std::string& id) { return runtime_dir_socket(id + ".sock"); }
std::string default_stream_socket(const std::string& id) { return runtime_dir_socket(id + ".stream.sock"); }
std::string default_socket(const std::string& id, const std::string& kind) {
    return runtime_dir_socket(id + "." + kind + ".sock");
}
std::string workspace_data_dir() { return ""; }

}  // namespace worker
