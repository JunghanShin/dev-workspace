// JSON 도우미. Python 판(json, dict)과 같은 결과를 내려고 둔다.
//
// - Json = ordered_json: 키 순서를 지킨다(설정 파일을 다시 쓸 때 사람이 쓴 순서 그대로, Python dict와 같음)
// - py_eq: Python ==와 같음(객체는 순서 무시, 1 == 1.0 == true)
// - py_str / py_repr: 메시지에 값을 넣을 때 Python str()·repr()과 같은 글
#pragma once

#include <optional>
#include <string>

#include "nlohmann/json.hpp"

namespace pm {

using Json = nlohmann::ordered_json;

bool py_eq(const Json& a, const Json& b);
bool py_truthy(const Json& v);
bool is_num(const Json& v);  // int·float(bool 아님)
std::string py_str(const Json& v);
std::string py_repr(const Json& v);
std::string py_quote(const std::string& s);  // repr(str)

// 한 줄(API·구독). 깨진 UTF-8은 대체 글자로
std::string dump_line(const Json& v);
// 파일: 들여쓰기 2 + 줄바꿈(Python json.dumps(indent=2, ensure_ascii=False) + "\n")
std::string dump_file(const Json& v);
// 실패하면 nullopt, err에 이유
std::optional<Json> parse(const std::string& text, std::string* err = nullptr);

// heartbeat 값: 정수 → 실수 → 글자 순(Python int() → float() → 그대로)
Json py_number(const std::string& s);
bool py_int(const std::string& s, long long& out);
// "%g"
std::string fmt_g(double x);

}  // namespace pm
