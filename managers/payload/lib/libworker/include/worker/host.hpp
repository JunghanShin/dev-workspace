// 워커 호스트와 모듈 (워커 모듈 구조, 워크스페이스 design.md 6.6.3).
//
// 워커 = 호스트(이 파일, 공통) + 모듈 여러 개. 호스트가 워커 계약을 전부 맡고, 모듈은 자기 일만 한다.
//
//   호스트: 실행 인자(--config), 설정 읽기와 모듈별 칸 나눠 주기, 자체 워치독, 스레드 감시(ThreadWatch),
//           heartbeat(READY·WATCHDOG, 지표 모으기), 명령 소켓(명령 배달, get_status 모으기), 종료 절차
//   모듈:   설정 칸 하나, 명령 "<모듈>.<명령>", 지표 "<모듈>_<이름>", 스레드 "<모듈>.<역할>"
//
// 워커의 main은 모듈을 만들어 잇기만 한다:
//
//     int main(int argc, char** argv) {
//         worker::Host host("siyi-worker");
//         auto& link = host.add<SiyiLink>();
//         host.add<Motion>(link);                    // 모듈끼리는 포트(작은 인터페이스)로만 잇는다
//         host.add<video::StreamsModule>();
//         host.alias("gimbal_angle", "motion.move_to");  // 예전 명령 이름(바꾸는 동안)
//         return host.run(argc, argv);
//     }
//
// 설정 파일(Manager가 넘기는 것과 같은 형식): {"id": ..., "config_version": n, "config": {...}}
//   config 맨 위: 호스트 키(watchdog_sec, stop_timeout_sec, thread_stall_sec, command_socket, stream_socket)와
//   모듈 이름 칸({"<모듈>": {...}}). 그 밖의 키는 설정 오류(exit 1)
//
// 규칙: 모듈의 on_*·handle은 짧게 끝낸다(호스트 루프 하나를 같이 쓴다). 기다리거나 무거운 일은 모듈이
// start()에서 WorkerThread(host.watch())와 BoundedQueue로 따로 돌리거나, 제공 도구 host.input/host.output(입력·출력
// 단계, worker/stage.hpp)을 쓴다. 스레드 이름은 "<모듈>.<역할>"(15자까지). 실행 구조는 개발자가 정한다(design.md 6.6.9).
//
// 기본 안전망(항상 켜짐, design.md 6.6.9 ②): 모듈의 모든 진입점 예외를 호스트가 잡는다(명령 하나는 오류 응답,
// on_timer·on_poll이 연달아 3번이면 safe_state() 뒤 exit 4, worker::Fatal은 바로), 루프 한 번 시간 지표 host_loop_ms,
// 스레드가 멈추면 safe_state() 뒤 exit 4.
#pragma once

#include <poll.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "worker/config.hpp"
#include "worker/guard.hpp"
#include "worker/inputs.hpp"
#include "worker/stage.hpp"
#include "worker/thread.hpp"

namespace worker {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
// 명령 응답 한 줄: {"ok": true, "data": ...} 또는 {"ok": false, "error": {"code", "message"}}
using Reply = std::function<void(const Json&)>;

Json ok_reply(Json data = Json::object());
Json error_reply(const std::string& code, const std::string& message);

// heartbeat 지표. set("fps", 30) → "<모듈>_fps=30". 이미 "<모듈>_"로 시작하는 키는 그대로
class Metrics {
public:
    explicit Metrics(std::string prefix) : prefix_(std::move(prefix)) {}
    void set(const std::string& key, double value, int digits = -1);
    void set(const std::string& key, long long value);
    void set(const std::string& key, int value) { set(key, static_cast<long long>(value)); }
    void set(const std::string& key, uint64_t value) { set(key, static_cast<long long>(value)); }
    void set(const std::string& key, bool value) { set(key, static_cast<long long>(value)); }
    // 이미 글자로 만든 값(예: 소수 자리를 정한 값)
    void set_text(const std::string& key, const std::string& value);
    // 이미 "key=value ..." 꼴로 만든 지표(옮기는 동안). 앞에 접두어를 붙이지 않는다
    void raw(const std::string& text);
    const std::string& str() const { return out_; }

private:
    std::string key(const std::string& k) const;
    std::string prefix_;
    std::string out_;
};

class Host;

class Module {
public:
    virtual ~Module() = default;
    // 모듈 이름: 설정 칸·명령·지표·스레드 이름 앞부분. 소문자·숫자·밑줄
    virtual std::string name() const = 0;
    // 자기 설정 칸(없으면 빈 객체). 잘못되면 ConfigError(호스트가 exit 1)
    virtual void configure(const Json& config) { (void)config; }
    // 실행 중 설정이 바뀌었을 때(SIGHUP). 다시 시작하지 않고 반영했으면 true. false(기본)면 Manager가 다시 시작한다.
    // 반영할 수 없는 키가 바뀌었으면 아무것도 바꾸지 말고 false. 잘못된 값이면 worker::ConfigError
    virtual bool reconfigure(const Json& config) { (void)config; return false; }
    // 소켓·스레드 열기. 실패하면 예외(호스트가 exit 3). 스레드는 host.watch()로 만든다
    virtual void start(Host& host) { (void)host; }
    virtual void add_pollfds(std::vector<pollfd>& fds) { (void)fds; }
    virtual void on_poll(const std::vector<pollfd>& fds, TimePoint now) { (void)fds; (void)now; }
    virtual void on_timer(TimePoint now) { (void)now; }
    virtual TimePoint next_wakeup(TimePoint now) const { return now + std::chrono::seconds(1); }
    // 명령(앞의 "<모듈>."을 뗀 이름). 처리하면 true이고 reply를 정확히 한 번(나중에라도) 부른다
    virtual bool handle(const std::string& cmd, const Json& args, Reply reply) {
        (void)cmd; (void)args; (void)reply;
        return false;
    }
    // get_status의 "<모듈>" 칸
    virtual Json status() const { return Json::object(); }
    virtual void metrics(Metrics& m) const { (void)m; }
    // 처리량 카운터(heartbeat progress에 더한다). 늘지 않으면 Manager가 멈춤으로 볼 수 있다
    virtual uint64_t progress() const { return 0; }
    // 되돌릴 수 없는 실패(스레드 예외 등). 참이면 호스트가 exit 4로 끝내고 Manager가 다시 띄운다
    virtual bool failed(std::string* why) const { (void)why; return false; }
    // 안전 상태(기본 안전망): 정상 종료와 스스로 끝내는 경로(exit 4, 스레드 멈춤)에서 stop 전에 한 번 부른다.
    // 장치를 안전하게 둘 기회(정지 명령 등). 짧게. 기본은 아무것도 안 함
    virtual void safe_state() {}
    virtual void stop() {}

protected:
    // 이 모듈이 붙은 호스트. configure·start 안에서 쓸 수 있다(예: host()->id()로 워커 id). 호스트 없이 시험하면 null
    Host* host() const { return host_; }

private:
    friend class Host;
    Host* host_ = nullptr;
};

class Host {
public:
    explicit Host(std::string program);  // 로그·사용법에 쓰는 실행 파일 이름(예: "example-worker")
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // 모듈을 만들어 붙인다(붙인 순서로 시작, 반대 순서로 정지). 돌려준 참조로 다른 모듈에 포트를 넘긴다
    template <class M, class... Args>
    M& add(Args&&... args) {
        auto m = std::make_unique<M>(std::forward<Args>(args)...);
        M& ref = *m;
        static_cast<Module&>(ref).host_ = this;
        modules_.push_back(std::move(m));
        return ref;
    }
    // 예전 명령 이름 → 새 이름("<모듈>.<명령>")
    void alias(const std::string& old_cmd, const std::string& new_cmd) { aliases_[old_cmd] = new_cmd; }
    // 예전(모듈 칸 이전) 설정 키: config 맨 위에 있으면 그 모듈 칸으로 옮겨 읽는다(칸에 같은 키가 있으면 칸이 이김).
    // 옮겼으면 경고를 남긴다. 설정을 새 형식으로 바꾸는 동안만 쓴다
    // thread_stall_sec 기본값(설정에 없을 때). run 전에 부른다(예: 장치 열기가 몇 초 걸리는 워커)
    void thread_stall_default(double sec) { thread_stall_sec_ = sec; }
    void legacy_keys(const std::string& module, const std::vector<std::string>& keys) {
        for (const auto& k : keys) legacy_[k] = module;
    }

    // ---- 제공 도구: 입력·출력 단계(worker/stage.hpp, design.md 6.6.9 ③). 모듈의 start() 안에서 만든다 ----
    // 설정 "stages"의 같은 이름 칸이 defaults를 덮어쓴다. 만든 즉시 스레드가 돈다. 모듈이 stop되기 전에 호스트가 정지한다
    template <class T>
    Input<T>& input(const std::string& name, typename Input<T>::Reader read, typename Input<T>::Handler handle,
                    StageOptions defaults = {}) {
        auto st = std::make_unique<Input<T>>(name, stage_options(name, defaults), *watch_, std::move(read), std::move(handle));
        Input<T>& ref = *st;
        add_stage(std::move(st));
        return ref;
    }
    template <class T>
    Output<T>& output(const std::string& name, typename Output<T>::Writer write, StageOptions defaults = output_defaults()) {
        auto st = std::make_unique<Output<T>>(name, stage_options(name, defaults), *watch_, std::move(write));
        Output<T>& ref = *st;
        add_stage(std::move(st));
        return ref;
    }
    static StageOptions output_defaults() {
        StageOptions o;
        o.queue = 256;
        o.overflow = Overflow::Reject;
        return o;
    }

    int run(int argc, char** argv);
    // 설정 파일 읽기와 모듈별 칸 나눠 주기(run이 부른다. 시험에서 직접 불러도 된다). 실패하면 false와 error
    bool configure_file(const std::string& path, std::string* error);
    // 실행 중 설정 다시 읽기(SIGHUP이 오면 run이 부른다). 바뀐 모듈마다 reconfigure. 모두 반영하면 config_version을
    // 새 버전으로(heartbeat가 Manager에 알림) 하고 true. 하나라도 못 하면 아무것도 바꾸지 않은 버전 그대로 false
    // (Manager가 확인을 못 받아 다시 시작한다). 호스트 키가 바뀌었으면 false
    bool reload_file(const std::string& path, std::string* error);
    static const char* libworker_version();

    // start() 안에서 쓴다
    ThreadWatch& watch() { return *watch_; }
    double thread_stall_sec() const { return thread_stall_sec_; }
    const std::string& program() const { return program_; }
    const std::string& id() const { return id_; }
    long config_version() const { return config_version_; }
    const std::string& command_socket() const { return command_socket_; }
    const std::string& stream_socket() const { return stream_socket_; }  // 설정 또는 기본 경로
    double watchdog_sec() const { return watchdog_sec_; }
    // 가짜 장치 모드(설정 맨 위 "sim", design.md 6.6.8). 장치 모듈은 이것으로 실기와 다르게 할 수 있다(보통은 설정만 다름)
    bool sim() const { return sim_; }

    // 명령 한 줄 처리(명령 소켓이 부른다. 시험에서 직접 불러도 된다). peer_pid: 보낸 프로세스(명령하는 곳 검사,
    // 워크스페이스 docs/command-path.md 5장 ②. 0이면 모름). 읽기가 아닌 명령은 status()의 last_command에 남는다
    void dispatch(const std::string& line, Reply reply, int peer_pid = 0);
    // 명령하는 곳 검사(시험·run이 씀). path가 비면 검사하지 않음(단독 실행)
    void set_peers_file(const std::string& path);

    // 입력 주소(워크스페이스 design.md 6.6.7.3·6.6.7.5, inputs.hpp). 없거나 이어지지 않은 입력이면 nullptr.
    // configure 뒤(start()에서) 쓴다. 모듈은 붙었는지를 input_link로 알린다(지표 inputs_<입력>_link, get_status "inputs")
    const InputAddress* uses(const std::string& name) const;
    // 지금 처리 중인 명령을 보낸 곳(검사를 거친 from: screen, remote:<이름>, 워커 id). 모듈 handle() 안에서만, 밖에서는 빈 글
    // (libworker 0.14, 명령 입구 매니저가 넘길 때 origin으로 씀)
    const std::string& current_from() const { return current_from_; }

    // ---- 명령 임대(워크스페이스 docs/command-lease.md, libworker 0.15) ----
    // 계약(provides 역할 정의)의 명령 "lease_ms"·역할 "safe_state"로 임대 표를 만든다. run이 시작할 때 내장 io로 부른다.
    // 시험에서는 provides 객체({"<역할>": 정의})를 직접 넘겨도 된다
    void load_leases(const Json& provides);
    // 기한이 지난 임대: 그 역할의 safe_state 명령을 모듈에 넣고 이벤트를 쌓는다(run 루프가 부름, 시험에서도)
    void lease_tick(TimePoint now);
    // 다음 임대 기한(없으면 TimePoint::max())
    TimePoint next_lease_deadline() const;
    // 호스트가 낸 이벤트(공통 이벤트 형식, 예: lease_expired)를 꺼낸다. 스트림을 내는 모듈이 자기 이벤트에 붙여 발행
    std::vector<Json> take_events();
    void input_link(const std::string& name, bool up);
    const std::map<std::string, InputAddress>& inputs() const { return inputs_; }
    Json status() const;
    std::string metrics() const;

private:
    std::string current_from_;  // current_from()
    struct LeaseDef {
        std::string role;
        int ms = 0;
    };
    struct Lease {
        std::string cmd, from, origin;
        TimePoint deadline{};
    };
    void route(const std::string& cmd, const Json& args, Reply reply);
    void push_event(const std::string& severity, const std::string& code, const std::string& text, const Json& detail);
    std::map<std::string, LeaseDef> lease_cmds_;  // 명령 → (역할, lease_ms)
    std::map<std::string, Json> safe_states_;     // 역할 → {"cmd", "args"}
    std::map<std::string, Lease> leases_;         // 역할 → 지금 임대
    uint64_t lease_expired_ = 0;
    std::vector<Json> host_events_;
    uint64_t host_event_seq_ = 0;
    Json read_config(const std::string& path, std::string* id, long* version);
    Json host_keys(const Json& cfg) const;
    StageOptions stage_options(const std::string& name, StageOptions defaults) const;
    void add_stage(std::unique_ptr<Stage> st);
    void stop_stages_of(size_t module);
    // 기본 안전망: 모듈 진입점을 부르고 예외를 잡는다. 끝내야 하면 fatal_에 이유
    template <class F>
    bool guard(size_t module, const char* where, bool counts_toward_exit, F&& f);
    void note_error(const std::string& what);
    void call_safe_state();
    Json runtime_status() const;
    std::string host_metrics();
    bool fault(const Json& args, std::string* error);

    std::string program_;
    std::string config_path_;
    std::map<std::string, Json> sections_;  // 지금 적용된 모듈 칸
    Json host_cfg_;                         // 지금 적용된 호스트 키
    std::vector<std::unique_ptr<Module>> modules_;
    std::map<std::string, std::string> aliases_;
    std::map<std::string, std::string> legacy_;
    std::unique_ptr<ThreadWatch> watch_;
    std::string id_;
    long config_version_ = 0;
    // 명령 길: 허용 목록 검사, 마지막 명령, 위반 기록(같은 곳·명령은 60 s에 한 번)
    std::unique_ptr<CommandGuard> cmd_guard_;
    std::map<std::string, InputAddress> inputs_;   // 입력 주소(설정 inputs + 워크스페이스 파일)
    std::map<std::string, bool> input_up_;  // 모듈이 알린 연결 상태
    Json last_command_;
    std::map<std::string, TimePoint> guard_logged_;
    std::string command_socket_, stream_socket_;
    double watchdog_sec_ = 3, stop_timeout_sec_ = 5, thread_stall_sec_ = 2;
    // 가짜 장치 모드: "sim": {"cmd": "<프로그램> <인자>"} → 호스트가 자식으로 띄우고 죽으면 다시 띄움
    bool sim_ = false;
    std::vector<std::string> sim_cmd_;
    pid_t sim_pid_ = -1;
    int sim_restarts_ = 0;
    void sim_tick(TimePoint now);
    TimePoint sim_next_start_{};

    // 제공 도구: 단계
    Json stages_cfg_ = Json::object();
    int loop_budget_ms_ = 50;
    std::vector<std::unique_ptr<Stage>> stages_;
    std::vector<size_t> stage_owner_;  // stages_[i]를 만든 모듈 번호
    size_t starting_module_ = 0;
    // 기본 안전망
    std::string fatal_;                      // 비어 있지 않으면 루프를 끝내고 exit 4
    std::map<std::string, int> consecutive_errors_;  // "<모듈>.on_timer"·"<모듈>.on_poll"마다 연속 예외
    uint64_t host_errors_ = 0, budget_overruns_ = 0;
    int64_t loop_last_us_ = 0, loop_max_us_ = 0, loop_report_max_us_ = 0;
    std::vector<std::string> warnings_;      // 최근 경고(10개)
    TimePoint last_loop_warn_{}, stall_since_{};
    bool safe_state_done_ = false;
    // 장애 주입(시험 빌드: 환경 WORKER_FAULT_INJECTION=1)
    bool fault_enabled_ = false;
    int fault_throw_timer_ = 0, fault_throw_poll_ = 0;
    bool fault_fatal_ = false;
    size_t fault_module_ = 0;
};

// 이 실행 파일이 있는 폴더(/proc/self/exe 기준). 설정의 "@EXEC_DIR@"을 이것으로 바꾼다(가짜 장치 프로그램·라이브러리 경로)
std::string exec_dir();
// s 안의 "@EXEC_DIR@"을 exec_dir()로 바꾼 것
std::string expand_exec_dir(const std::string& s);

// 시험용: 모듈에 명령 하나를 보내고 그 자리에서 온 응답을 돌려준다(비동기 응답이면 null)
Json call_module(Module& m, const std::string& cmd, const Json& args = Json::object());

}  // namespace worker
