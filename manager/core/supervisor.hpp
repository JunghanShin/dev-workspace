// 워커 생명주기: 등록·기동·정지·감시·종료 판정·재기동 (design.md 6.3). Python supervisor.py를 옮김.
//
// 상태는 이 객체 하나가 가진다(제어 루프 스레드에서만 부름). 시계·spawn·파일 쓰기·파일 읽기는 포트로 받아
// 실제 프로세스 없이 시험한다(test/test_core.cpp).
//
// Python 판과 다른 점(밖에서는 같게 보이도록):
//   - 파일 쓰기는 pm.store 스레드가 한다. 그래서 쓰기 명령은 둘로 나눈다:
//       plan_*()  검사하고 쓸 파일(OpPlan.pre)과 "써진 뒤 할 일"(commit)을 돌려준다. 틀리면 ApiError
//       commit()  파일이 써진 뒤 루프에서 상태를 바꾸고 응답 data를 돌려준다
//   - 워커 설정 파일(state/workers/<id>.json)도 pm.store가 쓰고, 다 써지면 spawn·SIGHUP 한다
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "json_util.hpp"
#include "ports.hpp"
#include "worker_files.hpp"

namespace pm {

struct ApiError {
    std::string code;
    std::string message;
    Json fields;  // 설정 검사 오류 {경로: 메시지}, 없으면 null
};

struct OpPlan {
    std::vector<FileOp> pre;      // 워커 폴더 파일(먼저 씀)
    std::function<Json()> commit; // 파일이 써진 뒤 루프에서: 상태를 바꾸고 응답 data
};

// 데이터 폴더 안 실행 기록 위치(Python store.py)
struct StatePaths {
    std::string state_dir, run_dir, workers_file;
    std::string workers_dir() const { return state_dir + "/workers"; }
    std::string logs_dir() const { return state_dir + "/logs"; }
    std::string versions_path() const { return state_dir + "/versions.json"; }
    std::string runtime_path() const { return run_dir + "/desired.json"; }
    std::string temp_path() const { return run_dir + "/temp_config.json"; }
    std::string worker_config(const std::string& id) const { return workers_dir() + "/" + id + ".json"; }
    std::string log_path(const std::string& id) const { return logs_dir() + "/" + id + ".log"; }
};

struct SupervisorConfig {
    Json settings;                         // 기본값 + manager.json (settings.py DEFAULTS)
    Json types;                            // type 이름 → 선언(+ _worker_id, _conf_dir, _requires_met)
    StatePaths paths;
    std::string notify_path;
    std::vector<std::string> base_env;     // Manager 환경("K=V"), 워커가 물려받음
    bool product = false;                  // WORKSPACE_PRODUCT=1
    std::string workspace_root;            // get_io의 공통 정의 위치
    std::string boot_id;                   // 비어 있으면 null
    std::set<std::string> manager_api_workers;
    std::string peers_file;                // 허용 목록(docs/command-path.md 4.2). 워커에 WORKSPACE_PEERS_FILE로
};

struct Worker {
    // 기록(Python rec)
    std::string id, type;
    bool autostart = false;
    Json preset;                 // 사용 중 프리셋 이름(설정 파일 오류면 null)
    Json saved = Json::object(); // 저장된 값
    Json config = Json::object();// 지금 값
    bool temp = false;           // 저장하지 않고 적용한 값으로 도는 중
    std::string desired_state = "stopped";
    long long config_version = 0;

    std::unique_ptr<WorkerFiles> files;
    bool file_error = false;
    Json libworker;              // READY의 libworker 버전
    bool sim = false;
    Json peers = Json::object();
    Json exec_path;
    int pid = 0;                 // 0 = 프로세스 없음
    std::string actual = "stopped";
    Json last_exit_code, last_signal, last_error;
    int restart_count = 0;
    int fail_streak = 0;
    std::vector<double> fail_times;
    bool crash_loop = false;
    Json process;                // 적용을 요청한 우선순위·CPU
    Json started_at;
    std::optional<double> spawn_mono, last_hb_mono;
    Json progress;
    double next_start_mono = 0.0;
    std::optional<long long> applied_version, live_version;
    std::optional<double> live_deadline;
    std::optional<std::string> stop_reason;
    double term_mono = 0.0;
    bool killed = false;
    Json metrics, resources;
    bool spawning = false;       // 설정 파일 쓰기를 기다리는 중(이 동안 다시 띄우지 않음)

    bool has_proc() const { return pid > 0; }
};

class Supervisor {
public:
    static constexpr double kLiveAckS = 5.0;    // SIGHUP 뒤 워커가 config_version으로 확인해야 하는 시간
    static constexpr double kEarlyKeepS = 10.0; // 짝 없는 heartbeat 보관 시간
    static constexpr size_t kEarlyMax = 64;     // 보관 pid 수 상한
    static constexpr int kExitConfig = 1;

    using Listener = std::function<void(const std::string& event, const std::string& key, const Json& data)>;

    Supervisor(SupervisorConfig cfg, Clock& clock, Spawner& spawner, Store& store, FileSystem& fs);

    // 워커 폴더를 보고 워커를 만든다(6.3.1). 같은 부팅이면 직전 실행 상태·임시 설정 복원
    void load();
    Listener listener;  // worker_status 이벤트(구독자에게)

    // ---- 조회(ApiError) ----
    Json list();
    Json get(const Json& id);
    Json get_config(const Json& id);
    Json get_schema(const Json& id);
    Json get_io(const Json& id);

    // ---- 쓰기(ApiError, 둘로 나눔) ----
    OpPlan set_config(const Json& id, const Json& config, bool save);
    OpPlan use_preset(const Json& id, const Json& name);
    OpPlan save_preset(const Json& id, const Json& spec);
    OpPlan rename_preset(const Json& id, const Json& name, const Json& spec);
    OpPlan delete_preset(const Json& id, const Json& name);
    OpPlan set_autostart(const Json& id, const Json& value);
    OpPlan start(const Json& id);
    OpPlan stop(const Json& id);

    // ---- 루프 ----
    void on_notify(int pid, const Json& fields);  // fields: {"READY": "1", ...}
    void tick();
    void on_exit(int pid, int rc);                // rc < 0: 신호 번호의 음수
    void apply_resources(const std::map<int, Json>& by_pgid);
    std::vector<int> running_pids() const;
    std::map<std::string, int> pids_by_id() const;  // 떠 있는 워커 id → pid(= 프로세스 그룹)
    std::vector<std::string> ids() const;           // 등록된 워커 id(순서대로)

    // ---- 끝내기(desired_state는 그대로 두어 다시 시작하면 복원) ----
    double shutdown_begin();  // 모두 SIGTERM, 기다릴 최대 시간(초)
    void shutdown_kill();
    bool any_running() const;

    // ---- 시험용 ----
    Worker* worker(const std::string& id);
    size_t early_count() const { return early_.size(); }
    bool has_early(int pid) const { return early_.count(pid) > 0; }

private:
    Json status(const Worker& w, double now) const;
    void emit(Worker& w);
    Worker& lookup(const Json& id);
    Worker& valid(const Json& id);
    const Json& type_of(const Worker& w) const;
    double tnum(const Json& t, const char* key, double dflt) const;
    double rnum(const char* key) const;
    Json config_errors(const std::string& type, const Json& config) const;
    void check_schema(const std::string& type, const Json& config) const;
    void check_device_conflict(const std::string& id, const Json& config) const;
    void change_config(Worker& w, const Json& config, const Json* saved, bool temp);
    void try_live(Worker& w, const Json& old);
    void save_versions();
    void save_runtime();
    void submit_quiet(std::vector<FileOp> ops, const char* what);
    Json payload(const Worker& w) const;
    FileOp worker_config_op(const Worker& w) const;

    void keep_early(int pid, const Json& fields);
    void drain_early(Worker& w);
    void check_alive(Worker& w, const Json& t, double now);
    void check_idle(Worker& w, const Json& t, double now);
    void spawn(Worker& w, const Json& t);
    void do_spawn(Worker& w, const Json& t);
    void send_signal(Worker& w, int sig);
    void terminate(Worker& w, const std::string& reason);
    void on_failure(Worker& w, double now);
    Json load_boot_file(const std::string& path);
    Json load_json_file(const std::string& path, bool log_invalid);

    SupervisorConfig cfg_;
    Clock& clock_;
    Spawner& spawner_;
    Store& store_;
    FileSystem& fs_;
    std::vector<std::unique_ptr<Worker>> workers_;  // 워커 id 순(Python dict 순서)
    std::map<std::string, Worker*> by_id_;
    std::set<std::string> builtin_types_;
    // 아직 어느 워커의 것인지 모르는 heartbeat(pid → [(받은 시각, 칸)]). 9.5
    std::map<int, std::vector<std::pair<double, Json>>> early_;
};

// "a/b/../c" → "a/c" (Python os.path.normpath)
std::string normpath(const std::string& path);
// 신호 번호 → "SIGTERM" (모르면 숫자)
std::string signal_name(int sig);

}  // namespace pm
