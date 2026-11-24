#include "record/store.hpp"

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

namespace record {

namespace {

const char* kSuffix = ".jsonl";

double mono_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

bool ends_with(const std::string& s, const std::string& end) {
    return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

std::vector<std::string> list_records(const std::string& dir) {
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (ends_with(n, kSuffix)) names.push_back(n);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void make_dirs(const std::string& dir) {
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
        }
        if (i < dir.size()) cur += dir[i];
    }
}

std::string stamp_local() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

bool glob_match(const Json& pattern, const Json& value) {
    if (!pattern.is_string()) return true;
    if (!value.is_string()) return false;
    return fnmatch(pattern.get<std::string>().c_str(), value.get<std::string>().c_str(), 0) == 0;
}

}  // namespace

std::string boot_id() {
    std::ifstream f("/proc/sys/kernel/random/boot_id");
    std::string s;
    if (!f || !std::getline(f, s)) return "noboot00";
    s.erase(std::remove(s.begin(), s.end(), '-'), s.end());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s.size() >= 8 ? s.substr(0, 8) : "noboot00";
}

std::string now_iso() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    std::tm tm{};
    localtime_r(&ts.tv_sec, &tm);
    char base[32];
    std::strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tm);
    const long off = tm.tm_gmtoff;
    const long a = off < 0 ? -off : off;
    char out[96];
    std::snprintf(out, sizeof out, "%s.%03ld%c%02ld:%02ld", base, ts.tv_nsec / 1000000, off < 0 ? '-' : '+', a / 3600,
                  (a % 3600) / 60);
    return out;
}

int64_t mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

std::string make_line(const Json& rec, const std::string& t_iso, int64_t t_mono_ns) {
    Json line = Json::object();
    line["t"] = t_iso;
    line["t_mono_ns"] = t_mono_ns;
    if (rec.is_object())
        for (auto it = rec.begin(); it != rec.end(); ++it)
            if (it.key() != "t" && it.key() != "t_mono_ns") line[it.key()] = it.value();
    return line.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
}

RecordFile::RecordFile(std::string dir, uint64_t max_file_bytes, uint64_t max_total_bytes, double fsync_s,
                       std::string boot)
    : dir_(std::move(dir)), boot_(std::move(boot)), max_file_(max_file_bytes), max_total_(max_total_bytes),
      fsync_s_(fsync_s) {
    make_dirs(dir_);
    std::string last;
    for (const auto& n : list_records(dir_))
        if (ends_with(n, "_" + boot_ + kSuffix)) last = n;  // 이 부팅의 마지막 파일에 이어 씀
    open(last.empty() ? "" : dir_ + "/" + last, nullptr);
}

RecordFile::~RecordFile() { close(); }

bool RecordFile::open(const std::string& path_in, std::string* error) {
    if (f_) {
        sync();
        std::fclose(f_);
        f_ = nullptr;
    }
    make_dirs(dir_);  // 데이터 폴더가 지워졌어도 다시 만든다
    std::string path = path_in;
    if (path.empty()) {
        path = dir_ + "/" + stamp_local() + "_" + boot_ + kSuffix;
        if (path == path_) {  // 같은 초에 넘어가면 이름이 겹친다
            timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            path = path.substr(0, path.size() - std::strlen(kSuffix)) + "-" + std::to_string(ts.tv_nsec / 1000000) + kSuffix;
        }
    }
    f_ = std::fopen(path.c_str(), "a");
    if (!f_) {
        if (error) *error = "open " + path + ": " + std::strerror(errno);
        return false;
    }
    path_ = path;
    struct stat st {};
    size_ = ::stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
    ++files_opened_;
    prune();
    return true;
}

void RecordFile::prune() {
    struct Item {
        std::string path;
        uint64_t size;
    };
    std::vector<Item> files;
    uint64_t total = 0;
    for (const auto& n : list_records(dir_)) {
        const std::string p = dir_ + "/" + n;
        struct stat st {};
        if (::stat(p.c_str(), &st) != 0) continue;
        files.push_back({p, static_cast<uint64_t>(st.st_size)});
        total += st.st_size;
    }
    for (const auto& it : files) {
        if (total <= max_total_) break;
        if (it.path == path_) continue;
        if (::unlink(it.path.c_str()) == 0) {
            total -= it.size;
            ++removed_;
        }
    }
}

void RecordFile::sync() {
    if (!f_) return;
    std::fflush(f_);
    ::fsync(fileno(f_));
    last_sync_ = mono_s();
}

bool RecordFile::write(const std::string& line, std::string* error) {
    if (!f_ && !open("", error)) return false;
    if (std::fwrite(line.data(), 1, line.size(), f_) != line.size() || std::fflush(f_) != 0) {
        if (error) *error = std::string("write: ") + std::strerror(errno);
        std::fclose(f_);
        f_ = nullptr;  // 다음 줄에서 새로 연다
        return false;
    }
    size_ += line.size();
    if (mono_s() - last_sync_ >= fsync_s_) sync();
    if (size_ >= max_file_) open("", nullptr);
    return true;
}

void RecordFile::close() {
    if (!f_) return;
    sync();
    std::fclose(f_);
    f_ = nullptr;
}

Json find(const std::string& dir, const Json& q, const std::string& boot) {
    auto str = [&](const char* k) { return q.contains(k) && q[k].is_string() ? q[k].get<std::string>() : std::string(); };
    auto num = [&](const char* k, int64_t def) { return q.contains(k) && q[k].is_number() ? q[k].get<int64_t>() : def; };
    const std::string since = str("since"), until = str("until");
    const int64_t since_ns = num("since_ns", -1), until_ns = num("until_ns", -1);
    const bool by_mono = since_ns >= 0 || until_ns >= 0;
    int64_t limit = num("limit", 1000);
    limit = std::max<int64_t>(1, std::min<int64_t>(limit, 5000));
    const Json types = q.contains("types") && q["types"].is_array() ? q["types"] : Json();
    const Json src = q.contains("src") ? q["src"] : Json(), topic = q.contains("topic") ? q["topic"] : Json();

    Json lines = Json::array();
    bool more = false;
    int files = 0;
    for (const auto& n : list_records(dir)) {
        if (by_mono && !ends_with(n, "_" + boot + kSuffix)) continue;  // 단조 시계는 이 부팅에서만 뜻이 있음
        std::ifstream f(dir + "/" + n);
        if (!f) continue;
        ++files;
        std::string text;
        while (std::getline(f, text)) {
            if (text.empty()) continue;
            Json r = Json::parse(text, nullptr, false);
            if (!r.is_object()) continue;
            const std::string t = r.contains("t") && r["t"].is_string() ? r["t"].get<std::string>() : "";
            if (!since.empty() && t < since) continue;
            if (!until.empty() && t > until && t.compare(0, until.size(), until) != 0) continue;  // until은 앞부분 포함
            if (by_mono) {
                if (!r.contains("t_mono_ns") || !r["t_mono_ns"].is_number()) continue;
                const int64_t m = r["t_mono_ns"].get<int64_t>();
                if (since_ns >= 0 && m < since_ns) continue;
                if (until_ns >= 0 && m > until_ns) continue;
            }
            if (types.is_array()) {
                const Json ty = r.contains("type") ? r["type"] : Json();
                if (std::find(types.begin(), types.end(), ty) == types.end()) continue;
            }
            if (!src.is_null()) {
                const Json s = r.contains("src") ? r["src"] : (r.contains("id") ? r["id"] : Json());
                if (!glob_match(src, s)) continue;
            }
            if (!topic.is_null() && !glob_match(topic, r.contains("topic") ? r["topic"] : Json())) continue;
            if (static_cast<int64_t>(lines.size()) >= limit) {
                more = true;
                break;
            }
            lines.push_back(std::move(r));
        }
        if (more) break;
    }
    Json out = Json::object();
    out["lines"] = std::move(lines);
    out["more"] = more;
    out["files"] = files;
    return out;
}

}  // namespace record
