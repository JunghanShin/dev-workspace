// 보드에서 화면 제공 (design.md 6.11, Python workspace_rest/ui.py). 3단계에서 화면 파일은 nginx로 옮긴다.
//
//   GET  /, /<파일>              화면 파일(ui/web, 워커 패널 폴더). 그 밖은 주지 않음
//   GET  /api/remote/<이름>       보드 읽기 → {"ok", "lines"}
//   GET  /api/panels             공용 패널(ui/web/panels/*/panel.json) + 워커 패널(workers/<워커>/workspace/panel/)
//   GET  /api/ui                 제목·갱신 주기·영상 주소 틀
#pragma once

#include <map>
#include <optional>
#include <string>

#include "reads.hpp"
#include "text.hpp"

namespace gw {

struct UiConfig {
    std::string web;          // ui/web(절대 경로)
    std::string workers;      // workers 폴더
    std::string media_url;    // {host}는 접속한 주소로
    long long refresh_ms = 1000;
    std::string title = "Workspace";
};

struct StaticFile {
    std::string data, ctype;
};

class Ui {
public:
    explicit Ui(UiConfig cfg) : cfg_(std::move(cfg)) {}
    bool available() const;  // shell.html이 있는가
    std::optional<StaticFile> static_file(const std::string& path) const;
    Json panels() const;
    Json ui_info(const std::string& host_header, const std::string& data_dir, bool product) const;
    // (상태 코드, 응답)
    std::pair<int, Json> remote(const std::string& name, const Params& params, Reads& reads) const;
    const UiConfig& config() const { return cfg_; }

private:
    std::map<std::string, std::string> worker_panel_dirs() const;  // 패널 id → 폴더(이름 순, 같은 id면 뒤의 것)
    UiConfig cfg_;
};

}  // namespace gw
