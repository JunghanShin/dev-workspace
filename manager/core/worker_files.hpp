// 워커 설정 파일: 워커 폴더에 있다 (design.md 6.3.1). Python workerfiles.py와 같은 파일 모양.
//
//   <워커 폴더>/workspace/config.json         {"active": "<프리셋>", "autostart": false}
//   <워커 폴더>/workspace/presets/<이름>.json  {"label": "<화면 이름>", "config": {...}}
//
// 읽기는 바로(FileSystem), 쓰기는 FileOp로 돌려주고 Supervisor가 pm.store로 보낸다.
#pragma once

#include <optional>
#include <stdexcept>
#include <string>

#include "json_util.hpp"
#include "ports.hpp"

namespace pm {

extern const char* const kDefaultPreset;   // "default"
extern const char* const kNamePattern;     // 프리셋 이름 규칙(메시지에 그대로)

struct WorkerFileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// 이름이 규칙에 맞지 않으면 WorkerFileError
void check_name(const Json& name);
bool name_ok(const std::string& name);

struct WorkerConfig {
    std::string active;
    bool autostart = false;
};

struct Preset {
    Json label;
    Json config;
};

class WorkerFiles {
public:
    WorkerFiles(FileSystem& fs, std::string dir);

    const std::string& dir() const { return dir_; }
    std::string preset_path(const Json& name) const;  // 이름이 틀리면 WorkerFileError

    std::optional<WorkerConfig> read_config() const;   // 없으면 nullopt, 깨졌으면 WorkerFileError
    FileOp write_config(const std::string& active, bool autostart) const;

    bool has_preset(const Json& name) const;
    std::optional<Preset> read_preset(const Json& name) const;
    Json presets() const;  // [{"name", "label"}] 이름순
    FileOp write_preset(const Json& name, const Json& label, const Json& config) const;
    FileOp delete_preset(const Json& name) const;
    FileOp rename_preset(const Json& name, const Json& new_name) const;

private:
    std::optional<Json> read_json(const std::string& path) const;

    FileSystem& fs_;
    std::string dir_, config_path_, presets_dir_;
};

}  // namespace pm
