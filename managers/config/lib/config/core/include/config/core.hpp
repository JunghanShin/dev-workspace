// config 코어(managers/config/design.md 2·3장). libworker를 모른다. 파일은 읽기만(쓰기는 모듈이 io 스레드에서).
//
//   버전 목록: 실행 파일 폴더의 BUILD(소스 커밋·빌드 시각·sha256) → 실제 파일 sha256과 대조
//   스냅숏 비교: 두 스냅숏(JSON)의 다른 점(워커 설정 칸별, 연결 선언, 파일 줄별, 버전)
//   남기기: 수 상한을 넘으면 오래된 auto부터 지울 것을 고름
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace config {

using Json = nlohmann::json;

std::string sha256_hex(const std::string& data);
// 파일 sha256(읽을 수 없으면 빈 글)
std::string file_sha256(const std::string& path);

struct Build {
    std::string commit, built, env;
    std::map<std::string, std::string> sha;  // 파일 이름 → sha256
};
// BUILD 파일 내용 → Build
Build parse_build(const std::string& text);

// 부품 하나(실행 파일 폴더): {"commit", "built", "files": {이름: {"ok", "sha"}}, "ok"}. 폴더에 BUILD가 없으면 null
Json component_version(const std::string& bin_dir);

// 워크스페이스 루트에서 실행 파일 폴더들: manager/bin, gateway/bin, monitor/bin, managers/*/bin, workers/*/workspace/bin|bin
std::map<std::string, std::string> bin_dirs(const std::string& root);

// 두 스냅숏의 다른 점. 칸: workers.<id>.<경로>, connections, files.<이름>(줄 +/-), versions.<부품>
Json diff(const Json& from, const Json& to);
// 설정 부분만(버전·시각 빼고) 같은가: 자동 스냅숏을 남길지
bool same_config(const Json& a, const Json& b);

struct SnapshotInfo {
    std::string id, label;
};
// 남길 수(keep)를 넘으면 지울 id들: 오래된 auto부터. 이름 붙인 것은 지우지 않음. infos는 오래된 것부터
std::vector<std::string> to_prune(const std::vector<SnapshotInfo>& infos, size_t keep);

}  // namespace config
