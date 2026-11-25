#include "settings.hpp"

#include <cmath>
#include <cstdio>
#include <set>

namespace gw {

namespace {

bool key_char(char c, bool first) {
    return (c >= 'A' && c <= 'Z') || c == '_' || (!first && c >= '0' && c <= '9');
}

bool is_sp(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r'; }

bool valid_key(const std::string& k) {
    if (k.empty()) return false;
    for (size_t i = 0; i < k.size(); ++i)
        if (!key_char(k[i], i == 0)) return false;
    return true;
}

// 줄 하나 읽기. 주석 아닌 줄: read_kv와 같이 # 앞까지 ^\s*KEY=(.*)$. 주석 줄: ^\s*#\s*KEY=
struct EnvLine {
    bool active = false, commented = false;
    std::string key, value;  // value: 주석 아닌 줄의 값(read_kv처럼 다듬음)
    std::string raw_value;   // 주석 줄이 아니면 # 앞까지 그대로, 주석 줄이면 = 뒤 전부
    std::string tail;        // 주석 아닌 줄의 # 뒤(앞 공백 포함, 바꿀 때 그대로 둠)
};

EnvLine parse_line(const std::string& line) {
    EnvLine e;
    size_t i = 0;
    while (i < line.size() && is_sp(line[i])) ++i;
    const bool hash = i < line.size() && line[i] == '#';
    size_t k = i;
    if (hash) {
        ++k;
        while (k < line.size() && is_sp(line[k])) ++k;
    }
    size_t j = k;
    while (j < line.size() && key_char(line[j], j == k)) ++j;
    if (j == k || j >= line.size() || line[j] != '=') return e;
    e.key = line.substr(k, j - k);
    if (hash) {
        e.commented = true;
        e.raw_value = line.substr(j + 1);
        return e;
    }
    e.active = true;
    const size_t h = line.find('#', j + 1);
    std::string v = line.substr(j + 1, h == std::string::npos ? std::string::npos : h - j - 1);
    if (h != std::string::npos) {
        size_t t = v.size();
        while (t > 0 && is_sp(v[t - 1])) --t;
        e.tail = v.substr(t) + line.substr(h);
        v.resize(t);
    }
    e.raw_value = v;
    size_t end = v.size();
    while (end > 0 && (is_sp(v[end - 1]) || v[end - 1] == '\n')) --end;
    v.resize(end);
    if (!v.empty() && v.front() == '"') v.erase(0, 1);
    if (!v.empty() && v.back() == '"') v.pop_back();
    e.value = v;
    return e;
}

std::string fmt_num(double x) {
    char buf[40];
    if (std::fabs(x) < 1e15 && x == std::floor(x)) {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(x));
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%.15g", x);
    double back = 0;
    if (!py_float(buf, back) || back != x) std::snprintf(buf, sizeof buf, "%.17g", x);
    return buf;
}

bool num_of(const Json& v, double& out) {
    if (v.is_number()) {
        out = v.get<double>();
        return std::isfinite(out);
    }
    if (v.is_string()) return py_float(strip(v.get<std::string>()), out) && std::isfinite(out);
    return false;
}

std::string range_text(const Json& f) {
    std::string s;
    if (f.contains("min") && f["min"].is_number()) s += "min " + fmt_num(f["min"].get<double>());
    if (f.contains("max") && f["max"].is_number()) s += std::string(s.empty() ? "" : ", ") + "max " + fmt_num(f["max"].get<double>());
    return s;
}

// 값 하나 검사 → 파일 글. 틀리면 이유
bool check_value(const Json& f, const Json& v, std::string& text, std::string& why) {
    const std::string type = f.value("type", "");
    if (type == "string") {
        if (!v.is_string()) return why = "must be a string", false;
        text = strip(v.get<std::string>());
        for (unsigned char c : text)
            if (c < 0x20 || c == 0x7f || c == '#') return why = "must not contain # or control characters", false;
        if (!text.empty() && (text.front() == '"' || text.back() == '"')) return why = "must not start or end with \"", false;
        return true;
    }
    if (type == "enum") {
        if (v.is_string())
            for (const auto& e : f["enum"])
                if (e == v) return text = v.get<std::string>(), true;
        std::string opts;
        for (const auto& e : f["enum"]) opts += (opts.empty() ? "" : ", ") + e.get<std::string>();
        return why = "must be one of " + opts, false;
    }
    double x = 0;
    if (v.is_boolean() || !num_of(v, x)) return why = type == "integer" ? "must be an integer" : "must be a number", false;
    if (type == "integer" && x != std::floor(x)) return why = "must be an integer", false;
    const bool lo = f.contains("min") && f["min"].is_number() && x < f["min"].get<double>();
    const bool hi = f.contains("max") && f["max"].is_number() && x > f["max"].get<double>();
    if (lo || hi) return why = "out of range (" + range_text(f) + ")", false;
    text = fmt_num(x);
    return true;
}

}  // namespace

bool settings_parse_def(const std::string& text, Json& def, std::string& err) {
    auto j = parse_json(text, &err);
    if (!j) return false;
    if (!j->is_object() || !j->contains("groups") || !(*j)["groups"].is_array()) return err = "groups must be a list", false;
    static const std::set<std::string> types = {"string", "number", "integer", "enum"};
    std::set<std::string> seen;
    for (const auto& g : (*j)["groups"]) {
        if (!g.is_object() || !g.contains("fields") || !g["fields"].is_array()) return err = "group needs fields", false;
        for (const auto& f : g["fields"]) {
            const std::string key = f.is_object() && f.contains("key") && f["key"].is_string() ? f["key"].get<std::string>() : "";
            if (!valid_key(key)) return err = "field key must be KEY_NAME", false;
            if (!seen.insert(key).second) return err = key + ": duplicate key", false;
            if (!f.contains("type") || !f["type"].is_string() || !types.count(f["type"].get<std::string>()))
                return err = key + ": type must be string, number, integer or enum", false;
            const Json apply = f.value("apply", Json(nullptr));
            if (apply != "live" && apply != "restart") return err = key + ": apply must be live or restart", false;
            if (f["type"] == "enum") {
                if (!f.contains("enum") || !f["enum"].is_array() || f["enum"].empty()) return err = key + ": enum needs values", false;
                for (const auto& e : f["enum"])
                    if (!e.is_string()) return err = key + ": enum values must be strings", false;
            }
        }
    }
    def = std::move(*j);
    return true;
}

const Json* settings_field(const Json& def, const std::string& key) {
    for (const auto& g : def["groups"])
        for (const auto& f : g["fields"])
            if (f["key"] == key) return &f;
    return nullptr;
}

Json settings_typed(const Json& f, const std::string& raw) {
    const std::string type = f.value("type", "");
    if (type == "number") {
        double x;
        if (py_float(strip(raw), x) && std::isfinite(x)) return x;
    } else if (type == "integer") {
        long long n;
        if (py_int(strip(raw), n)) return n;
    }
    return raw;
}

std::string settings_default_text(const Json& f) {
    const Json d = f.value("default", Json(nullptr));
    if (d.is_string()) return d.get<std::string>();
    if (d.is_number()) return fmt_num(d.get<double>());
    return "";
}

bool settings_check(const Json& def, const Json& body, EnvChanges& changes, Json& fields, std::string& err) {
    changes.clear();
    fields = Json::object();
    if (!body.is_object()) return err = "body must be {\"KEY\": value, ...}", false;
    for (auto it = body.begin(); it != body.end(); ++it) {
        const Json* f = settings_field(def, it.key());
        if (!f) {
            fields[it.key()] = "unknown setting";
            continue;
        }
        const Json& v = it.value();
        if (v.is_null() || (v.is_string() && strip(v.get<std::string>()).empty())) {
            changes.push_back({it.key(), std::nullopt});
            continue;
        }
        std::string text, why;
        if (!check_value(*f, v, text, why)) fields[it.key()] = why;
        else changes.push_back({it.key(), text});
    }
    if (!fields.empty()) {
        err = "invalid settings: ";
        bool first = true;
        for (auto it = fields.begin(); it != fields.end(); ++it, first = false)
            err += (first ? "" : ", ") + it.key();
        changes.clear();
        return false;
    }
    return true;
}

std::string env_edit(const std::string& text, const EnvChanges& changes) {
    // 줄로 나눔(마지막 줄바꿈 기억). \r은 줄에 남아 있다가 그대로 다시 붙는다
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    std::vector<bool> drop(lines.size(), false);
    std::vector<std::string> append;
    for (const auto& ch : changes) {
        const std::string& key = ch.first;
        std::vector<size_t> active, commented;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (drop[i]) continue;
            const EnvLine e = parse_line(lines[i]);
            if (e.key != key) continue;
            (e.active ? active : commented).push_back(i);
        }
        const auto cr = [&](size_t i) { return !lines[i].empty() && lines[i].back() == '\r' ? "\r" : ""; };
        if (ch.second) {
            const std::string line = key + "=" + *ch.second;
            if (!active.empty()) {
                const size_t i = active[0];
                const EnvLine e = parse_line(lines[i]);
                std::string tail = e.tail;
                if (!tail.empty() && tail.back() == '\r') tail.pop_back();
                lines[i] = line + tail + cr(i);
                for (size_t n = 1; n < active.size(); ++n) drop[active[n]] = true;
            } else if (!commented.empty()) {
                const size_t i = commented[0];
                lines[i] = line + cr(i);
            } else {
                append.push_back(line);
            }
        } else {
            for (size_t n = 0; n < active.size(); ++n) {
                const size_t i = active[n];
                if (!commented.empty() || n > 0) {
                    drop[i] = true;
                } else {
                    std::string v = parse_line(lines[i]).raw_value;
                    if (!v.empty() && v.back() == '\r') v.pop_back();
                    lines[i] = "#" + key + "=" + v + cr(i);
                }
            }
            for (auto& a : append)  // 이번 요청에서 더한 줄도
                if (a.rfind(key + "=", 0) == 0) a.clear();
        }
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (drop[i]) continue;
        out += lines[i];
        if (i + 1 < lines.size() || (!text.empty() && text.back() == '\n')) out += '\n';
    }
    for (const auto& a : append) {
        if (a.empty()) continue;
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += a + "\n";
    }
    return out;
}

std::map<std::string, std::string> env_active(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        const EnvLine e = parse_line(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (e.active) out[e.key] = e.value;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

}  // namespace gw
