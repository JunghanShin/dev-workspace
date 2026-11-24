// 입력 주소 (워크스페이스 design.md 6.6.7.3·6.6.7.5). 워커가 쓰는 정의(io.json "uses")의 입력마다 상대 주소.
//
//   어디서:  1) 워크스페이스: process-manager가 연결 선언·자동 연결로 푼 결과 파일(환경 변수 WORKSPACE_INPUTS_FILE,
//               {"inputs": {...}}). 설정 파일 키가 아닌 것은 예전 libworker가 모르는 설정 키에 멈추지 않게
//            2) 단독 실행·워크스페이스 선언이 없을 때: 설정 맨 위 호스트 키 "inputs"
//            같은 입력이 둘 다 있으면 1)이 이긴다
//   모양:    {"worker": "<id>"}(기본 소켓 경로로 바꿈) 또는 {"command": "<경로>", "stream": "<경로>", "data": {"<이름>": "<경로>"}}
//
// 모듈은 host.uses("<입력>")로 주소를 받아 Subscriber·CommandClient·framebus::Consumer를 만든다(주소를 직접 계산하지
// 않음). 붙었는지는 host.input_link("<입력>", true|false)로 알리면 지표 inputs_<입력>_link와 get_status "inputs"에 나온다.
#pragma once

#include <map>
#include <string>

#include "nlohmann/json.hpp"

namespace worker {

struct InputAddress {
    std::string name;    // 입력 이름(io.json uses의 키)
    std::string worker;  // 상대 워커 id(주소를 직접 받았으면 빈 글)
    std::string command, stream;
    std::map<std::string, std::string> data;  // 데이터 이름 → 데이터 버스 소켓
    std::string from;    // 어디서 왔나: "workspace"(process-manager) | "config"(설정 inputs)
    bool connected() const { return !command.empty() || !stream.empty() || !data.empty(); }
};

// {"worker": id} 또는 주소 → Input. 형식이 틀리면 std::runtime_error(설정 오류)
InputAddress resolve_input(const std::string& name, const nlohmann::json& spec);

// {"<입력>": spec} → 입력 표. 틀리면 std::runtime_error
std::map<std::string, InputAddress> resolve_inputs(const nlohmann::json& inputs, const std::string& from);

}  // namespace worker
