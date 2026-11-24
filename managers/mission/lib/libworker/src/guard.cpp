#include "worker/guard.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

namespace worker {

using nlohmann::json;

CommandGuard::CommandGuard(std::string worker_id, std::string path) : id_(std::move(worker_id)), path_(std::move(path)) {}

void CommandGuard::reload() {
    struct stat st {};
    const long long m = ::stat(path_.c_str(), &st) == 0
                            ? static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec
                            : 0;
    if (m == mtime_ns_) return;
    mtime_ns_ = m;
    if (m == 0) {  // 표가 아직 없음(process-manager가 곧 씀): warn처럼
        loaded_ = false;
        return;
    }
    json doc;
    try {
        std::ifstream in(path_);
        doc = json::parse(in);
    } catch (const std::exception&) {
        return;  // 깨졌거나 쓰는 중: 직전 표를 그대로(처음이면 warn처럼)
    }
    if (!doc.is_object()) return;
    std::map<std::string, Peer> peers;
    std::set<std::string> read;
    std::map<std::string, std::set<std::string>> allow;
    std::set<std::string> all;
    if (doc.contains("peers") && doc["peers"].is_object())
        for (const auto& [who, p] : doc["peers"].items()) {
            if (!p.is_object()) continue;
            Peer x;
            x.pid = p.value("pid", 0);
            x.pgid = p.value("pgid", 0);
            x.trusted_from = p.value("trusted_from", false);
            peers[who] = x;
        }
    const json w = doc.contains("workers") && doc["workers"].is_object() && doc["workers"].contains(id_)
                       ? doc["workers"][id_]
                       : json::object();
    if (w.contains("read") && w["read"].is_array())
        for (const auto& c : w["read"])
            if (c.is_string()) read.insert(c.get<std::string>());
    if (w.contains("allow") && w["allow"].is_object())
        for (const auto& [who, a] : w["allow"].items()) {
            if (a.is_string() && a.get<std::string>() == "*") all.insert(who);
            else if (a.is_array())
                for (const auto& c : a)
                    if (c.is_string()) allow[who].insert(c.get<std::string>());
        }
    guard_ = doc.value("guard", std::string("warn"));
    if (guard_ != "off" && guard_ != "warn" && guard_ != "enforce") guard_ = "warn";
    peers_ = std::move(peers);
    read_ = std::move(read);
    allow_ = std::move(allow);
    allow_all_ = std::move(all);
    loaded_ = true;
}

CommandGuard::Decision CommandGuard::check(int pid, const std::string& cmd, const std::string& claimed_from) {
    Decision d;
    if (path_.empty()) {  // 단독 실행: 보지 않음
        d.read = cmd == "get_status";
        d.from = claimed_from;
        return d;
    }
    reload();
    // 누구인가: pid가 같은 곳, 아니면 프로세스 그룹이 같은 곳
    std::string who;
    bool trusted = false;
    const int pgid = pid > 0 ? static_cast<int>(::getpgid(pid)) : -1;
    for (const auto& [name, p] : peers_)
        if ((p.pid > 0 && p.pid == pid) || (p.pgid > 0 && p.pgid == pgid)) {
            who = name;
            trusted = p.trusted_from;
            break;
        }
    d.trusted = trusted;
    if (who.empty()) d.from = "unknown:" + std::to_string(pid);
    else if (trusted) d.from = claimed_from.empty() ? who : claimed_from;
    else d.from = who;  // 믿지 않는 곳이 적은 from은 무시
    d.read = cmd == "get_status" || read_.count(cmd) > 0;
    if (d.read) return d;
    if (!loaded_) {  // 표가 없거나 아직 못 읽음: 실행하고 기록(목표 ④·6장)
        d.violation = true;
        d.reason = "no peers table yet";
        return d;
    }
    if (guard_ == "off") return d;
    const bool ok = !who.empty() && (allow_all_.count(who) || (allow_.count(who) && allow_.at(who).count(cmd)));
    if (ok) return d;
    d.violation = true;
    d.reason = who.empty() ? "unknown process (pid " + std::to_string(pid) + ")" : who + " is not allowed to send " + cmd;
    d.run = guard_ != "enforce";
    return d;
}

}  // namespace worker
