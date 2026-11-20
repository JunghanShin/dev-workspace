// core 단위 시험: 설정 스키마 검사(test_schema.py와 같은 경우), heartbeat가 spawn보다 먼저 올 때
// (test_notify_race.py), Supervisor 규칙(재시작 간격·연속 실패·멈춤 판정·설정 버전·live_keys·복원).
// 실제 프로세스 없이 가짜 시계·가짜 spawn·가짜 store로. 워커 폴더 파일만 임시 폴더에 진짜로 쓴다.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "json_util.hpp"
#include "posix_fs.hpp"
#include "schema.hpp"
#include "supervisor.hpp"

using pm::Json;

namespace {

int g_fail = 0, g_checks = 0;
std::string g_test;

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        ++g_checks;                                                                              \
        if (!(cond)) {                                                                           \
            ++g_fail;                                                                            \
            std::cerr << "FAIL " << g_test << " " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        }                                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        ++g_checks;                                                                                        \
        auto va = (a);                                                                                     \
        auto vb = (b);                                                                                     \
        if (!(va == vb)) {                                                                                 \
            ++g_fail;                                                                                      \
            std::cerr << "FAIL " << g_test << " " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b "\n"; \
        }                                                                                                  \
    } while (0)

std::vector<std::pair<std::string, std::function<void()>>>& tests() {
    static std::vector<std::pair<std::string, std::function<void()>>> t;
    return t;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { tests().emplace_back(n, std::move(f)); }
};
#define TEST(name)                         \
    static void name();                    \
    static Reg reg_##name(#name, name);    \
    static void name()

Json J(const char* s) { return Json::parse(s); }

std::set<std::string> keys(const Json& o) {
    std::set<std::string> out;
    for (auto it = o.begin(); it != o.end(); ++it) out.insert(it.key());
    return out;
}

std::string repo() { return PM_REPO_ROOT; }

Json load(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return Json::parse(ss.str());
}

void write_file(const std::string& path, const std::string& text) {
    pm::mkdir_p(path.substr(0, path.rfind('/')));
    std::ofstream(path) << text;
}

bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

std::vector<std::string> list_dir(const std::string& d) {
    std::vector<std::string> out;
    if (DIR* dd = ::opendir(d.c_str())) {
        while (dirent* e = ::readdir(dd))
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        ::closedir(dd);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- 가짜 포트 ----------------------------------------------------------------------------------

struct FakeClock : pm::Clock {
    double t = 1000.0;
    double mono() override { return t; }
    std::string now_iso() override { return "2026-11-20T10:00:00+09:00"; }
};

struct FakeSpawner : pm::Spawner {
    int next_pid = 100;
    bool fail = false;
    std::vector<pm::SpawnSpec> spawned;
    std::vector<std::pair<int, int>> signals, group_kills;
    int spawn(const pm::SpawnSpec& spec, std::string& err) override {
        if (fail) {
            err = "[Errno 2] No such file or directory: 'x'";
            return -1;
        }
        spawned.push_back(spec);
        return next_pid++;
    }
    void signal(int pid, int sig) override { signals.emplace_back(pid, sig); }
    void kill_group(int pgid, int sig) override { group_kills.emplace_back(pgid, sig); }
    bool executable(const std::string& p) override { return p.find("missing") == std::string::npos; }
    int count(int sig) const {
        int n = 0;
        for (auto& s : signals) n += s.second == sig;
        return n;
    }
};

// 요청을 모아 두었다가 flush()에서 진짜로 쓰고 done을 부른다(루프의 pm.store 결과 처리와 같은 차례)
struct FakeStore : pm::Store {
    std::vector<std::pair<std::vector<pm::FileOp>, pm::StoreDone>> q;
    bool fail = false;
    void submit(std::vector<pm::FileOp> ops, pm::StoreDone done) override { q.emplace_back(std::move(ops), std::move(done)); }
    void flush() {
        while (!q.empty()) {
            auto items = std::move(q);
            q.clear();
            for (auto& [ops, done] : items) {
                std::string err;
                const bool ok = !fail && pm::run_file_ops(ops, err);
                if (done) done(ok, fail ? "disk full" : err);
            }
        }
    }
};

std::string tmpdir() {
    char tmpl[] = "/tmp/pm-core-XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    return d ? d : "/tmp";
}

struct Rig {
    std::string dir;
    FakeClock clock;
    FakeSpawner sp;
    FakeStore store;
    pm::PosixFs fs;
    std::unique_ptr<pm::Supervisor> sup;
    std::vector<Json> events;

    // workers: {이름: {"decl": {...}, "config": {...}|null, "autostart": bool}}
    Rig(const Json& workers, const Json& restart = Json::object(), bool product = false,
        const std::string& boot_id = "boot-1", std::function<void(const std::string&)> before = nullptr) {
        dir = tmpdir();
        Json types = Json::object();
        for (auto it = workers.begin(); it != workers.end(); ++it) {
            const std::string ws = dir + "/ws/" + it.key();
            pm::mkdir_p(ws);
            if (it->contains("autostart"))
                write_file(ws + "/config.json", "{\"active\": \"default\", \"autostart\": " +
                                                    std::string((*it)["autostart"].get<bool>() ? "true" : "false") + "}");
            if (it->contains("config") && !(*it)["config"].is_null()) {
                Json p = Json::object();
                p["label"] = "기본";
                p["config"] = (*it)["config"];
                write_file(ws + "/presets/default.json", p.dump());
            }
            Json t = J(R"({"exec": ["/bin/w"], "start_timeout_sec": 10, "watchdog_sec": 3, "stop_timeout_sec": 5})");
            if (it->contains("decl")) t.update((*it)["decl"]);
            t["_worker_id"] = it.key();
            t["_conf_dir"] = ws;
            types[it.key() + "_t"] = t;
        }
        if (before) before(dir);
        pm::SupervisorConfig c;
        c.settings = J(R"({"restart": {"interval_s": 2.0, "max_interval_s": 30.0, "healthy_s": 60.0,
                                       "crash_window_s": 300.0, "crash_count": 5}})");
        c.settings["restart"].update(restart);
        c.types = types;
        c.paths = pm::StatePaths{dir + "/data/state", dir + "/data/run", dir + "/data/config/workers.json"};
        c.notify_path = dir + "/data/run/notify.sock";
        c.base_env = {"PATH=/usr/bin", "WORKSPACE_MANAGER_SOCKET=/old"};
        c.product = product;
        c.workspace_root = dir;
        c.boot_id = boot_id;
        sup = std::make_unique<pm::Supervisor>(c, clock, sp, store, fs);
        sup->listener = [this](const std::string&, const std::string&, const Json& d) { events.push_back(d); };
        sup->load();
        store.flush();
    }
    ~Rig() {
        const std::string cmd = "rm -rf '" + dir + "'";
        if (std::system(cmd.c_str()) != 0) std::cerr << "cleanup failed\n";
    }
    pm::Worker& w(const std::string& id) { return *sup->worker(id); }
    Json st(const std::string& id) { return sup->get(Json(id)); }
    // 쓰기 op 한 건: 파일 → commit → 실행 기록 쓰기(루프와 같은 차례)
    Json run(pm::OpPlan p) {
        std::string err;
        if (!p.pre.empty() && !pm::run_file_ops(p.pre, err)) throw std::runtime_error(err);
        Json d = p.commit();
        store.flush();
        return d;
    }
    // tick → 설정 파일 쓰기 → spawn
    void tick() {
        sup->tick();
        store.flush();
    }
    int start(const std::string& id) {
        run(sup->start(Json(id)));
        tick();
        return w(id).pid;
    }
    void ready(int pid) { sup->on_notify(pid, J(R"({"READY": "1", "libworker": "0.9.1"})")); }
    void beat(int pid, const std::string& extra = "") {
        Json f = J(R"({"WATCHDOG": "1", "progress": "5"})");
        if (!extra.empty()) f.update(J(extra.c_str()));
        sup->on_notify(pid, f);
    }
};

pm::ApiError api_error_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const pm::ApiError& e) {
        return e;
    }
    return pm::ApiError{"", "", nullptr};
}

// ---- JSON 도우미 --------------------------------------------------------------------------------

TEST(json_python_semantics) {
    CHECK(pm::py_eq(J(R"({"a": 1, "b": [1, 2.0]})"), J(R"({"b": [1.0, 2], "a": 1.0})")));
    CHECK(pm::py_eq(J("true"), J("1")));
    CHECK(!pm::py_eq(J(R"({"a": 1})"), J(R"({"a": 1, "b": null})")));
    CHECK(!pm::py_eq(J(R"("1")"), J("1")));
    CHECK_EQ(pm::py_repr(Json("nope")), std::string("'nope'"));
    CHECK_EQ(pm::py_repr(Json("it's")), std::string("\"it's\""));
    CHECK_EQ(pm::py_repr(Json(nullptr)), std::string("None"));
    CHECK_EQ(pm::py_repr(J("[1, \"a\", true]")), std::string("[1, 'a', True]"));
    CHECK_EQ(pm::py_str(Json(1.5)), std::string("1.5"));
    CHECK_EQ(pm::py_str(Json(3.0)), std::string("3.0"));
    CHECK_EQ(pm::py_number("30"), Json(30));
    CHECK_EQ(pm::py_number("-2"), Json(-2));
    CHECK_EQ(pm::py_number("0.5"), Json(0.5));
    CHECK_EQ(pm::py_number("1_000"), Json(1000));
    CHECK_EQ(pm::py_number("abc"), Json("abc"));
    CHECK_EQ(pm::py_number("0x10"), Json("0x10"));
    CHECK_EQ(pm::dump_file(J(R"({"label": "기본", "config": {}})")), std::string("{\n  \"label\": \"기본\",\n  \"config\": {}\n}\n"));
    CHECK_EQ(pm::normpath("/a/b/../c/./d"), std::string("/a/c/d"));
    CHECK_EQ(pm::signal_name(SIGSEGV), std::string("SIGSEGV"));
}

// ---- 설정 스키마 (test_schema.py와 같은 경우) -----------------------------------------------------

Json gimbal() {
    Json g = J(R"({"groups": [
        {"title": "카메라", "section": "siyi", "fields": [
            {"key": "camera_ip", "type": "string", "format": "ipv4", "default": "192.168.144.25"},
            {"key": "center_pitch", "type": "number", "min": -90, "max": 90, "default": 0},
            {"key": "video_bitrate_kbps", "type": "int", "min": 256, "max": 20000},
            {"key": "time_sync", "type": "bool", "default": true},
            {"key": "video_codec", "type": "enum", "options": ["h264", "h265"]}]},
        {"title": "송출", "section": "stream", "fields": [
            {"key": "streams", "type": "list", "item_key": "name", "fields": []}]}]})");
    g["groups"][1]["fields"][0]["fields"] = load(repo() + "/workers/_template/lib/libvideo/stream-schema.json")["fields"];
    return g;
}
const Json kCamera = J(R"({"groups": [{"title": "카메라", "section": "camera", "fields": [
    {"key": "downsampling", "type": "enum", "options": [[1, "원본"], [2, "2×2 비닝"]], "default": 1}]}]})");
const Json kLink = J(R"({"groups": [{"title": "장치 링크", "section": "link", "fields": [
    {"key": "listen", "type": "string", "format": "host_port", "default": "127.0.0.1:9000"},
    {"key": "streams", "type": "map", "key_pattern": "^[A-Z][A-Z0-9_]*$", "value_type": "number", "min": 0, "max": 500}]}]})");

TEST(schema_examples_well_formed) {
    CHECK(pm::schema::check_schema(gimbal()).empty());
    CHECK(pm::schema::check_schema(kCamera).empty());
    CHECK(pm::schema::check_schema(kLink).empty());
}

TEST(schema_empty_and_unknown_keys) {
    CHECK(pm::schema::check_config(gimbal(), J("{}")).empty());
    CHECK(pm::schema::check_config(gimbal(), J(R"({"command_socket": "/tmp/x.sock"})")).empty());
}

TEST(schema_scalar_errors) {
    const Json e = pm::schema::check_config(gimbal(), J(R"({"siyi": {"camera_ip": "192.168.1", "center_pitch": 120,
        "video_bitrate_kbps": 1.5, "time_sync": "yes", "video_codec": "mjpeg"}})"));
    CHECK_EQ(keys(e), (std::set<std::string>{"siyi.camera_ip", "siyi.center_pitch", "siyi.video_bitrate_kbps",
                                             "siyi.time_sync", "siyi.video_codec"}));
    CHECK_EQ(e["siyi.center_pitch"], Json("-90 ~ 90 사이여야 합니다"));
    CHECK_EQ(e["siyi.video_codec"], Json("선택지 중 하나여야 합니다: h264, h265"));
    CHECK_EQ(e["siyi.video_bitrate_kbps"], Json("정수여야 합니다"));
}

TEST(schema_enum_numeric_and_bool) {
    CHECK(pm::schema::check_config(kCamera, J(R"({"camera": {"downsampling": 2}})")).empty());
    CHECK(pm::schema::check_config(kCamera, J(R"({"camera": {"downsampling": true}})")).contains("camera.downsampling"));
    CHECK(pm::schema::check_config(kCamera, J(R"({"camera": {"downsampling": 3}})")).contains("camera.downsampling"));
    CHECK(pm::schema::check_config(kCamera, J(R"({"camera": {"downsampling": 2.0}})")).empty());
}

TEST(schema_list_items) {
    const Json e = pm::schema::check_config(gimbal(), J(R"({"stream": {"streams": [
        {"name": "main", "url": "rtsp://127.0.0.1:8554/siyi"},
        {"name": "main", "url": "http://x"},
        {"url": "rtsp://a:1/b", "bitrate_kbps": 10}]}})"));
    CHECK_EQ(keys(e), (std::set<std::string>{"stream.streams[1].name", "stream.streams[1].url",
                                             "stream.streams[2].name", "stream.streams[2].bitrate_kbps"}));
    CHECK_EQ(e["stream.streams[1].name"], Json("같은 값이 이미 있습니다: main"));
    CHECK_EQ(e["stream.streams[2].name"], Json("비울 수 없습니다"));
}

TEST(schema_map) {
    CHECK(pm::schema::check_config(kLink, J(R"({"link": {"streams": {"STATUS": 10, "POSITION": 1}}})")).empty());
    const Json e = pm::schema::check_config(kLink, J(R"({"link": {"streams": {"status": 10, "POSITION": 900}}})"));
    CHECK_EQ(keys(e), (std::set<std::string>{"link.streams.status", "link.streams.POSITION"}));
}

TEST(schema_sections) {
    const Json sch = J(R"({"groups": [
        {"title": "모듈", "section": "link", "fields": [{"key": "listen", "type": "string", "format": "host_port"}]},
        {"title": "호스트", "fields": [{"key": "watchdog_sec", "type": "number", "min": 1}]}]})");
    CHECK(pm::schema::check_schema(sch).empty());
    CHECK(pm::schema::check_config(sch, J(R"({"link": {"listen": "127.0.0.1:1"}, "watchdog_sec": 3})")).empty());
    CHECK_EQ(keys(pm::schema::check_config(sch, J(R"({"link": {"listen": "x"}, "watchdog_sec": 0})"))),
             (std::set<std::string>{"link.listen", "watchdog_sec"}));
    CHECK_EQ(keys(pm::schema::check_config(sch, J(R"({"link": 5})"))), (std::set<std::string>{"link"}));
    CHECK(pm::schema::check_config(sch, J("{}")).empty());
    CHECK(!pm::schema::check_schema(J(R"({"groups": [{"section": "Bad-Name", "fields": []}]})")).empty());
}

TEST(schema_host_port) {
    CHECK(pm::schema::check_config(kLink, J(R"({"link": {"listen": "127.0.0.1:9000"}})")).empty());
    for (const char* bad : {"127.0.0.1", ":9000", "host:0", "host:99999"}) {
        Json c = J(R"({"link": {}})");
        c["link"]["listen"] = bad;
        CHECK(pm::schema::check_config(kLink, c).contains("link.listen"));
    }
}

TEST(schema_worker_id_and_refs) {
    const Json schema = J(R"({"groups": [
        {"section": "peer", "fields": [{"key": "peer", "type": "string", "format": "worker_id"}]},
        {"section": "multi", "fields": [{"key": "links", "type": "list", "item_key": "to", "fields": [
            {"key": "to", "type": "string", "format": "worker_id"}]}]}]})");
    CHECK(pm::schema::check_config(schema, J(R"({"peer": {"peer": "siyi"}})")).empty());
    CHECK(pm::schema::check_config(schema, J(R"({"peer": {"peer": ""}})")).empty());
    for (const char* bad : {"Siyi", "a b", "../x"}) {
        Json c = J(R"({"peer": {}})");
        c["peer"]["peer"] = bad;
        CHECK(pm::schema::check_config(schema, c).contains("peer.peer"));
    }
    const Json cfg = J(R"({"peer": {"peer": "siyi"}, "multi": {"links": [{"to": "ximea"}, {"to": ""}]}})");
    CHECK(pm::py_eq(pm::schema::worker_refs(schema, cfg), J(R"({"peer.peer": "siyi", "multi.links[0].to": "ximea"})")));
    CHECK(pm::schema::worker_refs(schema, J(R"({"peer": {"peer": ""}})")).empty());
}

TEST(schema_ipv4_rules) {
    const Json s = J(R"({"groups": [{"fields": [{"key": "ip", "type": "string", "format": "ipv4"}]}]})");
    for (const char* good : {"192.168.1.10", "0.0.0.0", "255.255.255.255"}) {
        Json c = Json::object();
        c["ip"] = good;
        CHECK(pm::schema::check_config(s, c).empty());
    }
    for (const char* bad : {"256.1.1.1", "01.2.3.4", "1.2.3", "1.2.3.4.5", "a.b.c.d", ""}) {
        Json c = Json::object();
        c["ip"] = bad;
        CHECK(pm::schema::check_config(s, c).contains("ip"));
    }
}

// 저장소의 워커 스키마·프리셋(test_schema.py SchemaFiles와 같은 데이터)
TEST(repo_worker_schemas_and_presets) {
    int types = 0, presets = 0;
    for (const auto& w : list_dir(repo() + "/workers")) {
        for (const std::string sub : {"", "/workspace"}) {
            const std::string tp = repo() + "/workers/" + w + sub + "/worker-types.json";
            if (!file_exists(tp)) continue;
            const Json decls = load(tp);
            for (auto it = decls.begin(); it != decls.end(); ++it) {
                ++types;
                CHECK(it->contains("config_schema"));
                const auto errs = pm::schema::check_schema((*it)["config_schema"]);
                if (!errs.empty()) std::cerr << "  " << it.key() << ": " << errs[0] << "\n";
                CHECK(errs.empty());
            }
            if (sub != "/workspace") continue;
            const std::string pd = repo() + "/workers/" + w + "/workspace/presets";
            for (const auto& f : list_dir(pd)) {
                ++presets;
                const Json data = load(pd + "/" + f);
                CHECK(data["config"].is_object());
                for (auto it = decls.begin(); it != decls.end(); ++it) {
                    const Json e = pm::schema::check_config((*it)["config_schema"], data["config"]);
                    if (!e.empty()) std::cerr << "  " << f << ": " << e.dump() << "\n";
                    CHECK(e.empty());
                }
            }
        }
    }
    CHECK(types > 0);
    CHECK(presets > 0);
}

// ---- heartbeat가 spawn보다 먼저 올 때 (test_notify_race.py) ----------------------------------------

TEST(notify_ready_before_spawn_finished) {
    Rig r(J(R"({"fast": {"config": {}, "autostart": false}})"));
    r.sp.next_pid = 4242;
    // 1) spawn이 끝나기 전에 READY가 먼저 옴(그 pid의 워커 없음)
    r.sup->on_notify(4242, J(R"({"READY": "1", "id": "fast", "libworker": "0.9.1"})"));
    CHECK(r.sup->has_early(4242));
    // 2) spawn이 끝나면 보관한 것을 처리
    CHECK_EQ(r.start("fast"), 4242);
    CHECK_EQ(r.w("fast").actual, std::string("running"));
    CHECK_EQ(r.w("fast").libworker, Json("0.9.1"));
    CHECK(!r.sup->has_early(4242));
}

TEST(notify_normal_order_unchanged) {
    Rig r(J(R"({"slow": {"config": {}, "autostart": false}})"));
    r.sp.next_pid = 7;
    CHECK_EQ(r.start("slow"), 7);
    CHECK_EQ(r.w("slow").actual, std::string("starting"));
    r.ready(7);
    CHECK_EQ(r.w("slow").actual, std::string("running"));
    CHECK_EQ(r.sup->early_count(), 0u);
}

TEST(notify_old_and_too_many_are_dropped) {
    Rig r(J(R"({"x": {"config": {}}})"));
    r.sup->on_notify(1, J(R"({"READY": "1"})"));
    r.clock.t += pm::Supervisor::kEarlyKeepS + 1;  // 오래됨
    r.sup->on_notify(2, J(R"({"READY": "1"})"));
    CHECK(!r.sup->has_early(1));
    for (int pid = 100; pid < 100 + static_cast<int>(pm::Supervisor::kEarlyMax) + 10; ++pid)
        r.sup->on_notify(pid, J(R"({"WATCHDOG": "1"})"));
    CHECK(r.sup->early_count() <= pm::Supervisor::kEarlyMax);
}

// ---- Supervisor 규칙 ----------------------------------------------------------------------------

TEST(spawn_args_env_and_payload) {
    Rig r(J(R"({"w": {"config": {"a": 1}, "autostart": true,
                       "decl": {"process": {"nice": 5, "cpu": [0, 1]}, "exec_candidates": ["/missing/x", "/ok/x"]}}})"));
    r.tick();
    CHECK_EQ(r.sp.spawned.size(), 1u);
    const auto& s = r.sp.spawned[0];
    CHECK_EQ(s.argv, (std::vector<std::string>{"/ok/x", "--config", r.dir + "/data/state/workers/w.json"}));
    CHECK(std::find(s.env.begin(), s.env.end(), "WORKSPACE_WORKER_ID=w") != s.env.end());
    CHECK(std::find(s.env.begin(), s.env.end(), "WORKSPACE_MANAGER_SOCKET=/old") == s.env.end());
    CHECK(s.nice == 5);
    CHECK_EQ(s.cpus, (std::vector<int>{0, 1}));
    CHECK_EQ(s.log_path, r.dir + "/data/state/logs/w.log");
    CHECK(pm::py_eq(load(r.dir + "/data/state/workers/w.json"),
                    J(R"({"id": "w", "type": "w_t", "config_version": 1, "config": {"a": 1}})")));
    const Json st = r.st("w");
    CHECK_EQ(st["exec"], Json("/ok/x"));
    CHECK(pm::py_eq(st["process"], J(R"({"nice": 5, "cpu": [0, 1]})")));
    CHECK_EQ(st["started_at"], Json("2026-11-20T10:00:00+09:00"));
    CHECK_EQ(st["applied_version"], Json(1));
}

TEST(restart_backoff_and_crash_loop) {
    Rig r(J(R"({"c": {"config": {}, "autostart": true}})"));
    std::vector<double> intervals;
    for (int i = 0; i < 6; ++i) {
        r.tick();
        const int pid = r.w("c").pid;
        CHECK(pid > 0);
        r.clock.t += 0.1;
        r.sup->on_exit(pid, 2);
        const Json st = r.st("c");
        CHECK_EQ(st["actual_state"], Json("exited"));
        CHECK_EQ(st["last_error"], Json("exit 2"));
        CHECK_EQ(st["last_exit_code"], Json(2));
        CHECK_EQ(r.sp.group_kills.back(), std::make_pair(pid, SIGKILL));  // 남은 자식 정리
        intervals.push_back(st["restart_in_s"].get<double>());
        CHECK_EQ(st["crash_loop"], Json(i >= 4));  // 5번째 실패부터
        r.tick();  // 아직 간격 안: 띄우지 않음
        CHECK(!r.w("c").has_proc());
        r.clock.t += intervals.back();
    }
    CHECK_EQ(intervals, (std::vector<double>{2.0, 4.0, 8.0, 16.0, 30.0, 30.0}));
    CHECK_EQ(r.st("c")["restart_count"], Json(6));
    // start 명령이면 처음부터
    const Json d = r.run(r.sup->start(Json("c")));
    CHECK_EQ(d["restart_count"], Json(0));
    CHECK_EQ(d["crash_loop"], Json(false));
    // 오래(healthy_s) 정상으로 돌면 간격·연속 실패 표시를 처음으로
    r.tick();
    const int pid = r.w("c").pid;
    r.ready(pid);
    r.sup->on_exit(pid, 2);
    CHECK_EQ(r.st("c")["restart_in_s"], Json(2.0));
    r.clock.t += 2.0;
    r.tick();
    const int pid2 = r.w("c").pid;
    r.ready(pid2);
    for (int i = 0; i < 62; ++i) {
        r.clock.t += 1.0;
        r.beat(pid2);
        r.sup->tick();
    }
    CHECK_EQ(r.w("c").fail_streak, 0);
    r.sup->on_exit(pid2, 3);
    CHECK_EQ(r.st("c")["restart_in_s"], Json(2.0));
    CHECK_EQ(r.st("c")["last_error"], Json("exit 3"));
}

TEST(exit_reasons) {
    Rig r(J(R"({"x": {"config": {}, "autostart": true}})"));
    auto cycle = [&](int rc) {
        r.clock.t += 60;
        r.tick();
        const int pid = r.w("x").pid;
        r.ready(pid);
        r.sup->on_exit(pid, rc);
        return r.st("x");
    };
    CHECK_EQ(cycle(0)["last_error"], Json("unexpected exit 0"));
    CHECK_EQ(cycle(1)["last_error"], Json("config error (exit 1)"));
    Json st = cycle(-SIGSEGV);
    CHECK_EQ(st["last_error"], Json("killed by SIGSEGV"));
    CHECK_EQ(st["last_signal"], Json("SIGSEGV"));
    CHECK(st["last_exit_code"].is_null());
    r.sp.fail = true;
    r.clock.t += 60;
    r.tick();
    CHECK_EQ(r.st("x")["last_error"], Json("spawn failed: [Errno 2] No such file or directory: 'x'"));
    CHECK_EQ(r.st("x")["actual_state"], Json("exited"));
}

TEST(stalled_no_ready_then_sigkill) {
    Rig r(J(R"({"s": {"config": {}, "autostart": true}})"));
    r.tick();
    const int pid = r.w("s").pid;
    r.clock.t += 9.9;
    r.sup->tick();
    CHECK_EQ(r.sp.count(SIGTERM), 0);
    r.clock.t += 0.2;
    r.sup->tick();
    CHECK_EQ(r.sp.count(SIGTERM), 1);
    CHECK_EQ(r.st("s")["last_error"], Json("stalled: no READY in 10s"));
    r.clock.t += 4.9;
    r.sup->tick();
    CHECK(r.sp.group_kills.empty());
    r.clock.t += 0.2;
    r.sup->tick();
    CHECK_EQ(r.sp.group_kills.size(), 1u);
    CHECK_EQ(r.sp.group_kills[0], std::make_pair(pid, SIGKILL));
    r.sup->tick();
    CHECK_EQ(r.sp.group_kills.size(), 1u);  // 한 번만
    r.sup->on_exit(pid, -SIGKILL);
    const Json st = r.st("s");
    CHECK_EQ(st["last_error"], Json("stalled: no READY in 10s"));
    CHECK_EQ(st["last_signal"], Json("SIGKILL"));
    CHECK_EQ(st["actual_state"], Json("exited"));
}

TEST(stalled_no_heartbeat_and_metrics) {
    Rig r(J(R"({"h": {"config": {}, "autostart": true, "decl": {"watchdog_sec": 1.5}}})"));
    r.tick();
    const int pid = r.w("h").pid;
    r.sup->on_notify(pid, J(R"({"READY": "1", "libworker": "0.9.1", "sim": "1"})"));
    r.beat(pid, R"({"x_fps": "30", "x_name": "abc", "x_ratio": "0.5", "id": "h", "config_version": "1"})");
    Json st = r.st("h");
    CHECK(pm::py_eq(st["metrics"], J(R"({"x_fps": 30, "x_name": "abc", "x_ratio": 0.5})")));
    CHECK_EQ(st["progress"], Json(5));
    CHECK_EQ(st["sim"], Json(true));
    CHECK_EQ(st["last_heartbeat_ms_ago"], Json(0));
    r.clock.t += 0.25;
    CHECK_EQ(r.st("h")["last_heartbeat_ms_ago"], Json(250));
    // progress가 정수가 아니면 그 heartbeat의 나머지는 버림(Python과 같음)
    r.sup->on_notify(pid, J(R"({"WATCHDOG": "1", "progress": "x", "y_val": "1"})"));
    CHECK(!r.st("h")["metrics"].contains("y_val"));
    r.clock.t += 1.5;
    r.sup->tick();
    CHECK_EQ(r.st("h")["last_error"], Json("stalled: no heartbeat for 1.5s"));
    CHECK_EQ(r.sp.count(SIGTERM), 1);
    r.sup->on_exit(pid, 0);
    st = r.st("h");
    CHECK_EQ(st["actual_state"], Json("exited"));  // stalled는 실패
    CHECK(st["metrics"].is_null());
    CHECK(st["resources"].is_null());
}

TEST(stop_requested) {
    Rig r(J(R"({"w": {"config": {}, "autostart": true}})"));
    r.tick();
    const int pid = r.w("w").pid;
    r.ready(pid);
    r.run(r.sup->stop(Json("w")));
    r.sup->tick();
    CHECK_EQ(r.sp.count(SIGTERM), 1);
    r.sup->on_exit(pid, 0);
    const Json st = r.st("w");
    CHECK_EQ(st["actual_state"], Json("stopped"));
    CHECK_EQ(st["last_exit_code"], Json(0));
    CHECK(st["last_error"].is_null());
    CHECK(pm::py_eq(load(r.dir + "/data/run/desired.json")["workers"], J(R"({"w": "stopped"})")));
}

TEST(live_keys_sighup_ack_and_timeout) {
    Rig r(J(R"({"l": {"config": {"mod": {"level": 1, "name": "a"}}, "autostart": true,
                       "decl": {"live_keys": ["mod.level"]}}})"));
    r.tick();
    const int pid = r.w("l").pid;
    r.ready(pid);
    // live 키만: 설정 파일을 쓴 뒤 SIGHUP, 다시 시작하지 않음
    r.run(r.sup->set_config(Json("l"), J(R"({"mod": {"level": 2, "name": "a"}})"), false));
    CHECK_EQ(r.sp.count(SIGHUP), 1);
    CHECK_EQ(load(r.dir + "/data/state/workers/l.json")["config_version"], Json(2));
    r.sup->tick();
    CHECK_EQ(r.sp.count(SIGTERM), 0);
    r.beat(pid, R"({"config_version": "2"})");
    CHECK_EQ(r.st("l")["applied_version"], Json(2));
    CHECK(!r.w("l").live_version);
    // 확인이 없으면 5 s 뒤 재시작
    r.run(r.sup->set_config(Json("l"), J(R"({"mod": {"level": 3, "name": "a"}})"), false));
    CHECK_EQ(r.sp.count(SIGHUP), 2);
    r.clock.t += 4.9;
    r.beat(pid);
    r.sup->tick();
    CHECK_EQ(r.sp.count(SIGTERM), 0);
    r.clock.t += 0.2;
    r.sup->tick();
    CHECK_EQ(r.sp.count(SIGTERM), 1);
    CHECK_EQ(*r.w("l").stop_reason, std::string("restart"));
    r.sup->on_exit(pid, 0);
    CHECK_EQ(r.st("l")["actual_state"], Json("stopped"));
    r.tick();  // 바로 새 설정으로
    CHECK_EQ(r.st("l")["applied_version"], Json(3));
    const int pid2 = r.w("l").pid;
    r.ready(pid2);
    // live가 아닌 키: 바로 재시작
    r.run(r.sup->set_config(Json("l"), J(R"({"mod": {"level": 3, "name": "b"}})"), false));
    CHECK_EQ(r.sp.count(SIGHUP), 2);
    r.sup->tick();
    CHECK_EQ(r.sp.count(SIGTERM), 2);
}

TEST(load_versions_runtime_temp) {
    auto prep = [](const std::string& boot) {
        return [boot](const std::string& dir) {
            write_file(dir + "/data/state/versions.json",
                       R"({"a": {"config_version": 7, "config": {"x": 1}}, "b": {"config_version": 4, "config": {"x": 0}}})");
            write_file(dir + "/data/run/desired.json", "{\"boot_id\": \"" + boot + "\", \"workers\": {\"a\": \"running\", \"b\": \"stopped\"}}");
            write_file(dir + "/data/run/temp_config.json", "{\"boot_id\": \"" + boot + "\", \"workers\": {\"b\": {\"x\": 9}}}");
            write_file(dir + "/data/state/workers/ghost.json", "{}");
        };
    };
    const Json workers = J(R"({"a": {"config": {"x": 1}, "autostart": false}, "b": {"config": {"x": 2}, "autostart": true}})");
    {
        Rig r(workers, Json::object(), false, "boot-1", prep("boot-1"));  // 같은 부팅
        CHECK_EQ(r.w("a").config_version, 7);   // 값이 같으면 버전 그대로
        CHECK_EQ(r.w("a").desired_state, std::string("running"));
        CHECK_EQ(r.w("b").desired_state, std::string("stopped"));
        CHECK(r.w("b").temp);
        CHECK(pm::py_eq(r.w("b").config, J(R"({"x": 9})")));
        CHECK_EQ(r.w("b").config_version, 5);   // 다르면 +1
        CHECK(!file_exists(r.dir + "/data/state/workers/ghost.json"));
        CHECK(pm::py_eq(load(r.dir + "/data/run/temp_config.json"), J(R"({"boot_id": "boot-1", "workers": {"b": {"x": 9}}})")));
    }
    {
        Rig r(workers, Json::object(), false, "boot-2", prep("boot-1"));  // 다시 부팅
        CHECK_EQ(r.w("a").desired_state, std::string("stopped"));
        CHECK_EQ(r.w("b").desired_state, std::string("running"));
        CHECK(!r.w("b").temp);
        CHECK_EQ(r.w("b").config_version, 5);
    }
}

TEST(product_refuses_sim) {
    Rig r(J(R"({"p": {"config": {"sim": {}}, "autostart": true}})"), Json::object(), true);
    r.tick();
    CHECK(r.sp.spawned.empty());
    CHECK_EQ(r.st("p")["actual_state"], Json("error"));
    CHECK_EQ(r.st("p")["last_error"], Json("sim (fake device) not allowed in product"));
}

TEST(write_ops_and_errors) {
    const Json schema = J(R"({"groups": [{"section": "mod", "fields": [{"key": "level", "type": "int", "min": 0, "max": 10}]}]})");
    Json decl = Json::object();
    decl["config_schema"] = schema;
    Json workers = Json::object();
    workers["w"] = Json::object();
    workers["w"]["autostart"] = false;
    workers["w"]["decl"] = decl;
    Rig r(workers);
    auto e = api_error_of([&] { r.sup->set_config(Json("w"), J(R"({"mod": {"level": 11}})"), true); });
    CHECK_EQ(e.code, std::string("bad_request"));
    CHECK_EQ(e.message, std::string("설정 값이 맞지 않습니다: mod.level: 0 ~ 10 사이여야 합니다"));
    CHECK(pm::py_eq(e.fields, J(R"({"mod.level": "0 ~ 10 사이여야 합니다"})")));
    e = api_error_of([&] { r.sup->get(Json("nope")); });
    CHECK_EQ(e.message, std::string("unknown worker id 'nope'"));
    e = api_error_of([&] { r.sup->get(Json()); });
    CHECK_EQ(e.message, std::string("unknown worker id None"));
    // 저장: 프리셋 파일 생성(label 기본), 버전 +1
    Json d = r.run(r.sup->set_config(Json("w"), J(R"({"mod": {"level": 3}})"), true));
    CHECK_EQ(d["config_version"], Json(2));
    CHECK(pm::py_eq(load(r.dir + "/ws/w/presets/default.json"), J(R"({"label": "기본", "config": {"mod": {"level": 3}}})")));
    // 새 이름으로 저장 → 사용 중
    d = r.run(r.sup->save_preset(Json("w"), J(R"({"name": "out", "label": "야외"})")));
    CHECK_EQ(d["preset"], Json("out"));
    CHECK(pm::py_eq(load(r.dir + "/ws/w/config.json"), J(R"({"active": "out", "autostart": false})")));
    e = api_error_of([&] { r.sup->save_preset(Json("w"), J(R"({"name": "out"})")); });
    CHECK_EQ(e.message, std::string("preset 'out' already exists"));
    e = api_error_of([&] { r.sup->delete_preset(Json("w"), Json("out")); });
    CHECK_EQ(e.code, std::string("conflict"));
    e = api_error_of([&] { r.sup->delete_preset(Json("w"), Json("Bad Name")); });
    CHECK_EQ(e.code, std::string("internal"));  // Python 판과 같음
    e = api_error_of([&] { r.sup->use_preset(Json("w"), Json("Bad Name")); });
    CHECK_EQ(e.code, std::string("bad_request"));
    d = r.run(r.sup->rename_preset(Json("w"), Json("out"), J(R"({"name": "out2"})")));
    CHECK_EQ(d["preset"], Json("out2"));
    CHECK(file_exists(r.dir + "/ws/w/presets/out2.json"));
    CHECK(!file_exists(r.dir + "/ws/w/presets/out.json"));
    d = r.run(r.sup->set_autostart(Json("w"), J(R"({"autostart": true})")));
    CHECK_EQ(d["autostart"], Json(true));
    e = api_error_of([&] { r.sup->set_autostart(Json("w"), Json("yes")); });
    CHECK_EQ(e.message, std::string("autostart must be true or false"));
}

}  // namespace

int main() {
    for (auto& [name, fn] : tests()) {
        g_test = name;
        const int before = g_fail;
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_fail;
            std::cerr << "FAIL " << name << ": exception " << e.what() << "\n";
        }
        std::cout << (g_fail == before ? "ok   " : "FAIL ") << name << "\n";
    }
    std::cout << tests().size() << " tests, " << g_checks << " checks, " << g_fail << " failures\n";
    return g_fail ? 1 : 0;
}
