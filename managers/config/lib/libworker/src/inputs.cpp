#include "worker/inputs.hpp"

#include <stdexcept>

#include "worker/workspace.hpp"

namespace worker {

using nlohmann::json;

namespace {
bool valid_id(const std::string& s) {
    if (s.empty() || s.size() > 64 || s.find('/') != std::string::npos) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}
std::string str(const json& j, const char* key, const std::string& where) {
    if (!j.contains(key)) return "";
    if (!j[key].is_string()) throw std::runtime_error(where + "." + key + " must be a string");
    return j[key].get<std::string>();
}
}  // namespace

InputAddress resolve_input(const std::string& name, const json& spec) {
    const std::string where = "inputs." + name;
    if (!spec.is_object()) throw std::runtime_error(where + " must be an object");
    for (const auto& [k, v] : spec.items())
        if (k != "worker" && k != "command" && k != "stream" && k != "data")
            throw std::runtime_error(where + ": unknown key " + k);
    InputAddress in;
    in.name = name;
    in.worker = str(spec, "worker", where);
    if (!in.worker.empty() && !valid_id(in.worker)) throw std::runtime_error(where + ".worker: bad worker id");
    in.command = str(spec, "command", where);
    in.stream = str(spec, "stream", where);
    if (spec.contains("data")) {
        if (!spec["data"].is_object()) throw std::runtime_error(where + ".data must be an object");
        for (const auto& [dn, p] : spec["data"].items()) {
            if (!p.is_string()) throw std::runtime_error(where + ".data." + dn + " must be a string");
            in.data[dn] = p.get<std::string>();
        }
    }
    // 워커 id만 주면 기본 소켓 경로(9.6·9.7·6.7). 직접 준 경로는 그대로
    if (!in.worker.empty()) {
        if (in.command.empty()) in.command = default_command_socket(in.worker);
        if (in.stream.empty()) in.stream = default_stream_socket(in.worker);
        if (in.data.empty()) in.data["frames"] = default_socket(in.worker, "frames");
    }
    return in;
}

std::map<std::string, InputAddress> resolve_inputs(const json& inputs, const std::string& from) {
    std::map<std::string, InputAddress> out;
    if (inputs.is_null()) return out;
    if (!inputs.is_object()) throw std::runtime_error("inputs must be an object {\"<input>\": {...}}");
    for (const auto& [name, spec] : inputs.items()) {
        InputAddress in = resolve_input(name, spec);
        in.from = from;
        out[name] = std::move(in);
    }
    return out;
}

}  // namespace worker
