// 글·JSON 도우미. Python REST(json, str)와 같은 결과를 내려고 둔다. 시스템 호출 없음.
//
// - Json = ordered_json: 키 순서를 지킨다(Python dict와 같음)
// - py_dumps: json.dumps(obj, ensure_ascii=False) 그대로(구분자 ", " ": "). HTTP JSON 응답
// - compact: json.dumps(separators=(",", ":")). SSE·지난 지표 줄
// - utf8_replace / splitlines / universal_newlines: 파일·자식 출력 읽기(errors="replace", 텍스트 모드)
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace gw {

using Json = nlohmann::ordered_json;

std::string py_dumps(const Json& v);
std::string compact(const Json& v, bool ensure_ascii = false);
// 엄격한 JSON(앞의 BOM도 오류, Python json.loads와 같음). 실패하면 nullopt
std::optional<Json> parse_json(const std::string& text, std::string* err = nullptr);

bool py_truthy(const Json& v);
std::string py_str(const Json& v);       // str(): CSV 칸
double py_round(double x, int nd);       // round(x, nd)
std::string fmt_fixed(double x, int nd); // "%.{nd}f"
bool py_int(const std::string& s, long long& out);   // int("...")
bool py_float(const std::string& s, double& out);    // float("...")
double py_min(double a, double b);       // min(a, b) (nan 처리까지 Python과 같게)
double py_max(double a, double b);

// 깨진 UTF-8은 U+FFFD로
std::string utf8_replace(const std::string& bytes);
size_t utf8_count(const std::string& s);                       // 글자 수(코드 포인트)
std::string utf8_prefix(const std::string& s, size_t chars);   // 앞에서 chars 글자
// str.splitlines(): \n \r \r\n \v \f \x1c \x1d \x1e U+0085 U+2028 U+2029
std::vector<std::string> splitlines(const std::string& s);
// 텍스트 모드 읽기: \r\n, \r → \n
std::string universal_newlines(const std::string& s);
// str.split(): 공백으로 나눔(빈 칸 없음)
std::vector<std::string> split_ws(const std::string& s);
std::string strip(const std::string& s);
std::string rstrip_char(const std::string& s, char c);
std::string to_lower(const std::string& s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
// Python 정규식 ^...$ 처럼 끝의 "\n" 하나는 봐준다
std::string drop_final_newline(const std::string& s);

// OSError 글: "[Errno 2] No such file or directory"
std::string errno_text(int err);

}  // namespace gw
