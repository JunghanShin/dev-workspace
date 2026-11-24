// 플랫폼 기록 규칙: process-manager 구독 메시지 → 기록 줄(managers/record/design.md 2.1).
// 예전 Python workspace-logger와 같은 규칙·같은 줄 모양(키 순서까지)이라 게이트웨이 /history가 그대로 읽는다.
//
//   worker   워커 상태·지표. 상태 키가 바뀌면 바로, 아니면 워커마다 interval에 한 번
//   removed  워커가 빠짐
//   command  쓰기 명령 이력
//   system   Manager 클라이언트 접속 상태가 바뀔 때
//   logger   기록기 자신(시작·끝·붙음·끊김). 이름은 예전 그대로(/history·기존 파일과 맞춤)
//
// 시스템 호출·시계를 모른다(now는 인자로). 돌려준 기록에는 아직 시각이 없다(줄로 만들 때 붙임: line.hpp)
#pragma once

#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace record {

using Json = nlohmann::ordered_json;

class PlatformRecorder {
public:
    explicit PlatformRecorder(double interval_s) : interval_(interval_s) {}

    // 구독 한 줄({"event": ..., "data": ...}) → 남길 기록(0개 이상). now_s: 단조 시계(초)
    std::vector<Json> handle(const Json& msg, double now_s);
    // 구독이 붙음: logger connected
    Json connected();
    // 구독이 끊김: 붙어 있었을 때만 logger disconnected 하나. 다시 붙으면 모든 워커를 새로 남기게 잊는다
    std::vector<Json> disconnected(const std::string& reason);
    bool is_connected() const { return connected_; }

    // 상태가 바뀐 것으로 보는 키(이 값들이 바뀌면 주기와 상관없이 바로)
    static const std::vector<std::string>& state_keys();

private:
    double interval_;
    std::map<std::string, Json> last_state_;     // 워커 id → 상태 키 값들
    std::map<std::string, double> last_written_;  // 워커 id → 마지막으로 남긴 시각
    Json last_system_;
    bool connected_ = false;
};

}  // namespace record
