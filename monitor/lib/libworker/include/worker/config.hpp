// 워커 설정 파일 읽기 (design.md 9.1, 6.6 독립 실행).
//
// 형식은 Manager가 넘기는 파일과 같다. 단독 실행할 때도 같은 형식으로 쓴다:
//     {"id": "cam1", "config_version": 3, "config": {...워커 설정...}}
// - id가 없으면 (워크스페이스 연동) WORKSPACE_WORKER_ID, 그것도 없으면 설정 파일 이름(확장자 제외)
// - config_version은 선택(바로 적용 확인용)
#pragma once

#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "nlohmann/json.hpp"
#include "worker/workspace.hpp"

namespace worker {

using Json = nlohmann::json;

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ConfigFile {
    std::string id;
    long config_version = 0;
    Json config = Json::object();  // "config" 객체(없으면 빈 객체)
};

// 파일을 읽어 id·버전·config를 꺼낸다. 열 수 없거나 JSON이 아니면 ConfigError.
ConfigFile load_config_file(const std::string& path);

// config에 known 밖의 키가 있으면 ConfigError("unknown config key: ...").
void check_keys(const Json& config, const std::set<std::string>& known);

void require(bool ok, const std::string& message);

// config[key]가 있으면 out에 넣는다. 타입이 다르면 ConfigError. 없으면 out은 그대로(기본값).
template <class T>
void read(const Json& config, const char* key, T& out) {
    const auto it = config.find(key);
    if (it == config.end()) return;
    bool ok;
    if constexpr (std::is_same_v<T, std::string>) {
        ok = it->is_string();
    } else if constexpr (std::is_same_v<T, bool>) {
        ok = it->is_boolean();
    } else {
        ok = it->is_number();
    }
    if (!ok) throw ConfigError(std::string("config.") + key + " has a wrong type");
    out = it->template get<T>();
}

// 소켓 기본 경로·데이터 폴더(default_command_socket 등)는 워크스페이스 연동이라 worker/workspace.hpp

}  // namespace worker
