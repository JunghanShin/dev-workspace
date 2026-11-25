// 스트림 중계 상태 규칙 (design.md 9.7, Python workspace_rest/stream.py). 잠금·스레드는 부르는 쪽(src/hub)이 가진다.
//
//   워커 메시지:  {"seq","kf","t","topics":{...},"events":[...],"events_next"}
//   화면 메시지:  {"seq","kf","topics":{...},"events":[...],"t","events_next","stream":"up"|"down"}
//
// - UpstreamState: 워커 하나의 최신 상태(주제별 값, 최근 이벤트 200개). 새 화면에는 snapshot(기준 상태)
// - SseMerge: 화면 하나에 보낼 것을 합친다. 전송 사이에 온 변경은 주제별 최신 값만, 이벤트는 모두(200개까지)
#pragma once

#include <cstddef>

#include "text.hpp"

namespace gw {

constexpr size_t kMaxEvents = 200;

class SseMerge {
public:
    void offer(const Json& msg, bool keyframe);
    bool dirty() const { return dirty_; }
    // 보낼 메시지(합친 것). dirty일 때만 부른다. 부르면 비워진다
    Json take();

private:
    bool kf_ = false;
    Json topics_ = Json::object();
    Json events_ = Json::array();
    Json extra_ = Json::object();  // t, events_next, stream(처음 들어온 순서)
    bool dirty_ = false;
    long long seq_ = 0;
};

// /streams(docs/stream-mux.md) 한 줄: 화면 메시지 + "worker". 꺼짐(stream "down")이면 {"worker", "down": true}를
// 그 워커에 한 번만(down이 이미 true면 "" = 보내지 않음). 다시 올라온 줄(기준 상태)부터는 그대로
std::string mux_line(const std::string& worker, Json msg, bool& down);

class UpstreamState {
public:
    // 워커 메시지 하나 → 화면에 나눠 줄 메시지. fresh(다시 붙은 뒤 첫 기준 상태)면 상태를 비우고 기준 상태를 만든다
    Json apply(const Json& msg, bool fresh);
    // 새 화면용 기준 상태
    Json snapshot(bool up) const;

private:
    Json topics_ = Json::object();
    Json events_ = Json::array();
    double last_event_ = 0;
    Json extra_ = Json::object();
};

}  // namespace gw
