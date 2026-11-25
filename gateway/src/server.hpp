// 게이트웨이 HTTP (docs/gateway.md 2장). cpp-httplib가 연결을 받고, 요청마다 같은 단계를 지난다:
//   신분 확인 → 권한 → 전달(Manager op · 워커 명령 · 스트림 · 보드 읽기 · 화면) → 기록 → 응답
//
// 스레드:
//   gw.listen   받기(TCP, Unix 소켓마다 하나). 받은 연결을 gw.http 풀에 넣기만 한다(막히지 않음)
//   gw.http ×N  요청 하나씩(SSE면 그 화면이 끝날 때까지). 동시 요청이 threads를 넘으면 503
//               (풀 스레드는 threads + 여분 4: 503을 답할 스레드가 늘 남게)
//   gw.up.<워커> 워커 스트림 구독(hub.hpp)
//   gw.build    제품 빌드 출력 읽기(productbuild.hpp)
//   gw.mgr      Manager 구독 하나로 워커·시스템 상태 캐시(mgr_cache.hpp)
//   gw.log      명령 기록 파일 쓰기(recorder.hpp)
// 신분 확인: WORKSPACE_AUTH=off면 AuthOff(지금 REST 토큰 검사), on·제품 모드면 AuthOn(auth.hpp).
// 게이트웨이가 직접 답하는 경로: /api/auth/login·logout·me, /api/mode(운용/정비 모드), /workspace/settings(설정 탭).
// 연결 하나에 요청 하나(keep-alive 없음, Python REST의 HTTP/1.0과 같음). 읽기·쓰기 상한 5 s.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "auth.hpp"
#include "hub.hpp"
#include "mgr_cache.hpp"
#include "pipeline.hpp"
#include "recorder.hpp"
#include "productbuild.hpp"
#include "reads.hpp"
#include "sys.hpp"
#include "ui.hpp"
#include "worker/thread.hpp"

namespace httplib {
class Server;
}

namespace gw {

struct GatewayConfig {
    std::string data_dir;        // WORKSPACE_DATA(그대로, 비어 있을 수 있음)
    std::string workspace;       // WORKSPACE_ROOT(절대 경로)
    std::string manager_socket;
    std::string bind = "0.0.0.0";
    int port = 8080;
    std::string unix_socket;     // WORKSPACE_GATEWAY_SOCKET(비어 있으면 TCP만)
    bool tcp = true;             // WORKSPACE_GATEWAY_TCP=0이면 TCP를 열지 않음(nginx가 포트를 가질 때, Unix 소켓 필수)
    double connect_timeout = 1.0, response_timeout = 2.0, command_timeout = 3.0;
    size_t threads = 16;         // 동시 요청 상한(SSE 포함)
    std::string token;           // WORKSPACE_REST_TOKEN(신분 확인 꺼짐일 때만)
    bool auth_on = false;        // WORKSPACE_AUTH=on 또는 제품 모드
    std::string users_file;      // 비면 <데이터>/config/users.json
    bool record = true;          // 명령 기록(데이터 폴더가 있을 때)
    bool cache = true;           // Manager 캐시(gw.mgr)
    bool ui_enabled = true, product = false;
    std::string access_mode = "development";  // 시작할 때 정함, 바꾸지 않음(docs/modes.md). 제품이면 늘 operation
    UiConfig ui;
    double io_timeout = 5.0;     // HTTP 읽기·쓰기 상한(느린 클라이언트가 스레드를 붙잡지 못하게)
    double ping_s = 15.0;
    StreamTiming stream;
    std::string python = "python3";
    // 시작할 때의 환경 변수(WORKSPACE_*). 설정 탭의 "다시 시작하면 적용"을 가리는 기준(main이 채움)
    std::map<std::string, std::string> start_env;
};

// 응답 하나
struct Reply {
    enum class Kind { Json, Raw, File, Sse } kind = Kind::Json;
    int status = 200;
    std::string body, ctype;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string file_path;
    std::shared_ptr<SseSession> sse;
};

// 요청 본문(필요할 때만 읽음)
struct Body {
    bool ok = true;
    Json value;          // 비어 있으면 null
    std::string error;
};

// gw.http 풀: 크기 고정, 대기열 상한(넘치면 연결을 닫음)
class Pool {
public:
    Pool(size_t threads, size_t queue_cap);
    ~Pool();
    bool enqueue(std::function<void()> fn);
    // 멈추고 기다린다(상한). 상한 안에 다 끝나면 true
    bool shutdown(double timeout_s);

private:
    void run();
    const size_t cap_;
    std::mutex mu_;  // q_, stop_, active_
    std::condition_variable cv_, idle_cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
    size_t alive_ = 0;
    std::vector<std::thread> threads_;
    bool joined_ = false;
};

class Gateway {
public:
    explicit Gateway(GatewayConfig cfg);
    ~Gateway();
    // 소켓을 열고 스레드를 띄운다. 실패하면 false와 이유
    bool start(std::string& err);
    // 받기를 멈추고 진행 중인 요청을 상한(timeout_s)까지 기다린다. 다 끝나면 true
    bool stop(double timeout_s);
    int port() const { return bound_port_; }
    // 멈춘 스레드 이름(없으면 "")
    std::string stalled();
    void tick();  // 주기 정리(끝난 Upstream 거두기)

    // 요청 단계(시험에서도 부름). body: 본문을 읽는 함수(필요할 때만 부름)
    Reply handle(const RequestView& rq, const std::function<Body()>& body, const std::string& remote);

    // 모드(docs/modes.md): 배포 형태가 정한다. 워크스페이스 development, 제품 operation. 실행 중 바꾸지 않음
    struct Mode {
        std::string mode = "development", since, by;
    };
    Mode mode();

private:
    // 요청 하나의 기록거리(명령 이름, 로그인 이름 등)
    struct Ctx {
        std::string cmd, user;
        Role role = Role::None;
        bool user_set = false;
        std::string from;  // 워커 명령의 보낸 곳: screen 또는 remote:<토큰 이름>(docs/command-path.md 2장)
        bool read = false;  // 읽기 명령(get_status·계약의 read): 기록하지 않음(화면이 주기로 읽어 기록이 묻힘)
    };
    // 워커 명령 종류(계약에서, 10 s 동안 기억). 계약을 못 읽으면 get_status만 읽기
    CommandKinds kinds_of(const std::string& id);
    // 명령 입구(역할 "mission"을 내보내는 워커, managers/mission/design.md 2장). 없으면 빈 글. 10 s 기억
    std::string mission_entry();
    Reply finish_command(const std::string& who, const CommandResult& r);
    bool start_tcp(std::string& err);
    bool start_unix(std::string& err);
    Reply dispatch(const RequestView& rq, const std::function<Body()>& body, const std::string& remote,
                   const Identity& who, Ctx& ctx);
    Reply own(Own o, const RequestView& rq, const std::function<Body()>& body, const std::string& remote,
              const Identity& who, Ctx& ctx);
    void sync_mode(const Json& system);  // gw.mgr: Manager가 다른 모드를 들고 있으면 다시 알림
    Reply forward(const RequestView& rq, const std::function<Body()>& body, Ctx& ctx);
    Reply manager_op(const Route& r, const std::function<Body()>& body);
    Reply manager_reply(const Json& resp, int ok_status);
    Reply health(const std::string& query);
    Reply command(const std::string& id, const std::function<Body()>& body, Ctx& ctx);
    Reply stream(const std::string& id, const std::string& query);
    Reply product_build(const std::string& method, const std::string& path, const std::string& sub,
                        const std::string& query, const std::function<Body()>& body);
    Reply read(const Route& r, const std::string& query);
    // 워크스페이스 설정(ui/workspace-screen.md 2장): GET 정의+값, PUT 검사 → workspace.env 그 줄만 → live 칸 바로 적용
    Reply settings(const std::string& method, const std::function<Body()>& body, Ctx& ctx);
    std::string settings_env_path() const;
    std::string settings_def_path() const;
    int open_stream(const std::string& worker_id, std::string& why);
    void setup(httplib::Server& svr, int watch_slot, bool via_unix);

    GatewayConfig cfg_;
    ManagerClient mgr_;
    Reads reads_;
    Ui ui_;
    ProductBuild build_;
    Hub hub_;
    std::unique_ptr<Auth> auth_;
    AuthOn* auth_on_ = nullptr;           // auth_가 AuthOn이면 그것(로그인·세션)
    std::unique_ptr<Recorder> recorder_;
    FileRecorder* file_rec_ = nullptr;    // recorder_가 FileRecorder면 그것(지표)
    ManagerCache cache_;
    std::mutex mode_mu_;  // mode_ (I/O 없음)
    std::mutex kinds_mu_;  // kinds_ (I/O 없음: Manager 호출은 잠금 밖)
    std::map<std::string, std::pair<double, CommandKinds>> kinds_;
    std::pair<double, std::string> mission_ = {-1e9, ""};  // kinds_mu_로 보호
    Mode mode_;
    double last_mode_push_ = -1e9;  // gw.mgr만
    std::atomic<double> command_timeout_;  // 워커 명령 응답 대기(설정 탭에서 바로 바뀜)
    std::mutex settings_mu_;  // workspace.env 읽고-고치고-쓰기를 하나씩(파일 I/O를 잠금 안에서: 이 잠금은 PUT만 기다림)

    worker::ThreadWatch watch_;
    Pool pool_;
    std::atomic<size_t> in_flight_{0};
    std::atomic<bool> stopping_{false};
    std::unique_ptr<httplib::Server> tcp_, unix_;
    std::vector<std::thread> listeners_;
    int bound_port_ = 0;
};

}  // namespace gw
