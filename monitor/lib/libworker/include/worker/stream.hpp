// 스트림 한 줄 (워크스페이스 design.md 9.7, 공통 정의 io/common.json "stream", libworker 0.19).
//
// 스트림을 내는 모듈은 줄을 손으로 만들지 않고 이것을 쓴다. 호스트 이벤트 큐(임대 만료, 받은 칸 검사,
// 모듈이 host()->event로 낸 것)를 여기서 꺼내 붙이므로 빠뜨릴 수 없다.
//
//     pub_->publish(worker::stream_line(host(), ++seq_, kf, {{"<주제>", {...}}}));
//
// - 칸: seq, kf, t(보낸 시각, unix 초), topics, events, events_next(0)
// - events = 모듈 자기 이벤트(events) 먼저, 그 뒤 호스트 이벤트(host->take_events()). host가 nullptr이면 모듈 것만
// - 워커 루프 스레드에서만(take_events가 스레드 안전하지 않다). 모듈 on_timer·on_poll·handle 안이면 된다
// - 구독자가 없어 발행하지 않으면 부르지 않는다: 이벤트는 다음 줄까지 큐에 남는다(큐는 64개까지, 넘으면 오래된 것부터 버림)
#pragma once

#include <cstdint>
#include <string>

#include "worker/config.hpp"

namespace worker {

class Host;

// 스트림 한 줄(줄바꿈 없는 JSON 글)
std::string stream_line(Host* host, uint64_t seq, bool kf, Json topics, Json events = Json::array());

}  // namespace worker
