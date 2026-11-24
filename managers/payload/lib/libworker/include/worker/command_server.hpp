// 명령 소켓 서버 (design.md 9.6). Unix SOCK_STREAM, 명령 1건 = 연결 1개, UTF-8 JSON 한 줄.
// 논블로킹이라 느린 연결이 이벤트 루프를 막지 않는다. 스레드 안전하지 않다.
#pragma once

#include <poll.h>

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace worker {

class CommandServer {
public:
    using Clock = std::chrono::steady_clock;
    // 한 줄을 받으면 호출한다. 응답은 reply(conn, ...)로 바로 또는 나중에 보낸다.
    using Handler = std::function<void(int conn, const std::string& line)>;

    explicit CommandServer(Handler handler) : handler_(std::move(handler)) {}
    ~CommandServer();

    // 기존 파일을 지우고 bind, 권한 0600. 폴더가 없으면 만든다.
    bool open(const std::string& path, std::string* error);
    void close();

    // poll()에 넣을 fd 목록과, 그 결과 처리.
    void add_pollfds(std::vector<pollfd>& fds) const;
    void on_poll(const std::vector<pollfd>& fds);
    // 요청 한 줄을 2 s 안에 보내지 않는 연결을 끊는다. 요청을 받은 연결은 응답을 기다리고(모듈이 나중에 답해도 됨),
    // 60 s 안에 응답이 없으면 internal 오류로 답하고 닫는다(design.md 9.6).
    void on_timer(Clock::time_point now);

    // 응답 한 줄을 쓰고 연결을 닫는다. 이미 닫혔으면 아무 일도 하지 않는다.
    void reply(int conn, const std::string& line);
    // 연결한 상대 프로세스 pid(SO_PEERCRED, 커널이 알려 줌). 모르면 0. 명령하는 곳 검사(guard.hpp)에 쓴다
    int peer_pid(int conn) const;

private:
    struct Conn {
        int fd;
        std::string in;
        Clock::time_point opened;
        Clock::time_point requested{};  // 요청 한 줄을 받은 시각
        bool answered = false;          // 요청을 받음(응답할 때까지 더 읽지 않음)
        int pid = 0;                    // 상대 프로세스(SO_PEERCRED)
    };
    void accept_all();
    void read_conn(int id);
    void drop(int id);

    Handler handler_;
    std::string path_;
    int listen_fd_ = -1;
    int next_id_ = 1;
    std::map<int, Conn> conns_;
};

}  // namespace worker
