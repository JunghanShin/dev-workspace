#include "supervisor.hpp"

#include <algorithm>
#include <cmath>
#include <csignal>

#include "log.hpp"
#include "schema.hpp"

namespace pm {

namespace {

const std::set<std::string> kRequested = {"requested", "restart", "shutdown"};
// heartbeat 예약 키. 나머지 key=value는 워커 지표 (design.md 9.5)
const std::set<std::string> kHbKeys = {"READY", "WATCHDOG", "progress", "config_version", "id", "libworker", "sim"};

Json jget(const Json& obj, const std::string& key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? Json(nullptr) : *it;
}

ApiError api_error(const std::string& code, const std::string& message) { return ApiError{code, message, nullptr}; }

// 바뀐 설정 경로. 모듈 칸(객체)은 한 단계 더 들어가 "<모듈>.<키>"(live_keys와 같은 형식)
std::set<std::string> changed_paths(const Json& old, const Json& now) {
    std::set<std::string> out, keys;
    for (auto it = old.begin(); it != old.end(); ++it) keys.insert(it.key());
    for (auto it = now.begin(); it != now.end(); ++it) keys.insert(it.key());
    for (const auto& k : keys) {
        const Json a = jget(old, k), b = jget(now, k);
        if (py_eq(a, b)) continue;
        if (a.is_object() && b.is_object()) {
            std::set<std::string> sub;
            for (auto it = a.begin(); it != a.end(); ++it) sub.insert(it.key());
            for (auto it = b.begin(); it != b.end(); ++it) sub.insert(it.key());
            for (const auto& kk : sub)
                if (!py_eq(jget(a, kk), jget(b, kk))) out.insert(k + "." + kk);
        } else {
            out.insert(k);
        }
    }
    return out;
}

// type "process": {"nice": -20..19, "cpu": [번호...]}. 잘못된 값은 무시
void process_options(const Json& t, std::optional<int>& nice, std::vector<int>& cpus) {
    const Json p = jget(t, "process");
    if (!p.is_object()) return;
    const Json n = jget(p, "nice"), c = jget(p, "cpu");
    if (n.is_number_integer() && n.get<long long>() >= -20 && n.get<long long>() <= 19) nice = static_cast<int>(n.get<long long>());
    if (c.is_array() && !c.empty()) {
        std::vector<int> v;
        for (const auto& x : c) {
            if (!x.is_number_integer() || x.get<long long>() < 0) return;
            v.push_back(static_cast<int>(x.get<long long>()));
        }
        cpus = v;
    }
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

std::string dirname(const std::string& p) {
    const size_t s = p.rfind('/');
    if (s == std::string::npos) return "";
    return s == 0 ? "/" : p.substr(0, s);
}

std::string basename(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

std::string normpath(const std::string& path) {
    if (path.empty()) return ".";
    const bool abs = path[0] == '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        const std::string seg = path.substr(i, j - i);
        if (seg.empty() || seg == ".") {
        } else if (seg == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!abs) parts.push_back("..");
        } else {
            parts.push_back(seg);
        }
        i = j + 1;
    }
    std::string out = abs ? "/" : "";
    out += join(parts, "/");
    return out.empty() ? "." : out;
}

std::string signal_name(int sig) {
    static const std::map<int, const char*> names = {
        {SIGHUP, "SIGHUP"},   {SIGINT, "SIGINT"},   {SIGQUIT, "SIGQUIT"}, {SIGILL, "SIGILL"},
        {SIGTRAP, "SIGTRAP"}, {SIGABRT, "SIGABRT"}, {SIGBUS, "SIGBUS"},   {SIGFPE, "SIGFPE"},
        {SIGKILL, "SIGKILL"}, {SIGUSR1, "SIGUSR1"}, {SIGSEGV, "SIGSEGV"}, {SIGUSR2, "SIGUSR2"},
        {SIGPIPE, "SIGPIPE"}, {SIGALRM, "SIGALRM"}, {SIGTERM, "SIGTERM"}, {SIGSTKFLT, "SIGSTKFLT"},
        {SIGCHLD, "SIGCHLD"}, {SIGCONT, "SIGCONT"}, {SIGSTOP, "SIGSTOP"}, {SIGTSTP, "SIGTSTP"},
        {SIGTTIN, "SIGTTIN"}, {SIGTTOU, "SIGTTOU"}, {SIGURG, "SIGURG"},   {SIGXCPU, "SIGXCPU"},
        {SIGXFSZ, "SIGXFSZ"}, {SIGVTALRM, "SIGVTALRM"}, {SIGPROF, "SIGPROF"}, {SIGWINCH, "SIGWINCH"},
        {SIGIO, "SIGIO"},     {SIGPWR, "SIGPWR"},   {SIGSYS, "SIGSYS"},
    };
    auto it = names.find(sig);
    if (it != names.end()) return it->second;
    if (sig == SIGRTMIN) return "SIGRTMIN";
    if (sig == SIGRTMAX) return "SIGRTMAX";
    return std::to_string(sig);
}

Supervisor::Supervisor(SupervisorConfig cfg, Clock& clock, Spawner& spawner, Store& store, FileSystem& fs)
    : cfg_(std::move(cfg)), clock_(clock), spawner_(spawner), store_(store), fs_(fs) {
    for (auto it = cfg_.types.begin(); it != cfg_.types.end(); ++it)
        if (jget(*it, "builtin").is_object()) builtin_types_.insert(it.key());
}

// ---- 설정 값 -----------------------------------------------------------------------------------

const Json& Supervisor::type_of(const Worker& w) const {
    static const Json empty = Json::object();
    auto it = cfg_.types.find(w.type);
    return it == cfg_.types.end() ? empty : *it;
}

double Supervisor::tnum(const Json& t, const char* key, double dflt) const {
    const Json v = jget(t, key);
    return v.is_number() ? v.get<double>() : dflt;
}

double Supervisor::rnum(const char* key) const {
    const Json v = jget(jget(cfg_.settings, "restart"), key);
    return v.is_number() ? v.get<double>() : 0.0;
}

// ---- 영속화 -------------------------------------------------------------------------------------

Json Supervisor::load_json_file(const std::string& path, bool log_invalid) {
    std::string text, err;
    if (fs_.read(path, text, err) != FileSystem::Read::Ok) return Json::object();
    auto j = parse(text, &err);
    if (!j) {
        if (log_invalid) PM_ERROR("%s is not valid JSON (%s); not moved", path.c_str(), err.c_str());
        return Json::object();
    }
    return j->is_object() ? *j : Json::object();
}

Json Supervisor::load_boot_file(const std::string& path) {
    const Json data = load_json_file(path, false);
    const Json want = cfg_.boot_id.empty() ? Json(nullptr) : Json(cfg_.boot_id);
    if (!data.contains("boot_id") || data["boot_id"] != want) return Json::object();
    const Json w = jget(data, "workers");
    return w.is_object() ? w : Json::object();
}

void Supervisor::submit_quiet(std::vector<FileOp> ops, const char* what) {
    std::string w = what;
    store_.submit(std::move(ops), [w](bool ok, const std::string& err) {
        if (!ok) PM_ERROR("%s write failed: %s", w.c_str(), err.c_str());
    });
}

void Supervisor::save_versions() {
    Json versions = Json::object();
    for (const auto& w : workers_) {
        if (w->file_error) continue;
        Json v = Json::object();
        v["config_version"] = w->config_version;
        v["config"] = w->config;
        versions[w->id] = v;
    }
    FileOp op;
    op.path = cfg_.paths.versions_path();
    op.text = dump_file(versions);
    op.sync_dir = true;
    submit_quiet({op}, "versions.json");
}

void Supervisor::save_runtime() {
    const Json boot = cfg_.boot_id.empty() ? Json(nullptr) : Json(cfg_.boot_id);
    Json states = Json::object(), temps = Json::object();
    for (const auto& w : workers_) {
        if (!w->file_error) states[w->id] = w->desired_state;
        if (w->temp) temps[w->id] = w->config;
    }
    Json a = Json::object(), b = Json::object();
    a["boot_id"] = boot;
    a["workers"] = states;
    b["boot_id"] = boot;
    b["workers"] = temps;
    FileOp oa, ob;
    oa.path = cfg_.paths.runtime_path();
    oa.text = dump_file(a);
    oa.sync_dir = true;
    ob.path = cfg_.paths.temp_path();
    ob.text = dump_file(b);
    ob.sync_dir = true;
    submit_quiet({oa, ob}, "run state");
}

void Supervisor::load() {
    const Json legacy = load_json_file(cfg_.paths.workers_file, true);
    const Json versions = load_json_file(cfg_.paths.versions_path(), false);
    const Json runtime = load_boot_file(cfg_.paths.runtime_path());
    const Json temps = load_boot_file(cfg_.paths.temp_path());
    if (!runtime.empty()) PM_INFO("run state: restored from %s", cfg_.paths.runtime_path().c_str());
    else PM_INFO("run state: boot (autostart)");

    std::vector<std::pair<std::string, const Json*>> order;
    for (auto it = cfg_.types.begin(); it != cfg_.types.end(); ++it) order.emplace_back(it.key(), &*it);
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        const Json x = jget(*a.second, "_worker_id"), y = jget(*b.second, "_worker_id");
        return (x.is_string() ? x.get<std::string>() : "") < (y.is_string() ? y.get<std::string>() : "");
    });

    std::vector<FileOp> migrate;
    for (const auto& [tname, tp] : order) {
        const Json& t = *tp;
        const Json wid = jget(t, "_worker_id"), conf = jget(t, "_conf_dir");
        if (!wid.is_string() || wid.get<std::string>().empty() || !conf.is_string() || conf.get<std::string>().empty())
            continue;
        auto w = std::make_unique<Worker>();
        w->id = wid.get<std::string>();
        w->type = tname;
        w->files = std::make_unique<WorkerFiles>(fs_, conf.get<std::string>());
        try {
            const Json builtin = jget(t, "builtin").is_object() ? jget(t, "builtin") : Json::object();
            auto cfg = w->files->read_config();
            std::optional<Preset> preset;
            bool moved = false;
            if (!cfg || !w->files->has_preset(cfg->active)) {
                const Json* old = nullptr;
                for (auto it = legacy.begin(); it != legacy.end(); ++it)
                    if (it->is_object() && jget(*it, "type") == Json(tname)) {
                        old = &*it;
                        break;
                    }
                if (old && jget(*old, "config").is_object()) {
                    // 예전 workers.json의 값을 워커 폴더로 옮긴다(한 번)
                    const Json as = old->contains("autostart") ? (*old)["autostart"]
                                                               : Json(jget(*old, "desired_state") == Json("running"));
                    migrate.push_back(w->files->write_preset(kDefaultPreset, "기본", (*old)["config"]));
                    migrate.push_back(w->files->write_config(kDefaultPreset, py_truthy(as)));
                    PM_INFO("%s: settings moved from workers.json to %s", w->id.c_str(), w->files->dir().c_str());
                    cfg = WorkerConfig{kDefaultPreset, py_truthy(as)};
                    preset = Preset{"기본", (*old)["config"]};
                    moved = true;
                } else if (!cfg) {
                    cfg = WorkerConfig{kDefaultPreset, py_truthy(jget(builtin, "autostart"))};
                }
            }
            if (!moved) preset = w->files->read_preset(cfg->active);
            if (!preset) {
                // 프리셋 파일이 아직 없다: type 선언의 기본값으로 돈다. 저장하면 파일이 생긴다
                const Json d = jget(t, "default").is_object() ? jget(t, "default") : builtin;
                const Json c = jget(d, "config");
                preset = Preset{"기본", c.is_object() ? c : Json::object()};
            }
            w->autostart = cfg->autostart;
            w->preset = cfg->active;
            w->saved = preset->config;
            w->config = preset->config;
        } catch (const WorkerFileError& e) {
            w->autostart = false;
            w->preset = nullptr;
            w->actual = "error";
            w->last_error = std::string("설정 파일: ") + e.what();
            w->file_error = true;
            PM_ERROR("%s: %s", w->id.c_str(), w->last_error.get<std::string>().c_str());
        }
        if (!w->file_error) {
            const Json tc = jget(temps, w->id);
            if (tc.is_object()) {
                w->config = tc;  // 이번 부팅에서 '적용'만 하고 저장하지 않은 값
                w->temp = true;
            }
            const Json errs = config_errors(tname, w->config);
            if (!errs.empty()) {
                std::vector<std::string> parts;
                for (auto it = errs.begin(); it != errs.end(); ++it) parts.push_back(it.key() + ": " + it->get<std::string>());
                std::sort(parts.begin(), parts.end());
                PM_WARN("%s: config does not match schema: %s", w->id.c_str(), join(parts, "; ").c_str());
            }
            const Json prev = jget(versions, w->id);
            if (!prev.is_object()) {
                w->config_version = 1;
            } else {
                const Json pv = jget(prev, "config_version");
                if (py_eq(jget(prev, "config"), w->config))
                    w->config_version = pv.is_number_integer() ? pv.get<long long>() : 1;
                else
                    w->config_version = (pv.is_number_integer() ? pv.get<long long>() : 0) + 1;
            }
            const Json rs = jget(runtime, w->id);
            if (rs == Json("running") || rs == Json("stopped")) w->desired_state = rs.get<std::string>();
            else w->desired_state = w->autostart ? "running" : "stopped";
            const Json met = jget(t, "_requires_met");  // src가 type을 읽을 때 채움("requires" 도구가 다 있나)
            if (met.is_boolean() && !met.get<bool>()) {
                // 기본 탑재 워커의 필요 도구가 없는 기계: 띄우지 않는다
                w->desired_state = "stopped";
                w->actual = "disabled";
                std::vector<std::string> req;
                for (const auto& r : jget(t, "requires"))
                    if (r.is_string()) req.push_back(r.get<std::string>());
                w->last_error = "needs " + join(req, ", ");
            }
            w->peers = schema::worker_refs(jget(t, "config_schema"), w->config);
        } else {
            w->desired_state = "stopped";
            w->config_version = 0;
        }
        auto found = by_id_.find(w->id);
        if (found != by_id_.end()) {
            for (auto& slot : workers_)
                if (slot.get() == found->second) {
                    slot = std::move(w);
                    found->second = slot.get();
                    break;
                }
        } else {
            by_id_[w->id] = w.get();
            workers_.push_back(std::move(w));
        }
    }
    if (!migrate.empty()) submit_quiet(std::move(migrate), "worker folder");
    if (!legacy.empty()) {
        FileOp op;
        op.kind = FileOp::Kind::Rename;
        op.path = cfg_.paths.workers_file;
        op.to = cfg_.paths.workers_file + ".moved";
        submit_quiet({op}, "workers.json rename");
        PM_INFO("%s moved to worker folders; renamed to workers.json.moved", cfg_.paths.workers_file.c_str());
    }
    PM_INFO("loaded %zu worker(s) from worker folders", workers_.size());
    // 빠진 워커·예전 id의 실행 설정 파일 정리(로그는 기록이라 둔다)
    std::vector<FileOp> prune;
    for (const auto& f : fs_.list(cfg_.paths.workers_dir())) {
        if (f.size() <= 5 || f.compare(f.size() - 5, 5, ".json") != 0) continue;
        if (by_id_.count(f.substr(0, f.size() - 5))) continue;
        FileOp op;
        op.kind = FileOp::Kind::Remove;
        op.path = cfg_.paths.workers_dir() + "/" + f;
        op.ignore_error = true;
        prune.push_back(op);
        PM_INFO("removed stale worker config %s", f.c_str());
    }
    if (!prune.empty()) submit_quiet(std::move(prune), "stale worker config");
    save_versions();
    save_runtime();
}

// ---- 상태 ---------------------------------------------------------------------------------------

Json Supervisor::status(const Worker& w, double now) const {
    Json s = Json::object();
    s["id"] = w.id;
    s["type"] = w.type;
    s["builtin"] = builtin_types_.count(w.type) > 0;  // 기본 탑재(화면은 워커 목록에서 뺀다)
    {
        const Json k = jget(type_of(w), "kind");
        s["kind"] = k.is_string() ? k : Json("worker");  // worker | manager(docs/managers.md)
    }
    s["autostart"] = w.autostart;
    s["preset"] = w.preset;
    s["temp"] = w.temp;
    s["libworker"] = w.libworker;
    s["sim"] = w.sim;
    s["exec"] = w.exec_path;
    s["peers"] = w.peers;
    s["inputs"] = inputs_status ? inputs_status(w.id) : Json::object();
    s["desired_state"] = w.desired_state;
    s["actual_state"] = w.actual;
    s["pid"] = w.has_proc() ? Json(w.pid) : Json(nullptr);
    s["last_exit_code"] = w.last_exit_code;
    s["last_signal"] = w.last_signal;
    s["last_error"] = w.last_error;
    s["restart_count"] = w.restart_count;
    s["crash_loop"] = w.crash_loop;
    if (!w.has_proc() && w.actual == "exited")
        s["restart_in_s"] = std::round(std::max(0.0, w.next_start_mono - now) * 10.0) / 10.0;
    else
        s["restart_in_s"] = nullptr;
    s["process"] = w.process;
    s["started_at"] = w.started_at;
    if (w.has_proc() && w.last_hb_mono)
        s["last_heartbeat_ms_ago"] = static_cast<long long>((now - *w.last_hb_mono) * 1000);
    else
        s["last_heartbeat_ms_ago"] = nullptr;
    s["progress"] = w.progress;
    s["config_version"] = w.config_version;
    s["applied_version"] = w.applied_version ? Json(*w.applied_version) : Json(nullptr);
    s["metrics"] = w.metrics;
    s["resources"] = w.resources;
    return s;
}

void Supervisor::emit(Worker& w) {
    w.peers = schema::worker_refs(jget(type_of(w), "config_schema"), w.config);
    if (listener) listener("worker_status", w.id, status(w, clock_.mono()));
}

Worker* Supervisor::worker(const std::string& id) {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : it->second;
}

Worker& Supervisor::lookup(const Json& id) {
    if (id.is_string()) {
        auto it = by_id_.find(id.get<std::string>());
        if (it != by_id_.end()) return *it->second;
    }
    throw api_error("not_found", "unknown worker id " + py_repr(id));
}

Worker& Supervisor::valid(const Json& id) {
    Worker& w = lookup(id);
    if (w.file_error)
        throw api_error("conflict", py_str(w.last_error) + "; fix the file in the worker folder and restart the workspace");
    return w;
}

Json Supervisor::list() {
    const double now = clock_.mono();
    Json out = Json::array();
    for (const auto& w : workers_) out.push_back(status(*w, now));
    return out;
}

Json Supervisor::get(const Json& id) { return status(lookup(id), clock_.mono()); }

Json Supervisor::get_config(const Json& id) {
    Worker& w = lookup(id);
    Json out = Json::object();
    out["id"] = w.id;
    out["config"] = w.config;
    out["saved"] = w.saved;
    out["temp"] = w.temp;
    out["preset"] = w.preset;
    out["presets"] = w.files ? w.files->presets() : Json::array();
    out["autostart"] = w.autostart;
    out["config_version"] = w.config_version;
    out["files"] = w.files ? Json(w.files->dir()) : Json(nullptr);
    const Json errs = config_errors(w.type, w.config);
    if (!errs.empty()) out["config_errors"] = errs;  // 파일을 직접 고쳐 스키마에 안 맞는 값(화면에 표시)
    return out;
}

Json Supervisor::get_schema(const Json& id) {
    Worker& w = lookup(id);
    const Json& t = type_of(w);
    Json out = Json::object();
    out["id"] = w.id;
    out["type"] = w.type;
    out["schema"] = jget(t, "config_schema");
    const Json lk = jget(t, "live_keys");
    out["live_keys"] = lk.is_array() ? lk : Json::array();
    return out;
}

Json Supervisor::get_io(const Json& id) {
    Worker& w = lookup(id);
    if (!w.files) throw api_error("not_found", "worker " + w.id + " has no worker folder");
    const std::string dir = w.files->dir();
    const std::string folder = basename(dir) == "workspace" ? dirname(dir) : dir;
    auto load = [&](const std::string& path, const std::string& what) -> Json {
        std::string text, err;
        const auto r = fs_.read(path, text, err);
        if (r == FileSystem::Read::Missing) throw api_error("not_found", what + ": [Errno 2] No such file or directory: " + py_quote(path));
        if (r == FileSystem::Read::Error) throw api_error("not_found", what + ": " + err);
        auto j = parse(text, &err);
        if (!j) throw api_error("not_found", what + ": " + err);
        return *j;
    };
    const std::string index_path = folder + "/io.json";
    if (!fs_.exists(index_path)) throw api_error("not_found", "worker " + w.id + " has no io definitions (io.json)");
    const Json index = load(index_path, "io.json");
    Json out = Json::object();
    out["id"] = w.id;
    out["common"] = nullptr;
    out["self"] = nullptr;
    out["includes"] = Json::object();
    out["provides"] = Json::object();
    out["uses"] = Json::object();
    // 공통 정의는 libworker가 내보내는 칸: 워커의 libworker 사본 것, 없으면(Python 워커 등) 틀의 것
    std::string common = folder + "/lib/libworker/io/common.json";
    if (!fs_.exists(common)) common = cfg_.workspace_root + "/workers/_template/lib/libworker/io/common.json";
    if (fs_.exists(common)) out["common"] = load(common, "common.json");
    const std::string self_path = folder + "/io/self.json";
    if (fs_.exists(self_path)) {
        out["self"] = load(self_path, "io/self.json");
        const Json inc = jget(out["self"], "include");
        for (const auto& i : inc.is_array() ? inc : Json::array()) {
            if (!i.is_string()) continue;
            const std::string s = i.get<std::string>();
            const std::string p = normpath(!s.empty() && s[0] == '/' ? s : folder + "/" + s);
            if (p.rfind(folder + "/", 0) != 0)
                throw api_error("not_found", "io/self.json include " + s + ": outside worker folder");
            out["includes"][s] = load(p, s);
        }
    }
    const Json provides = jget(index, "provides");
    for (const auto& role : provides.is_array() ? provides : Json::array()) {
        const std::string r = py_str(role);
        std::string p = folder + "/io/" + r + ".json";
        if (!fs_.exists(p)) p = folder + "/io/uses/" + r + ".json";
        out["provides"][r] = load(p, "provides " + r);
    }
    const Json uses = jget(index, "uses");
    if (uses.is_object()) {
        for (auto it = uses.begin(); it != uses.end(); ++it) {
            Json entry = it->is_object() ? *it : Json::object();
            const std::string p = folder + "/io/uses/" + py_str(jget(*it, "from")) + ".json";
            entry["definition"] = fs_.exists(p) ? load(p, "uses " + it.key()) : Json(nullptr);
            out["uses"][it.key()] = entry;
        }
    }
    return out;
}

Json Supervisor::config_errors(const std::string& type, const Json& config) const {
    auto it = cfg_.types.find(type);
    return schema::check_config(it == cfg_.types.end() ? Json() : jget(*it, "config_schema"), config);
}

void Supervisor::check_schema(const std::string& type, const Json& config) const {
    const Json errs = config_errors(type, config);
    if (errs.empty()) return;
    std::vector<std::pair<std::string, std::string>> items;
    for (auto it = errs.begin(); it != errs.end(); ++it) items.emplace_back(it.key(), it->get<std::string>());
    std::sort(items.begin(), items.end());
    std::vector<std::string> parts;
    for (size_t i = 0; i < items.size() && i < 5; ++i) parts.push_back(items[i].first + ": " + items[i].second);
    throw ApiError{"bad_request", "설정 값이 맞지 않습니다: " + join(parts, "; "), errs};
}

// 칸에 "unique": true가 붙은 값(장치 시리얼 등)을 다른 워커가 같은 경로(역시 unique)에 이미 쓰고 있으면 conflict.
// 플랫폼은 칸 이름을 모른다(스키마 표시만 본다)
void Supervisor::check_unique(const std::string& id, const Json& config) const {
    auto schema_of = [this](const std::string& type) {
        auto it = cfg_.types.find(type);
        return it == cfg_.types.end() ? Json() : jget(*it, "config_schema");
    };
    const Worker* me = nullptr;
    for (const auto& o : workers_)
        if (o->id == id) me = o.get();
    if (!me) return;
    const Json mine = schema::unique_values(schema_of(me->type), config);
    if (mine.empty()) return;
    for (const auto& o : workers_) {
        if (o->id == id) continue;
        const Json theirs = schema::unique_values(schema_of(o->type), o->config);
        for (auto m = mine.begin(); m != mine.end(); ++m) {
            auto t = theirs.find(m.key());
            if (t != theirs.end() && py_eq(*t, *m))
                throw api_error("conflict", m.key() + " " + py_repr(*m) + " already used by " + py_quote(o->id));
        }
    }
}

// ---- 명령 (요청 처리 중 워커 기동·종료를 기다리지 않는다) ------------------------------------------

namespace {
// 워커 폴더 파일 읽기 오류 → bad_request (Python _files_call)
template <class F>
auto files_call(F&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const WorkerFileError& e) {
        throw ApiError{"bad_request", e.what(), nullptr};
    }
}
}  // namespace

void Supervisor::change_config(Worker& w, const Json& config, const Json* saved, bool temp) {
    const Json old = w.config;
    w.config = config;
    w.temp = temp;
    if (saved) w.saved = *saved;
    if (!py_eq(config, old)) {
        w.config_version += 1;
        if (!w.has_proc()) w.next_start_mono = 0.0;  // 재시작 대기 중이면 새 설정으로 바로
        else try_live(w, old);
    }
    save_versions();
    save_runtime();
    emit(w);
}

OpPlan Supervisor::set_config(const Json& id, const Json& config, bool save) {
    Worker& w = valid(id);
    if (!config.is_object()) throw api_error("bad_request", "config must be an object");
    check_schema(w.type, config);
    check_unique(w.id, config);
    Worker* wp = &w;
    OpPlan plan;
    if (save) {
        auto p = files_call([&] { return w.files->read_preset(w.preset); });
        const Json label = p && py_truthy(p->label) ? p->label : Json("기본");
        plan.pre.push_back(files_call([&] { return w.files->write_preset(w.preset, label, config); }));
        std::optional<WorkerConfig> cur;
        try {
            cur = w.files->read_config();
        } catch (const WorkerFileError& e) {
            throw api_error("internal", e.what());
        }
        if (!cur) plan.pre.push_back(w.files->write_config(w.preset.get<std::string>(), w.autostart));
        plan.commit = [this, wp, config] {
            PM_INFO("%s: saved to preset %s", wp->id.c_str(), py_str(wp->preset).c_str());
            change_config(*wp, config, &config, false);
            return status(*wp, clock_.mono());
        };
    } else {
        plan.commit = [this, wp, config] {
            PM_INFO("%s: applied (temporary, not saved)", wp->id.c_str());
            change_config(*wp, config, nullptr, !py_eq(config, wp->saved));
            return status(*wp, clock_.mono());
        };
    }
    return plan;
}

OpPlan Supervisor::use_preset(const Json& id, const Json& name) {
    Worker& w = valid(id);
    auto preset = files_call([&] { return w.files->read_preset(name); });
    if (!preset) throw api_error("not_found", "no preset " + py_repr(name));
    check_unique(w.id, preset->config);
    OpPlan plan;
    plan.pre.push_back(w.files->write_config(name.get<std::string>(), w.autostart));
    Worker* wp = &w;
    const Json config = preset->config;
    plan.commit = [this, wp, name, config] {
        wp->preset = name;
        PM_INFO("%s: preset %s", wp->id.c_str(), py_str(name).c_str());
        change_config(*wp, config, &config, false);
        return status(*wp, clock_.mono());
    };
    return plan;
}

OpPlan Supervisor::save_preset(const Json& id, const Json& spec) {
    Worker& w = valid(id);
    if (!spec.is_object()) throw api_error("bad_request", "body must be {\"name\": ..., \"label\": ...}");
    const Json name = jget(spec, "name");
    files_call([&] { check_name(name); return 0; });
    if (w.files->has_preset(name)) throw api_error("conflict", "preset " + py_repr(name) + " already exists");
    const Json config = spec.contains("config") ? spec["config"] : w.config;
    if (!config.is_object()) throw api_error("bad_request", "config must be an object");
    check_schema(w.type, config);
    const Json label = py_truthy(jget(spec, "label")) ? spec["label"] : name;
    OpPlan plan;
    plan.pre.push_back(w.files->write_preset(name, label, config));
    Worker* wp = &w;
    const bool use = spec.contains("use") ? py_truthy(spec["use"]) : true;
    if (use) {
        // 만든 프리셋을 사용 중으로(Python은 save_preset → use_preset)
        check_unique(w.id, config);
        plan.pre.push_back(w.files->write_config(name.get<std::string>(), w.autostart));
        plan.commit = [this, wp, name, config] {
            PM_INFO("%s: preset %s created", wp->id.c_str(), py_str(name).c_str());
            wp->preset = name;
            PM_INFO("%s: preset %s", wp->id.c_str(), py_str(name).c_str());
            change_config(*wp, config, &config, false);
            return status(*wp, clock_.mono());
        };
    } else {
        plan.commit = [this, wp, name] {
            PM_INFO("%s: preset %s created", wp->id.c_str(), py_str(name).c_str());
            emit(*wp);
            return status(*wp, clock_.mono());
        };
    }
    return plan;
}

OpPlan Supervisor::rename_preset(const Json& id, const Json& name, const Json& spec_in) {
    Worker& w = valid(id);
    auto preset = files_call([&] { return w.files->read_preset(name); });
    if (!preset) throw api_error("not_found", "no preset " + py_repr(name));
    const Json spec = spec_in.is_object() ? spec_in : Json::object();
    const Json new_name = py_truthy(jget(spec, "name")) ? spec["name"] : name;
    files_call([&] { check_name(new_name); return 0; });
    OpPlan plan;
    bool active = false;
    if (!py_eq(new_name, name)) {
        if (w.files->has_preset(new_name)) throw api_error("conflict", "preset " + py_repr(new_name) + " already exists");
        plan.pre.push_back(w.files->rename_preset(name, new_name));
        if (py_eq(w.preset, name)) {
            active = true;
            plan.pre.push_back(w.files->write_config(new_name.get<std::string>(), w.autostart));
        }
    }
    if (py_truthy(jget(spec, "label"))) plan.pre.push_back(w.files->write_preset(new_name, spec["label"], preset->config));
    Worker* wp = &w;
    plan.commit = [this, wp, active, new_name] {
        if (active) wp->preset = new_name;
        emit(*wp);
        return status(*wp, clock_.mono());
    };
    return plan;
}

OpPlan Supervisor::delete_preset(const Json& id, const Json& name) {
    Worker& w = valid(id);
    if (py_eq(name, w.preset))
        throw api_error("conflict", "preset " + py_repr(name) + " is in use; switch to another preset first");
    bool has = false;
    try {
        has = w.files->has_preset(name);
    } catch (const WorkerFileError& e) {
        throw api_error("internal", e.what());  // Python 판도 여기서는 internal
    }
    if (!has) throw api_error("not_found", "no preset " + py_repr(name));
    OpPlan plan;
    plan.pre.push_back(w.files->delete_preset(name));
    Worker* wp = &w;
    plan.commit = [this, wp, name] {
        PM_INFO("%s: preset %s deleted", wp->id.c_str(), py_str(name).c_str());
        emit(*wp);
        return status(*wp, clock_.mono());
    };
    return plan;
}

OpPlan Supervisor::set_autostart(const Json& id, const Json& value_in) {
    Worker& w = valid(id);
    const Json value = value_in.is_object() ? jget(value_in, "autostart") : value_in;  // REST 본문 {"autostart": true}
    if (!value.is_boolean()) throw api_error("bad_request", "autostart must be true or false");
    OpPlan plan;
    plan.pre.push_back(w.files->write_config(w.preset.get<std::string>(), value.get<bool>()));
    Worker* wp = &w;
    const bool v = value.get<bool>();
    plan.commit = [this, wp, v] {
        wp->autostart = v;
        emit(*wp);
        return status(*wp, clock_.mono());
    };
    return plan;
}

OpPlan Supervisor::start(const Json& id) {
    Worker& w = valid(id);
    if (w.actual == "disabled") throw api_error("conflict", "worker cannot run on this machine (" + py_str(w.last_error) + ")");
    if (!cfg_.types.contains(w.type))
        throw api_error("conflict", py_truthy(w.last_error) ? py_str(w.last_error) : "unknown worker type");
    Worker* wp = &w;
    OpPlan plan;
    plan.commit = [this, wp] {
        wp->desired_state = "running";
        if (!wp->has_proc()) {
            // 재시작 대기 중이어도 명령이 오면 바로 띄운다
            wp->restart_count = 0;
            wp->fail_streak = 0;
            wp->crash_loop = false;
            wp->fail_times.clear();
            wp->next_start_mono = 0.0;
        }
        save_runtime();  // 현재 실행 상태만. 부팅 정책(autostart)은 바꾸지 않는다
        emit(*wp);
        return status(*wp, clock_.mono());
    };
    return plan;
}

OpPlan Supervisor::stop(const Json& id) {
    Worker& w = valid(id);
    Worker* wp = &w;
    OpPlan plan;
    plan.commit = [this, wp] {
        wp->desired_state = "stopped";
        if (!wp->has_proc() && wp->actual != "disabled") wp->actual = "stopped";
        save_runtime();
        emit(*wp);
        return status(*wp, clock_.mono());
    };
    return plan;
}

Json Supervisor::payload(const Worker& w) const {
    Json p = Json::object();
    p["id"] = w.id;
    p["type"] = w.type;
    p["config_version"] = w.config_version;
    p["config"] = w.config;
    return p;
}

FileOp Supervisor::worker_config_op(const Worker& w) const {
    FileOp op;
    op.path = cfg_.paths.worker_config(w.id);
    op.text = dump_file(payload(w));
    op.sync_dir = true;
    return op;
}

void Supervisor::try_live(Worker& w, const Json& old) {
    // 바뀐 키가 전부 type의 live_keys 안이면 다시 시작하지 않고 설정 파일만 바꿔 SIGHUP.
    // 워커가 heartbeat config_version으로 확인하지 않으면 check_alive가 재시작한다
    std::set<std::string> live;
    const Json lk = jget(type_of(w), "live_keys");
    if (lk.is_array())
        for (const auto& k : lk)
            if (k.is_string()) live.insert(k.get<std::string>());
    const auto changed = changed_paths(old, w.config);
    const bool in_sync = (w.applied_version && *w.applied_version == w.config_version - 1) || w.live_version.has_value();
    bool covered = true;
    for (const auto& p : changed) covered = covered && (live.count(p) || live.count(p.substr(0, p.find('.'))));
    if (live.empty() || changed.empty() || !covered || w.actual != "running" || w.stop_reason || !in_sync) return;
    w.live_version = w.config_version;
    w.live_deadline = clock_.mono() + kLiveAckS;
    std::vector<std::string> names(changed.begin(), changed.end());
    PM_INFO("%s live update v%lld (%s), SIGHUP", w.id.c_str(), *w.live_version, join(names, ", ").c_str());
    // 파일이 다 써진 뒤 SIGHUP(워커가 새 파일을 읽게). 그 사이 워커가 바뀌었으면 보내지 않는다
    Worker* wp = &w;
    const int pid = w.pid;
    const long long v = *w.live_version;
    store_.submit({worker_config_op(w)}, [this, wp, pid, v](bool ok, const std::string& err) {
        if (!ok) {
            PM_ERROR("%s live update write failed: %s", wp->id.c_str(), err.c_str());
            return;
        }
        if (wp->pid == pid && wp->live_version == v) send_signal(*wp, SIGHUP);
    });
}

// ---- heartbeat (design.md 9.5) -------------------------------------------------------------------

void Supervisor::keep_early(int pid, const Json& fields) {
    // pid가 아직 어느 워커에도 없음: spawn이 끝나기 전에 온 것일 수 있어 잠시 보관(오래된 것은 버림)
    const double now = clock_.mono();
    for (auto it = early_.begin(); it != early_.end();) {
        if (now - it->second.back().first > kEarlyKeepS) it = early_.erase(it);
        else ++it;
    }
    if (!early_.count(pid) && early_.size() >= kEarlyMax) return;
    early_[pid].emplace_back(now, fields);
}

void Supervisor::drain_early(Worker& w) {
    auto it = early_.find(w.pid);
    if (it == early_.end()) return;
    auto items = std::move(it->second);
    early_.erase(it);
    PM_INFO("%s: %zu heartbeat(s) arrived before spawn finished", w.id.c_str(), items.size());
    for (const auto& [t, fields] : items) on_notify(w.pid, fields);
}

void Supervisor::on_notify(int pid, const Json& fields) {
    Worker* w = nullptr;
    for (auto& x : workers_)
        if (x->has_proc() && x->pid == pid) {
            w = x.get();
            break;
        }
    if (!w) {
        keep_early(pid, fields);
        return;
    }
    if (w->stop_reason) return;
    const double now = clock_.mono();
    if (fields.contains("READY") && w->actual == "starting") {
        w->libworker = jget(fields, "libworker");
        w->sim = jget(fields, "sim") == Json("1");
        w->actual = "running";
        w->last_hb_mono = now;
        PM_INFO("%s ready (pid %d)", w->id.c_str(), pid);
        emit(*w);
    }
    if (fields.contains("WATCHDOG")) {
        w->last_hb_mono = now;
        if (fields.contains("progress")) {
            long long p;
            if (!py_int(py_str(fields["progress"]), p)) return;
            w->progress = p;
        }
        const Json version = fields.contains("config_version") ? py_number(py_str(fields["config_version"])) : Json();
        if (w->live_version && version.is_number_integer() && version.get<long long>() >= *w->live_version) {
            PM_INFO("%s applied config v%lld live", w->id.c_str(), version.get<long long>());
            w->applied_version = version.get<long long>();
            if (version.get<long long>() >= w->config_version) {
                w->live_version.reset();
                w->live_deadline.reset();
            }
            emit(*w);
        }
        Json metrics = Json::object();
        for (auto it = fields.begin(); it != fields.end(); ++it)
            if (!kHbKeys.count(it.key())) metrics[it.key()] = py_number(py_str(*it));
        if (!metrics.empty()) {
            w->metrics = metrics;
            emit(*w);  // 구독자(로거·UI)는 1초마다 지표를 받는다
        }
    }
}

// ---- 루프 ---------------------------------------------------------------------------------------

void Supervisor::tick() {
    const double now = clock_.mono();
    for (size_t i = 0; i < workers_.size(); ++i) {
        Worker& w = *workers_[i];
        auto it = cfg_.types.find(w.type);
        if (w.has_proc()) check_alive(w, it == cfg_.types.end() ? Json::object() : *it, now);
        else if (it != cfg_.types.end() && !w.spawning) check_idle(w, *it, now);
    }
}

std::vector<int> Supervisor::running_pids() const {
    std::vector<int> out;
    for (const auto& w : workers_)
        if (w->has_proc()) out.push_back(w->pid);
    return out;
}

std::map<std::string, int> Supervisor::pids_by_id() const {
    std::map<std::string, int> out;
    for (const auto& w : workers_)
        if (w->has_proc()) out[w->id] = w->pid;
    return out;
}

std::vector<std::string> Supervisor::ids() const {
    std::vector<std::string> out;
    for (const auto& w : workers_) out.push_back(w->id);
    return out;
}

Json Supervisor::config_of(const std::string& id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? Json::object() : it->second->config;
}

std::vector<std::string> Supervisor::restart_changed_inputs() {
    std::vector<std::string> out;
    if (!inputs_for || cfg_.inputs_dir.empty()) return out;
    for (auto& w : workers_) {
        if (!w->has_proc() || w->stop_reason || w->actual != "running") continue;
        Json now = inputs_for(w->id);
        if (!now.is_object()) now = Json::object();
        if (now == w->inputs_injected) continue;
        PM_INFO("%s: inputs changed, restarting", w->id.c_str());
        terminate(*w, "restart");
        out.push_back(w->id);
    }
    return out;
}

std::vector<std::string> Supervisor::rewrite_changed_many() {
    std::vector<std::string> out;
    if (!many_for || cfg_.inputs_dir.empty()) return out;
    std::vector<FileOp> ops;
    for (auto& w : workers_) {
        if (!w->has_proc() || w->stop_reason || w->spawning) continue;
        Json now = many_for(w->id);
        if (!now.is_object()) now = Json::object();
        if (now == w->many_injected) continue;
        w->many_injected = now;
        FileOp in;
        in.path = cfg_.inputs_dir + "/" + w->id + ".json";
        Json doc = Json::object();
        doc["inputs"] = w->inputs_injected;
        if (!now.empty()) doc["many"] = now;
        in.text = dump_file(doc);
        ops.push_back(in);
        PM_INFO("%s: many inputs changed, inputs file rewritten", w->id.c_str());
        out.push_back(w->id);
    }
    if (!ops.empty()) submit_quiet(std::move(ops), "inputs file");
    return out;
}

void Supervisor::apply_resources(const std::map<int, Json>& by_pgid) {
    for (auto& w : workers_) {
        if (!w->has_proc()) continue;
        auto it = by_pgid.find(w->pid);
        if (it != by_pgid.end()) w->resources = it->second;
    }
}

void Supervisor::check_alive(Worker& w, const Json& t, double now) {
    if (w.stop_reason) {
        const double st = tnum(t, "stop_timeout_sec", 5);
        if (!w.killed && now - w.term_mono >= st) {
            PM_WARN("%s did not stop in %ss, SIGKILL", w.id.c_str(), py_str(jget(t, "stop_timeout_sec")).c_str());
            send_signal(w, SIGKILL);
            w.killed = true;
        }
        return;
    }
    if (w.desired_state == "stopped") {
        terminate(w, "requested");
        return;
    }
    if (!w.applied_version || *w.applied_version != w.config_version) {
        if (w.live_version && *w.live_version == w.config_version && w.live_deadline && now < *w.live_deadline)
            return;  // 바로 적용 확인 대기
        if (w.live_version)
            PM_WARN("%s live update v%lld not applied in %gs, restarting", w.id.c_str(), *w.live_version, kLiveAckS);
        w.live_version.reset();
        w.live_deadline.reset();
        terminate(w, "restart");
        return;
    }
    if (w.actual == "starting") {
        if (now - w.spawn_mono.value_or(now) >= tnum(t, "start_timeout_sec", 10)) {
            w.last_error = "stalled: no READY in " + py_str(jget(t, "start_timeout_sec")) + "s";
            terminate(w, "stalled");
        }
        return;
    }
    // 오래 정상으로 돌았으면 재시작 간격·연속 실패 표시를 처음으로
    if ((w.fail_streak || w.crash_loop) && now - w.spawn_mono.value_or(now) >= rnum("healthy_s")) {
        w.fail_streak = 0;
        w.crash_loop = false;
        emit(w);
    }
    // heartbeat만 본다. progress는 장비가 끊기면 정상 워커도 늘지 않으므로 멈춤 판단에 쓰지 않는다(9장)
    if (now - w.last_hb_mono.value_or(now) >= tnum(t, "watchdog_sec", 3)) {
        w.last_error = "stalled: no heartbeat for " + py_str(jget(t, "watchdog_sec")) + "s";
        terminate(w, "stalled");
    }
}

void Supervisor::check_idle(Worker& w, const Json& t, double now) {
    if (w.desired_state != "running" || w.actual == "error" || w.actual == "disabled") return;
    if (now < w.next_start_mono) return;
    if (cfg_.product && w.config.is_object() && w.config.contains("sim")) {
        // 제품에서는 가짜 장치 모드로 띄우지 않는다(실기로 착각할 위험)
        w.actual = "error";
        w.last_error = "sim (fake device) not allowed in product";
        PM_ERROR("%s: %s", w.id.c_str(), w.last_error.get<std::string>().c_str());
        return;
    }
    spawn(w, t);
}

void Supervisor::spawn(Worker& w, const Json& /*t*/) {
    // 설정 파일을 pm.store로 쓰고, 써지면 띄운다. 그 사이 상태가 바뀌었으면 띄우지 않는다(다음 tick이 다시 판단)
    w.spawning = true;
    Worker* wp = &w;
    const long long version = w.config_version;
    const std::string tname = w.type;
    std::vector<FileOp> ops = {worker_config_op(w)};
    // 입력 주소 파일(design.md 6.6.7.4): 설정 파일이 아니라 따로(예전 libworker가 모르는 설정 키에 멈추지 않게)
    if (!cfg_.inputs_dir.empty()) {
        w.inputs_injected = inputs_for ? inputs_for(w.id) : Json::object();
        if (!w.inputs_injected.is_object()) w.inputs_injected = Json::object();
        w.many_injected = many_for ? many_for(w.id) : Json::object();
        if (!w.many_injected.is_object()) w.many_injected = Json::object();
        FileOp in;
        in.path = cfg_.inputs_dir + "/" + w.id + ".json";
        Json doc = Json::object();
        doc["inputs"] = w.inputs_injected;
        if (!w.many_injected.empty()) doc["many"] = w.many_injected;
        in.text = dump_file(doc);
        ops.push_back(in);
    }
    store_.submit(std::move(ops), [this, wp, version, tname](bool ok, const std::string& err) {
        wp->spawning = false;
        if (!ok) {
            wp->last_error = "spawn failed: " + err;
            PM_ERROR("%s spawn failed: %s", wp->id.c_str(), err.c_str());
            on_failure(*wp, clock_.mono());
            emit(*wp);
            return;
        }
        if (wp->desired_state != "running" || wp->has_proc() || wp->actual == "error" || wp->actual == "disabled" ||
            wp->config_version != version || !cfg_.types.contains(tname))
            return;
        do_spawn(*wp, cfg_.types[tname]);
    });
}

void Supervisor::do_spawn(Worker& w, const Json& t) {
    const double now = clock_.mono();
    SpawnSpec spec;
    for (const auto& a : jget(t, "exec")) spec.argv.push_back(py_str(a));
    spec.argv.push_back("--config");
    spec.argv.push_back(cfg_.paths.worker_config(w.id));
    // 실행 파일 후보(type "exec_candidates"): 띄울 때마다 있는 것 중 첫째. 제품 모드에서는 쓰지 않는다
    const Json cands = jget(t, "exec_candidates");
    if (!cfg_.product && cands.is_array())
        for (const auto& c : cands)
            if (c.is_string() && spawner_.executable(c.get<std::string>())) {
                spec.argv[0] = c.get<std::string>();
                break;
            }
    w.exec_path = spec.argv[0];
    const std::string mgr_sock = "WORKSPACE_MANAGER_SOCKET=";
    for (const auto& e : cfg_.base_env) {
        if (e.rfind("WORKSPACE_WORKER_ID=", 0) == 0 || e.rfind("WORKSPACE_NOTIFY_SOCKET=", 0) == 0 ||
            e.rfind(mgr_sock, 0) == 0 || e.rfind("WORKSPACE_PEERS_FILE=", 0) == 0 ||
            e.rfind("WORKSPACE_INPUTS_FILE=", 0) == 0)
            continue;
        spec.env.push_back(e);
    }
    spec.env.push_back("WORKSPACE_WORKER_ID=" + w.id);
    spec.env.push_back("WORKSPACE_NOTIFY_SOCKET=" + cfg_.notify_path);
    // 명령하는 곳 허용 목록(docs/command-path.md 4.2). 설정 파일이 아니라 환경 변수로: 예전 libworker는 모르는 값을 무시
    if (!cfg_.peers_file.empty()) spec.env.push_back("WORKSPACE_PEERS_FILE=" + cfg_.peers_file);
    if (!cfg_.inputs_dir.empty()) spec.env.push_back("WORKSPACE_INPUTS_FILE=" + cfg_.inputs_dir + "/" + w.id + ".json");
    // Manager API를 쓰는 워커에게만 전용 Manager 소켓을 알려 준다(design.md 6.5)
    if (cfg_.manager_api_workers.count(w.id))
        spec.env.push_back(mgr_sock + cfg_.paths.run_dir + "/manager-" + w.id + ".sock");
    spec.log_path = cfg_.paths.log_path(w.id);  // 파이프를 쓰지 않는다: 파이프가 차면 워커가 막힌다(9.4)
    process_options(t, spec.nice, spec.cpus);
    Json proc = Json::object();
    if (spec.nice) proc["nice"] = *spec.nice;
    if (!spec.cpus.empty()) proc["cpu"] = spec.cpus;
    w.process = proc.empty() ? Json(nullptr) : proc;
    std::string err;
    const int pid = spawner_.spawn(spec, err);
    if (pid <= 0) {
        w.last_error = "spawn failed: " + err;
        PM_ERROR("%s spawn failed: %s", w.id.c_str(), err.c_str());
        on_failure(w, now);
        emit(w);
        return;
    }
    w.pid = pid;
    w.actual = "starting";
    w.started_at = clock_.now_iso();
    w.spawn_mono = now;
    w.last_hb_mono.reset();
    w.progress = nullptr;
    w.metrics = nullptr;
    w.resources = nullptr;
    w.applied_version = w.config_version;
    w.live_version.reset();
    w.live_deadline.reset();
    w.stop_reason.reset();
    w.killed = false;
    PM_INFO("%s spawned pid %d (config v%lld)", w.id.c_str(), pid, w.config_version);
    drain_early(w);
    emit(w);
}

void Supervisor::send_signal(Worker& w, int sig) {
    // SIGTERM·SIGHUP은 워커에게만(워커가 자식 정리), SIGKILL은 프로세스 그룹 전체에
    if (!w.has_proc()) return;
    if (sig == SIGKILL) spawner_.kill_group(w.pid, sig);
    else spawner_.signal(w.pid, sig);
}

void Supervisor::terminate(Worker& w, const std::string& reason) {
    w.stop_reason = reason;
    w.term_mono = clock_.mono();
    w.killed = false;
    PM_INFO("%s SIGTERM (%s)", w.id.c_str(), reason.c_str());
    send_signal(w, SIGTERM);
    emit(w);
}

void Supervisor::on_exit(int pid, int rc) {
    Worker* wp = nullptr;
    for (auto& x : workers_)
        if (x->has_proc() && x->pid == pid) {
            wp = x.get();
            break;
        }
    if (!wp) return;
    Worker& w = *wp;
    const double now = clock_.mono();
    const auto reason = w.stop_reason;
    spawner_.kill_group(w.pid, SIGKILL);  // 워커 종료 후 남은 자식(ffmpeg 등) 정리
    w.pid = 0;
    w.metrics = nullptr;  // 지난 실행의 값이 현재 값처럼 보이지 않게
    w.resources = nullptr;
    w.stop_reason.reset();
    if (rc < 0) {
        w.last_exit_code = nullptr;
        w.last_signal = signal_name(-rc);
    } else {
        w.last_exit_code = rc;
        w.last_signal = nullptr;
    }
    PM_INFO("%s exited rc=%d reason=%s", w.id.c_str(), rc, reason ? reason->c_str() : "None");
    if ((reason && kRequested.count(*reason)) || (!reason && w.desired_state == "stopped")) {
        w.actual = "stopped";
        w.next_start_mono = 0.0;
        emit(w);
        return;
    }
    // 이하 실패(6.3 종료 판정 표). 사유만 기록하고 같은 규칙으로 계속 재시작한다
    if (reason && *reason == "stalled") {
        // last_error는 판정 시점에 기록됨
    } else if (rc == 0) {
        w.last_error = "unexpected exit 0";
    } else if (rc == kExitConfig) {
        w.last_error = "config error (exit 1)";
    } else if (rc < 0) {
        w.last_error = "killed by " + w.last_signal.get<std::string>();
    } else {
        w.last_error = "exit " + std::to_string(rc);
    }
    on_failure(w, now);
    emit(w);
}

void Supervisor::on_failure(Worker& w, double now) {
    // 오래 돌다 죽었으면 간격을 처음으로, 빨리 연달아 죽으면 두 배씩(최대 max_interval_s)
    const double ran = w.spawn_mono ? now - *w.spawn_mono : 0.0;
    w.fail_streak = ran >= rnum("healthy_s") ? 0 : w.fail_streak + 1;
    const double interval = std::min(rnum("max_interval_s"), rnum("interval_s") * std::pow(2.0, std::max(0, w.fail_streak - 1)));
    std::vector<double> kept;
    for (double x : w.fail_times)
        if (now - x < rnum("crash_window_s")) kept.push_back(x);
    kept.push_back(now);
    w.fail_times = kept;
    const bool was = w.crash_loop;
    w.crash_loop = static_cast<double>(w.fail_times.size()) >= rnum("crash_count");
    if (w.crash_loop && !was)
        PM_ERROR("%s crash loop: %zu failures in %.0fs (last: %s)", w.id.c_str(), w.fail_times.size(),
                 rnum("crash_window_s"), py_str(w.last_error).c_str());
    w.restart_count += 1;
    w.actual = "exited";
    w.next_start_mono = now + interval;
    PM_WARN("%s failed (%s), restart #%d in %.1fs", w.id.c_str(), py_str(w.last_error).c_str(), w.restart_count, interval);
}

int Supervisor::stop_order(const Worker& w) const {
    const Json& t = type_of(w);
    const Json o = jget(t, "stop_order");
    if (o.is_number_integer()) return o.get<int>();
    return jget(t, "kind") == Json("manager") ? 1 : 0;
}

bool Supervisor::is_manager(const std::string& id) const {
    auto it = by_id_.find(id);
    return it != by_id_.end() && jget(type_of(*it->second), "kind") == Json("manager");
}

std::vector<int> Supervisor::stop_orders() const {
    std::set<int> s;
    for (const auto& w : workers_)
        if (w->has_proc()) s.insert(stop_order(*w));
    return {s.begin(), s.end()};
}

double Supervisor::shutdown_begin(int order) {
    double deadline = 0;
    for (auto& w : workers_) {
        if (!w->has_proc() || stop_order(*w) != order) continue;
        if (!w->stop_reason) terminate(*w, "shutdown");
        deadline = std::max(deadline, tnum(type_of(*w), "stop_timeout_sec", 5));
    }
    return deadline;
}

bool Supervisor::any_running(int order) const {
    for (const auto& w : workers_)
        if (w->has_proc() && stop_order(*w) == order) return true;
    return false;
}

double Supervisor::shutdown_begin() {
    double deadline = 0;
    for (auto& w : workers_) {
        if (!w->has_proc()) continue;
        if (!w->stop_reason) terminate(*w, "shutdown");
        deadline = std::max(deadline, tnum(type_of(*w), "stop_timeout_sec", 5));
    }
    return deadline;
}

void Supervisor::shutdown_kill() {
    for (auto& w : workers_)
        if (w->has_proc()) send_signal(*w, SIGKILL);
}

bool Supervisor::any_running() const {
    for (const auto& w : workers_)
        if (w->has_proc()) return true;
    return false;
}

}  // namespace pm
