// 실행 파일에 넣은 입출력 정의 → 정의 묶음(io.hpp). 모으는 규칙은 process-manager Supervisor::get_io와 같다
#include "worker/io.hpp"

#include <map>

namespace worker {

namespace {

std::map<std::string, std::string>& files() {
    static std::map<std::string, std::string> m;
    return m;
}

// "./a/../b.json" → "b.json". 워커 폴더 밖(앞의 ..)이면 빈 문자열
std::string normalize(const std::string& path) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        const std::string p = path.substr(i, j - i);
        if (p == "..") {
            if (parts.empty()) return "";
            parts.pop_back();
        } else if (!p.empty() && p != ".") {
            parts.push_back(p);
        }
        i = j + 1;
    }
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

}  // namespace

bool register_io_files(const IoFiles& list) {
    for (const auto& [path, text] : list) files()[path] = text;
    return true;
}

size_t io_file_count() { return files().size(); }

nlohmann::ordered_json io_bundle(std::string* error) {
    using J = nlohmann::ordered_json;
    std::string err;
    auto load = [&](const std::string& path, const std::string& what, J* out) {
        auto it = files().find(path);
        if (it == files().end()) {
            err = what + ": not in the executable (" + path + ")";
            return false;
        }
        try {
            *out = J::parse(it->second);
        } catch (const std::exception& e) {
            err = what + ": " + e.what();
            return false;
        }
        return true;
    };
    auto fail = [&] {
        if (error) *error = err;
        return J();
    };
    if (files().empty()) {
        err = "no io definitions in this executable (build without worker_io)";
        return fail();
    }
    J index;
    if (!load("io.json", "io.json", &index)) return fail();
    J out = J::object();
    out["common"] = nullptr;
    out["self"] = nullptr;
    out["includes"] = J::object();
    out["provides"] = J::object();
    out["uses"] = J::object();
    if (files().count("lib/libworker/io/common.json") && !load("lib/libworker/io/common.json", "common.json", &out["common"]))
        return fail();
    if (files().count("io/self.json")) {
        if (!load("io/self.json", "io/self.json", &out["self"])) return fail();
        const J inc = out["self"].is_object() && out["self"].contains("include") ? out["self"]["include"] : J::array();
        for (const auto& i : inc.is_array() ? inc : J::array()) {
            if (!i.is_string()) continue;
            const std::string s = i.get<std::string>();
            const std::string p = normalize(s);
            if (p.empty() || s[0] == '/') {
                err = "io/self.json include " + s + ": outside worker folder";
                return fail();
            }
            if (!load(p, s, &out["includes"][s])) return fail();
        }
    }
    const J provides = index.contains("provides") ? index["provides"] : J::array();
    for (const auto& role : provides.is_array() ? provides : J::array()) {
        if (!role.is_string()) continue;
        const std::string r = role.get<std::string>();
        const std::string p = files().count("io/" + r + ".json") ? "io/" + r + ".json" : "io/uses/" + r + ".json";
        if (!load(p, "provides " + r, &out["provides"][r])) return fail();
    }
    const J uses = index.contains("uses") ? index["uses"] : J::object();
    if (uses.is_object()) {
        for (auto it = uses.begin(); it != uses.end(); ++it) {
            J entry = it->is_object() ? *it : J::object();
            const std::string from = entry.contains("from") && entry["from"].is_string() ? entry["from"].get<std::string>() : "";
            const std::string p = "io/uses/" + from + ".json";
            entry["definition"] = nullptr;
            if (files().count(p) && !load(p, "uses " + it.key(), &entry["definition"])) return fail();
            out["uses"][it.key()] = entry;
        }
    }
    return out;
}

}  // namespace worker
