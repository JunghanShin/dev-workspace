// 시계·파일·Unix 소켓 도우미(바깥과 닿는 곳). 모든 기다림에 시간 상한.
#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "text.hpp"

namespace gw {

double mono_now();  // 초(steady)
double wall_now();  // 초(time.time())

// ---- 파일 ----
bool is_file(const std::string& path);   // 일반 파일(링크는 따라감)
bool is_dir(const std::string& path);
bool path_exists(const std::string& path);
// 이름 목록(., .. 제외, 읽은 차례 = os.listdir). 없으면 빈 목록
std::vector<std::string> list_dir(const std::string& path);
// 통째로(상한 max 바이트). 없거나 못 읽으면 nullopt, err에 errno
std::optional<std::string> read_file(const std::string& path, size_t max = static_cast<size_t>(-1), int* err = nullptr);
// 텍스트 모드 읽기(errors="replace", 줄바꿈 \n으로). 글자 chars개까지(넘으면 chars+1개를 돌려 잘림을 알 수 있게)
std::optional<std::string> read_text(const std::string& path, size_t chars);
// 줄씩(텍스트 모드). fn이 false면 멈춤. 못 열면 false
bool for_each_line(const std::string& path, const std::function<bool(const std::string&)>& fn);
double mtime_of(const std::string& path);  // 없으면 -1
long long size_of(const std::string& path);
std::string abspath(const std::string& path);
std::string env_or(const char* key, const std::string& dflt);

// ---- Unix 소켓 ----
// 연결(비차단, 시간 상한). 실패하면 -1: timed_out 또는 err(errno, 경로가 길면 ENAMETOOLONG)
int connect_unix(const std::string& path, double timeout_s, int& err, bool& timed_out);
enum class Io { Ok, Timeout, Closed, TooLarge, Error };
Io send_all(int fd, const std::string& data, double deadline, int& err);
// 줄 하나(줄바꿈까지 또는 끝까지). 받은 것이 max를 넘으면 TooLarge(Python과 같은 판정)
Io read_line(int fd, std::string& buf, size_t max, double deadline, int& err);

// Manager API 클라이언트(design.md 14.1): 요청 1건 = 연결 1개, 연결·응답 시간 상한, 64 KiB 상한
class ManagerUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ManagerClient {
public:
    ManagerClient(std::string path, double connect_s, double response_s, size_t max_bytes = 65536)
        : path_(std::move(path)), connect_s_(connect_s), response_s_(response_s), max_(max_bytes) {}
    Json call(const Json& request) const;  // 실패하면 ManagerUnavailable
    const std::string& path() const { return path_; }

private:
    std::string path_;
    double connect_s_, response_s_;
    size_t max_;
};

// 워커 명령 소켓(design.md 9.6): JSON 한 줄 보내고 한 줄 받기
struct CommandResult {
    enum Kind { Ok, NoSocket, Timeout, Failed } kind = Failed;
    std::string message;  // Failed일 때 이유
    Json reply;
};
CommandResult worker_command(const std::string& path, const Json& request, double connect_s, double response_s);

}  // namespace gw
