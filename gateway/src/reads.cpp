#include "reads.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <thread>

#include "board_parse.hpp"
#include "history.hpp"
#include "profile_rules.hpp"
#include "proc.hpp"
#include "sys.hpp"
#include "ui_rules.hpp"

namespace gw {

namespace {

constexpr double kTaskSampleS = 0.4;  // 스레드별 CPU를 잴 간격
constexpr size_t kMaxThreads = 100;   // 프로세스 하나에서 돌려줄 스레드 수 상한(CPU 큰 순)
constexpr double kGitCacheS = 30;     // git status가 느릴 수 있어 재사용

long long int_param(const Params& p, const char* key, long long dflt, long long lo, long long hi) {
    auto it = p.find(key);
    long long v = dflt;
    if (it != p.end() && !py_int(it->second, v)) throw BadRequest(std::string(key) + " must be an integer");
    return std::max(lo, std::min(v, hi));
}

// 파일 끝 n줄(큰 파일도 끝부분만 읽음, reads._tail과 같은 방식)
std::vector<std::string> tail(const std::string& path, long long n) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    struct stat st{};
    ::fstat(fd, &st);
    off_t size = st.st_size;
    std::string data;
    const off_t block = 65536;
    while (size > 0 && static_cast<long long>(std::count(data.begin(), data.end(), '\n')) <= n) {
        const off_t step = std::min(block, size);
        size -= step;
        std::string chunk(static_cast<size_t>(step), '\0');
        const ssize_t got = ::pread(fd, &chunk[0], static_cast<size_t>(step), size);
        if (got <= 0) break;
        chunk.resize(static_cast<size_t>(got));
        data = chunk + data;
    }
    ::close(fd);
    auto lines = splitlines(utf8_replace(data));
    if (static_cast<long long>(lines.size()) > n) lines.erase(lines.begin(), lines.end() - n);
    return lines;
}

// 이름 순 *.jsonl(숨김 제외, glob과 같음)
std::vector<std::string> jsonl_files(const std::string& dir) {
    std::vector<std::string> out;
    for (const auto& n : list_dir(dir))
        if (!starts_with(n, ".") && ends_with(n, ".jsonl")) out.push_back(path_join(dir, n));
    std::sort(out.begin(), out.end());
    return out;
}

// 파일 끝의 줄 n개(빈 줄·CR 뺌, keep이 있으면 맞는 줄만). 끝에서 조금씩 거슬러 읽는다(큰 기록 파일을 다
// 읽지 않음, 많아야 끝 64 MiB)
std::vector<std::string> tail_lines(const std::string& path, size_t n,
                                    const std::function<bool(const std::string&)>& keep = nullptr) {
    std::vector<std::string> out;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0 || n == 0) {
        if (fd >= 0) ::close(fd);
        return out;
    }
    struct stat st {};
    ::fstat(fd, &st);
    off_t end = st.st_size;
    std::string buf;
    size_t want = n * 400 + 4096;
    while (true) {
        const off_t start = end > static_cast<off_t>(want) ? end - static_cast<off_t>(want) : 0;
        buf.assign(static_cast<size_t>(end - start), '\0');
        const ssize_t got = ::pread(fd, &buf[0], buf.size(), start);
        if (got < 0) break;
        buf.resize(static_cast<size_t>(got));
        size_t from = 0;
        if (start > 0) {  // 첫 줄은 잘렸을 수 있음
            const size_t nl = buf.find('\n');
            from = nl == std::string::npos ? buf.size() : nl + 1;
        }
        out.clear();
        for (auto& l : splitlines(buf.substr(from)))
            if (!l.empty() && (!keep || keep(l))) out.push_back(utf8_replace(l));
        if (out.size() >= n || start == 0 || want >= (64u << 20)) break;
        want *= 4;
    }
    ::close(fd);
    if (out.size() > n) out.erase(out.begin(), out.end() - static_cast<long>(n));
    return out;
}

// encoding="ascii", errors="replace": ASCII 밖 바이트마다 U+FFFD
std::string ascii_replace(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x80) out += static_cast<char>(c);
        else out += "\xEF\xBF\xBD";
    }
    return out;
}

long long to_int(const std::string& s) {
    long long v;
    if (!py_int(s, v)) throw std::runtime_error("invalid literal for int() with base 10: '" + s + "'");
    return v;
}

std::string proc_dead_text(const std::string& cmd, int err) { return errno_text(err) + ": '" + cmd + "'"; }

}  // namespace

ReadResult Reads::handle(const std::string& name, const std::string& id, bool has_id, const Params& params) {
    if (has_id && !valid_worker_id(id)) throw BadRequest("bad worker id");
    ReadResult r;
    if (name == "workspace") {
        r.body = workspace_info();
        return r;
    }
    if (name == "worker_log") {
        Json lines = Json::array();
        for (auto& l : tail(path_join(path_join(path_join(data_, "state"), "logs"), id + ".log"),
                            int_param(params, "lines", 200, 1, 5000)))
            lines.push_back(l);
        r.body = Json{{"lines", lines}};
        return r;
    }
    if (name == "service_log") {
        const long long n = int_param(params, "lines", 200, 1, 5000);
        const std::vector<std::string> argv = {"journalctl", "-u", "workspace", "-n", std::to_string(n),
                                               "--no-pager", "-o", "short-iso"};
        const RunResult p = run_capture(argv, 10);
        if (!p.started || p.timed_out) {
            const std::string why = !p.started ? proc_dead_text("journalctl", p.spawn_err)
                                               : "Command timed out after 10 seconds";
            r.status = 503;
            r.body = Json{{"error", {{"code", "unavailable"}, {"message", "journalctl: " + why}}}};
            return r;
        }
        Json lines = Json::array();
        for (auto& l : splitlines(utf8_replace(p.out.empty() ? p.err : p.out))) lines.push_back(l);
        r.body = Json{{"lines", lines}};
        return r;
    }
    if (name == "commands") {
        // 명령 이력: 게이트웨이 명령 기록(<데이터>/records/gateway/gateway-*.jsonl, docs/gateway.md)의 끝에서 n줄.
        // 새 파일(수정 시각 순)부터 거슬러 읽고, 파일마다 끝부분만 읽는다(파일 전체를 훑지 않음)
        const long long n = int_param(params, "lines", 200, 1, 5000);
        auto files = jsonl_files(path_join(path_join(data_, "records"), "gateway"));
        std::vector<std::pair<long long, std::string>> by_time;
        for (const auto& f : files) {
            struct stat st {};
            if (::stat(f.c_str(), &st) == 0) by_time.push_back({static_cast<long long>(st.st_mtime) * 1000000000LL + st.st_mtim.tv_nsec, f});
        }
        std::sort(by_time.begin(), by_time.end());
        std::deque<std::string> out;
        for (auto it = by_time.rbegin(); it != by_time.rend() && static_cast<long long>(out.size()) < n; ++it) {
            // 읽기 명령 줄은 뺀다(0.81 전 게이트웨이가 남긴 get_status가 같은 부팅의 파일에 남아 있음)
            const auto got = tail_lines(it->second, static_cast<size_t>(n - static_cast<long long>(out.size())),
                                        [](const std::string& l) {
                                            if (l.find("get_status") == std::string::npos) return true;
                                            const Json j = Json::parse(l, nullptr, false);
                                            return !(j.is_object() && j.value("cmd", Json()) == "get_status");
                                        });
            out.insert(out.begin(), got.begin(), got.end());
        }
        r.body = Json{{"lines", Json(std::vector<std::string>(out.begin(), out.end()))}};
        return r;
    }
    if (name == "processes") {
        const long long n = int_param(params, "lines", 12, 1, 50);
        const RunResult p = run_capture({"ps", "-eo", "pid,comm,%cpu,%mem,rss", "--sort=-%cpu", "--no-headers"}, 5);
        if (!p.started) throw std::runtime_error(proc_dead_text("ps", p.spawn_err));
        if (p.timed_out) throw std::runtime_error("Command 'ps' timed out after 5 seconds");
        try {
            r.body = Json{{"processes", parse_ps(utf8_replace(p.out), n)}};
        } catch (const std::invalid_argument& e) {
            throw std::runtime_error(e.what());
        }
        return r;
    }
    if (name == "power_modes") {
        auto text = read_file("/etc/nvpmodel.conf", 1 << 20);
        r.body = Json{{"modes", text ? parse_nvpmodel(universal_newlines(utf8_replace(*text))) : Json::object()}};
        return r;
    }
    if (name == "worker_tasks") {
        const long long pgid = int_param(params, "pid", 0, 0, 1LL << 22);
        if (pgid <= 1) throw BadRequest("pid is required (worker pid)");
        r.body = Json{{"pid", pgid}, {"processes", group_tasks(pgid)}};
        return r;
    }
    if (name == "history") {
        double minutes = 60;
        auto mi = params.find("minutes");
        if (mi != params.end() && !py_float(mi->second, minutes)) throw BadRequest("minutes must be a number");
        minutes = py_max(1.0, py_min(minutes, 7 * 24 * 60));
        const long long points = int_param(params, "points", 360, 0, 5000);
        auto ki = params.find("keys");
        const std::string keys_raw = ki == params.end() ? "" : ki->second;
        for (char c : drop_final_newline(keys_raw))
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ','))
                throw BadRequest("bad keys");
        std::vector<std::string> keys;
        size_t start = 0;
        while (start <= keys_raw.size()) {
            size_t comma = keys_raw.find(',', start);
            if (comma == std::string::npos) comma = keys_raw.size();
            if (comma > start) keys.push_back(keys_raw.substr(start, comma - start));
            start = comma + 1;
        }
        const double now = wall_now(), since = now - minutes * 60;
        const std::string needle = "\"id\":\"" + id + "\"";
        std::vector<HistRow> rows;
        // 파일 이름이 시작 시각 순이다. 기간 안에 쓰인(수정된) 파일만 읽는다
        for (const auto& f : jsonl_files(path_join(data_, "records"))) {
            const double mt = mtime_of(f);
            if (mt < 0 || mt < since) continue;
            for_each_line(f, [&](const std::string& line) {
                HistRow row;
                if (history_row(line, needle, since, &keys, row)) rows.push_back(std::move(row));
                return true;
            });
        }
        rows = history_reduce(std::move(rows), now, since, static_cast<int>(points));
        auto ci = params.find("csv");
        const bool csv = ci != params.end() && ci->second == "1";
        r.body = Json{{"lines", Json(history_render(rows, keys.empty() ? nullptr : &keys, csv))}};
        return r;
    }
    throw std::runtime_error("unknown read " + name);
}

// ---- 워커 프로세스·스레드 ----

namespace {
struct ThreadSample {
    long long tid;
    std::string name;
    long long ticks, nice, rt, policy;
    std::string owner;
};
struct ProcSample {
    std::string name;
    long long ppid, rss_pages;
    std::vector<ThreadSample> threads;  // /proc/<pid>/task 읽은 차례
};

std::map<long long, ProcSample> snapshot(long long pgid) {
    std::map<long long, ProcSample> procs;
    const std::string want = std::to_string(pgid);
    for (const auto& name : list_dir("/proc")) {
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        auto data = read_file("/proc/" + name + "/stat", 4096);
        std::string comm;
        std::vector<std::string> f;
        if (!data || !parse_proc_stat(ascii_replace(*data), comm, f) || f.size() < 22 || f[2] != want) continue;
        ProcSample p;
        p.name = comm;
        for (const auto& tid : list_dir("/proc/" + name + "/task")) {
            auto td = read_file("/proc/" + name + "/task/" + tid + "/stat", 4096);
            std::string tcomm;
            std::vector<std::string> tf;
            if (!td || !parse_proc_stat(ascii_replace(*td), tcomm, tf) || tf.size() <= 38) continue;
            // [11]+[12] utime+stime, [16] nice, [37] rt_priority, [38] policy (stat 14·15·19·40·41번째 칸)
            p.threads.push_back({to_int(tid), tcomm, to_int(tf[11]) + to_int(tf[12]), to_int(tf[16]), to_int(tf[37]),
                                 to_int(tf[38]), name});
        }
        p.ppid = to_int(f[1]);
        p.rss_pages = to_int(f[21]);
        procs[to_int(name)] = std::move(p);
    }
    return procs;
}

Json cpus_allowed(const std::string& pid, long long tid) {
    Json out = nullptr;
    for_each_line("/proc/" + pid + "/task/" + std::to_string(tid) + "/status", [&](const std::string& line) {
        if (starts_with(line, "Cpus_allowed_list:")) {
            out = strip(line.substr(line.find(':') + 1));
            return false;
        }
        return true;
    });
    return out;
}
}  // namespace

Json Reads::group_tasks(long long pgid) {
    static const long clk = ::sysconf(_SC_CLK_TCK);
    static const long page = ::sysconf(_SC_PAGESIZE);
    const auto a = snapshot(pgid);
    std::this_thread::sleep_for(std::chrono::duration<double>(kTaskSampleS));
    const auto b = snapshot(pgid);
    Json out = Json::array();
    for (const auto& kv : b) {
        std::map<long long, long long> prev;
        auto pa = a.find(kv.first);
        if (pa != a.end())
            for (const auto& t : pa->second.threads) prev[t.tid] = t.ticks;
        std::vector<Json> threads;
        for (const auto& t : kv.second.threads) {
            Json cpu = nullptr;
            auto pt = prev.find(t.tid);
            if (pt != prev.end())
                cpu = py_round(static_cast<double>(std::max(t.ticks - pt->second, 0LL)) / clk / kTaskSampleS * 100, 1);
            Json j = Json::object();
            j["tid"] = t.tid;
            j["name"] = t.name;
            j["cpu"] = cpu;
            j["policy"] = policy_name(t.policy);
            j["rt_priority"] = t.rt;
            j["nice"] = t.nice;
            j["cpus"] = cpus_allowed(t.owner, t.tid);
            threads.push_back(std::move(j));
        }
        auto cpu_of = [](const Json& t) { return t["cpu"].is_null() ? 0.0 : t["cpu"].get<double>(); };
        std::stable_sort(threads.begin(), threads.end(), [&](const Json& x, const Json& y) { return -cpu_of(x) < -cpu_of(y); });
        double total = 0;
        for (const auto& t : threads) total += cpu_of(t);
        Json p = Json::object();
        p["pid"] = kv.first;
        p["ppid"] = kv.second.ppid;
        p["name"] = kv.second.name;
        p["rss_mb"] = py_round(static_cast<double>(kv.second.rss_pages) * page / (1 << 20), 1);
        p["cpu"] = py_round(total, 1);
        p["thread_count"] = threads.size();
        Json tl = Json::array();
        for (size_t i = 0; i < threads.size() && i < kMaxThreads; ++i) tl.push_back(threads[i]);
        p["threads"] = tl;
        out.push_back(std::move(p));
    }
    return out;
}

// ---- 워크스페이스·프로필 (profile.py) ----

namespace {
std::optional<std::string> rd(const std::string& path) { return read_text(path, kMaxText); }

std::vector<std::string> profile_files(const std::string& folder) {
    std::vector<std::string> out;
    if (folder.empty() || !is_dir(folder)) return out;
    for (const auto& n : list_dir(folder))
        if (profile_file_name(n) && is_file(path_join(folder, n))) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
}

Json git_out(const std::string& root, std::vector<std::string> args) {
    // --no-optional-locks: status가 index.lock을 잡지 않게(시간 초과로 끊겨도 git pull을 막지 않음)
    std::vector<std::string> argv = {"git", "--no-optional-locks", "-c", "safe.directory=" + root, "-C", root};
    argv.insert(argv.end(), args.begin(), args.end());
    const RunResult p = run_capture(argv, 3);
    if (!p.started || p.timed_out || p.code != 0) return nullptr;
    return strip(utf8_replace(p.out));
}
}  // namespace

Json Reads::git_info() {
    const double now = mono_now();
    {
        std::lock_guard<std::mutex> lock(git_mu_);
        if (!git_v_.is_null() && now - git_t_ <= kGitCacheS) return git_v_;
    }
    Json v = Json::object();
    v["root"] = workspace_;
    v["commit"] = git_out(workspace_, {"rev-parse", "--short", "HEAD"});
    v["branch"] = git_out(workspace_, {"rev-parse", "--abbrev-ref", "HEAD"});
    const Json st = git_out(workspace_, {"status", "--porcelain", "--untracked-files=no"});
    v["dirty"] = st.is_null() ? Json(nullptr) : Json(!st.get<std::string>().empty());  // null = 확인 못 함
    std::lock_guard<std::mutex> lock(git_mu_);
    git_v_ = v;
    git_t_ = now;
    return v;
}

Json Reads::workspace_info() {
    const std::string cfg = path_join(data_, "config");
    const std::string defaults_dir = path_join(path_join(workspace_, "install"), "config");

    Json profile = nullptr;
    auto raw = rd(path_join(cfg, "profile.json"));
    if (raw && !raw->empty()) {
        auto j = parse_json(*raw);
        profile = j ? *j : Json{{"error", "profile.json을 읽지 못함"}};
    }
    std::string src_dir;
    if (!profile.is_null()) {
        if (!profile.is_object()) throw std::runtime_error("profile.json is not an object");
        auto it = profile.find("source");
        if (it != profile.end() && py_truthy(*it)) {
            if (!it->is_string()) throw std::runtime_error("profile source must be a string");
            const std::string p = it->get<std::string>();
            src_dir = (!p.empty() && p[0] == '/') ? p : path_join(workspace_, p);
            if (!is_dir(src_dir)) src_dir.clear();
        }
    }
    if (profile.is_null()) {
        // 기록 전에 설치한 보드: 보드 파일과 가장 많이 같은 프로필을 추정(비교에만 쓴다)
        const std::string pdir = path_join(path_join(workspace_, "install"), "profiles");
        std::vector<std::string> dirs;
        for (const auto& n : list_dir(pdir))
            if (!starts_with(n, ".") && is_dir(path_join(pdir, n))) dirs.push_back(path_join(pdir, n) + "/");
        std::sort(dirs.begin(), dirs.end());
        std::string best;
        long long best_n = 0;
        for (const auto& d : dirs) {
            long long n = 0;
            for (const auto& f : profile_files(d)) {
                auto board = rd(path_join(cfg, f));
                if (board && rd(path_join(d, f)) == board) ++n;
            }
            if (n > best_n) best = d, best_n = n;
        }
        if (!best.empty()) {
            src_dir = rstrip_char(best, '/');
            profile = Json::object();
            profile["name"] = basename(src_dir);
            profile["source"] = relpath(src_dir, workspace_);
            profile["guessed"] = true;
            profile["matching_files"] = best_n;
        }
    }

    Json files = Json::array();
    std::vector<std::string> names = profile_files(defaults_dir);
    for (const auto& f : profile_files(src_dir))
        if (std::find(names.begin(), names.end(), f) == names.end()) names.push_back(f);
    for (const auto& name : names) {
        auto board = rd(path_join(cfg, name));
        auto prof = src_dir.empty() ? std::nullopt : rd(path_join(src_dir, name));
        auto dflt = rd(path_join(defaults_dir, name));
        Json f = Json::object();
        f["name"] = name;
        f["path"] = path_join(cfg, name);
        f["exists"] = board.has_value();
        f["in_profile"] = prof.has_value();
        f["origin"] = file_origin(board, prof, dflt, !src_dir.empty());
        f["truncated"] = board.has_value() && utf8_count(*board) > kMaxText;
        f["text"] = board ? Json(mask_text(utf8_prefix(*board, kMaxText))) : Json(nullptr);
        files.push_back(std::move(f));
    }

    const Json env_board = env_values(rd(path_join(cfg, "workspace.env")).value_or(""));
    const Json env_prof = src_dir.empty() ? Json::object() : env_values(rd(path_join(src_dir, "workspace.env")).value_or(""));
    Json env = env_doc(rd(path_join(defaults_dir, "workspace.env")).value_or(""));
    std::vector<std::string> seen;
    for (const auto& e : env) seen.push_back(e["key"].get<std::string>());
    for (auto it = env_board.begin(); it != env_board.end(); ++it) {
        if (std::find(seen.begin(), seen.end(), it.key()) != seen.end()) continue;
        Json e = Json::object();
        e["key"] = it.key();
        e["default"] = nullptr;
        e["example"] = nullptr;
        e["desc"] = "";
        env.push_back(e);
    }
    env_finish(env, env_board, env_prof);

    Json out = Json::object();
    out["workspace"] = git_info();
    out["data_dir"] = data_;
    out["profile"] = profile;
    out["files"] = files;
    out["env"] = env;
    return out;
}

}  // namespace gw
