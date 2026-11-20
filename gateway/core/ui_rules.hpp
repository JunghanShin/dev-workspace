// 화면 제공 규칙(Python workspace_rest/ui.py): 경로 정규화, Content-Type, 브라우저가 접속한 주소. 시스템 호출 없음.
#pragma once

#include <string>
#include <vector>

#include "routes.hpp"
#include "text.hpp"

namespace gw {

// os.path.normpath / join / relpath / basename / dirname (POSIX)
std::string normpath(const std::string& path);
std::string path_join(const std::string& a, const std::string& b);
std::string relpath(const std::string& path, const std::string& start);  // 둘 다 절대 경로
std::string basename(const std::string& path);

// 화면 파일 위치 판정: base 안의 rel → 정규화한 경로. base 밖이면 ""(파일 있는지는 부르는 쪽이 봄)
std::string contained_path(const std::string& base, const std::string& rel);

// mimetypes.guess_type + ui.py 규칙(.js는 text/javascript, 글이면 "; charset=utf-8")
std::string content_type_for(const std::string& path);

// Host 헤더 → 접속한 주소(포트 없이, 소문자, IPv6는 [..]). 없으면 127.0.0.1
std::string request_host(const std::string& host_header);

// 패널 정렬 열쇠: (order, id). order가 숫자가 아니면 100으로(Python이면 비교 오류)
void sort_panels(Json& panels);

// /api/remote 이름 → reads 경로·인자. 모르는 이름이면 false
bool remote_read_path(const std::string& name, const Params& params, std::string& path, Params& query);
// reads 응답 → 화면이 쓰는 줄 목록
std::vector<std::string> remote_lines(const std::string& name, const Json& obj);

}  // namespace gw
