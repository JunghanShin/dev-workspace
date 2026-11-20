#include "schema.hpp"

#include <cctype>
#include <functional>
#include <cmath>
#include <map>
#include <memory>
#include <regex>
#include <set>

namespace pm::schema {

namespace {

const std::set<std::string> kTypes = {"string", "int", "number", "bool", "enum", "list", "map"};

// 스키마의 pattern은 같은 것이 자주 오므로 컴파일한 것을 둔다. core는 루프 스레드에서만 쓰지만 시험 대비 스레드마다
const std::regex* re_of(const std::string& pat) {
    thread_local std::map<std::string, std::unique_ptr<std::regex>> cache;
    auto it = cache.find(pat);
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<std::regex> re;
    try {
        re = std::make_unique<std::regex>(pat, std::regex::ECMAScript);
    } catch (const std::regex_error&) {
        re = nullptr;  // 잘못된 정규식: 맞지 않음으로 본다
    }
    if (cache.size() > 256) cache.clear();
    return cache.emplace(pat, std::move(re)).first->second.get();
}

bool re_search(const std::string& pat, const std::string& s) {
    const std::regex* re = re_of(pat);
    return re && std::regex_search(s, *re);
}

Json get(const Json& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? Json(nullptr) : *it;
}

std::string str_of(const Json& v) { return v.is_string() ? v.get<std::string>() : std::string(); }

std::vector<Json> list_of(const Json& v) {
    std::vector<Json> out;
    if (v.is_array())
        for (const auto& x : v) out.push_back(x);
    return out;
}

std::string fmt(const Json& x) { return x.is_number_float() ? fmt_g(x.get<double>()) : py_str(x); }

long double ld(const Json& v) {
    if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    if (v.is_number_unsigned()) return static_cast<long double>(v.get<uint64_t>());
    if (v.is_number_integer()) return static_cast<long double>(v.get<int64_t>());
    return static_cast<long double>(v.get<double>());
}

bool numeric_bound(const Json& b) { return b.is_number() || b.is_boolean(); }

void check_num(const Json& f, const Json& v, const std::string& path, Json& errs, bool integer) {
    if (!v.is_number() || (integer && v.is_number_float() &&
                           (!std::isfinite(v.get<double>()) || std::floor(v.get<double>()) != v.get<double>()))) {
        errs[path] = integer ? "정수여야 합니다" : "숫자여야 합니다";
        return;
    }
    const Json lo = get(f, "min"), hi = get(f, "max");
    const bool has_lo = numeric_bound(lo), has_hi = numeric_bound(hi);
    const long double x = ld(v);
    if ((has_lo && x < ld(lo)) || (has_hi && x > ld(hi))) {
        if (has_lo && has_hi) errs[path] = fmt(lo) + " ~ " + fmt(hi) + " 사이여야 합니다";
        else if (has_lo) errs[path] = fmt(lo) + " 이상이어야 합니다";
        else errs[path] = fmt(hi) + " 이하여야 합니다";
    }
}

bool is_ipv4(const std::string& s) {
    // Python ipaddress.IPv4Address: 10진 네 칸, 0~255, 앞자리 0 안 됨
    int parts = 0;
    size_t i = 0;
    while (true) {
        size_t j = i;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        const size_t n = j - i;
        if (n == 0 || n > 3) return false;
        if (n > 1 && s[i] == '0') return false;
        if (std::stoi(s.substr(i, n)) > 255) return false;
        ++parts;
        if (j == s.size()) break;
        if (s[j] != '.') return false;
        i = j + 1;
    }
    return parts == 4;
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!std::isdigit(c)) return false;
    return true;
}

void check_string(const Json& f, const Json& v, const std::string& path, Json& errs) {
    if (!v.is_string()) {
        errs[path] = "글자여야 합니다";
        return;
    }
    const std::string s = v.get<std::string>();
    const std::string format = str_of(get(f, "format"));
    if (format == "ipv4") {
        if (!is_ipv4(s)) {
            errs[path] = "IPv4 주소여야 합니다(예: 192.168.1.10)";
            return;
        }
    } else if (format == "host_port") {
        const size_t c = s.rfind(':');
        const std::string host = c == std::string::npos ? "" : s.substr(0, c);
        const std::string port = c == std::string::npos ? s : s.substr(c + 1);
        const bool port_ok = all_digits(port) && port.size() <= 6 && std::stoi(port) >= 1 && std::stoi(port) <= 65535;
        if (host.empty() || !port_ok || !re_search("^[A-Za-z0-9]([A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$", host)) {
            errs[path] = "호스트:포트 형식이어야 합니다(예: 127.0.0.1:9000)";
            return;
        }
    } else if (format == "stream_url") {
        if (!re_search(R"(^(rtsp|rtsps|srt|udp|rtmp)://[^/\s:]+(:\d{1,5})?(/\S*)?$)", s)) {
            errs[path] = "rtsp://호스트:포트/경로 같은 주소여야 합니다";
            return;
        }
    } else if (format == "worker_id") {
        if (!s.empty() && !re_search("^[a-z0-9][a-z0-9_-]{0,63}$", s)) {  // 빈 값 = 상대 없음(꺼짐)
            errs[path] = "워커 id여야 합니다(영문 소문자·숫자·-·_)";
            return;
        }
    }
    const Json pat = get(f, "pattern");
    if (pat.is_string() && !pat.get<std::string>().empty() && !re_search(pat.get<std::string>(), s))
        errs[path] = "형식이 맞지 않습니다";
}

std::vector<Json> options_of(const Json& f) {
    std::vector<Json> out;
    for (const auto& o : list_of(get(f, "options"))) {
        if (o.is_array()) out.push_back(o.empty() ? Json(nullptr) : o[0]);
        else out.push_back(o);
    }
    return out;
}

// enum 비교: true와 1을 같은 값으로 보지 않는다
bool same(const Json& a, const Json& b) {
    if (a.is_number() && b.is_number()) return py_eq(a, b);
    if (a.is_boolean() != b.is_boolean()) return false;
    if (a.is_number() != b.is_number()) return false;
    // Python type(a) is type(b): int와 float는 위에서, 나머지는 같은 종류만
    if (a.type() != b.type()) return false;
    return py_eq(a, b);
}

Json check_value(const Json& f, const Json& v, const std::string& path);

void check_list(const Json& f, const Json& v, const std::string& path, Json& errs) {
    if (!v.is_array()) {
        errs[path] = "목록이어야 합니다";
        return;
    }
    const Json item_key = get(f, "item_key");
    std::set<std::string> seen;
    const std::vector<Json> sub = list_of(get(f, "fields"));
    for (size_t i = 0; i < v.size(); ++i) {
        const std::string p = path + "[" + std::to_string(i) + "]";
        const Json& item = v[i];
        if (!item.is_object()) {
            errs[p] = "항목은 객체여야 합니다";
            continue;
        }
        errs.update(check_object(sub, item, p + "."));
        if (item_key.is_string() && !item_key.get<std::string>().empty()) {
            const Json val = get(item, item_key.get<std::string>().c_str());
            if (val.is_string()) {
                const std::string s = val.get<std::string>();
                if (seen.count(s)) errs[p + "." + item_key.get<std::string>()] = "같은 값이 이미 있습니다: " + s;
                seen.insert(s);
            }
        }
    }
}

void check_map(const Json& f, const Json& v, const std::string& path, Json& errs) {
    if (!v.is_object()) {
        errs[path] = "키·값 목록이어야 합니다";
        return;
    }
    const Json vt_j = get(f, "value_type");
    const std::string vt = vt_j.is_null() ? "string" : str_of(vt_j);
    const Json kp = get(f, "key_pattern");
    for (auto it = v.begin(); it != v.end(); ++it) {
        const std::string p = path + "." + it.key();
        if (kp.is_string() && !kp.get<std::string>().empty() && !re_search(kp.get<std::string>(), it.key()))
            errs[p] = "이름 형식이 맞지 않습니다: " + it.key();
        else if (vt == "int" || vt == "number")
            check_num(f, *it, p, errs, vt == "int");
        else if (!it->is_string())
            errs[p] = "글자여야 합니다";
    }
}

Json check_value(const Json& f, const Json& v, const std::string& path) {
    Json errs = Json::object();
    const std::string t = str_of(get(f, "type"));
    if (t == "string") check_string(f, v, path, errs);
    else if (t == "int") check_num(f, v, path, errs, true);
    else if (t == "number") check_num(f, v, path, errs, false);
    else if (t == "bool") {
        if (!v.is_boolean()) errs[path] = "켜기·끄기(true/false)여야 합니다";
    } else if (t == "enum") {
        const auto opts = options_of(f);
        bool ok = false;
        for (const auto& o : opts) ok = ok || same(v, o);
        if (!ok) {
            std::string list;
            for (size_t i = 0; i < opts.size(); ++i) list += (i ? ", " : "") + py_str(opts[i]);
            errs[path] = "선택지 중 하나여야 합니다: " + list;
        }
    } else if (t == "list") check_list(f, v, path, errs);
    else if (t == "map") check_map(f, v, path, errs);
    return errs;
}

}  // namespace

std::vector<std::pair<Json, std::vector<Json>>> sections_of(const Json& schema) {
    std::vector<std::pair<Json, std::vector<Json>>> out;
    for (const auto& g : list_of(get(schema, "groups"))) {
        if (!g.is_object()) continue;
        const Json sec = get(g, "section");
        auto it = out.begin();
        for (; it != out.end(); ++it)
            if (it->first == sec) break;
        if (it == out.end()) {
            out.emplace_back(sec, std::vector<Json>{});
            it = out.end() - 1;
        }
        for (const auto& f : list_of(get(g, "fields"))) it->second.push_back(f);
    }
    return out;
}

std::vector<Json> fields_of(const Json& schema) {
    std::vector<Json> out;
    for (const auto& g : list_of(get(schema, "groups")))
        for (const auto& f : list_of(get(g, "fields"))) out.push_back(f);
    return out;
}

std::vector<std::string> check_schema(const Json& schema) {
    std::vector<std::string> errs;
    if (!schema.is_object() || !get(schema, "groups").is_array()) return {"config_schema must be {\"groups\": [...]}"};
    std::function<void(const std::vector<Json>&, const std::string&)> walk = [&](const std::vector<Json>& fields,
                                                                                 const std::string& where) {
        std::set<std::string> seen;
        for (const auto& f : fields) {
            const Json k = get(f, "key");
            if (!k.is_string() || k.get<std::string>().empty()) {
                errs.push_back(where + ": field without key");
                continue;
            }
            const std::string key = k.get<std::string>();
            if (seen.count(key)) errs.push_back(where + "." + key + ": duplicate key");
            seen.insert(key);
            const Json t = get(f, "type");
            if (!t.is_string() || !kTypes.count(t.get<std::string>()))
                errs.push_back(where + "." + key + ": unknown type " + py_repr(t));
            const std::string ts = str_of(t);
            if (ts == "enum" && !py_truthy(get(f, "options"))) errs.push_back(where + "." + key + ": enum without options");
            if (ts == "list") walk(list_of(get(f, "fields")), where + "." + key + "[]");
            if (f.is_object() && f.contains("default")) {
                const Json e = check_value(f, f["default"], key);
                for (auto it = e.begin(); it != e.end(); ++it)
                    errs.push_back(where + "." + key + " default: " + it->get<std::string>());
            }
        }
    };
    for (const auto& [section, fields] : sections_of(schema)) {
        if (!section.is_null() &&
            (!section.is_string() || !re_search("^[a-z][a-z0-9_]*$", section.get<std::string>()))) {
            errs.push_back("bad section name " + py_repr(section));
            continue;
        }
        walk(fields, section.is_null() ? "config" : "config." + section.get<std::string>());
    }
    return errs;
}

Json check_object(const std::vector<Json>& fields, const Json& obj, const std::string& prefix) {
    Json errs = Json::object();
    for (const auto& f : fields) {
        const Json k = get(f, "key");
        if (!k.is_string()) continue;
        const std::string key = k.get<std::string>();
        const std::string path = prefix + key;
        const Json v = get(obj, key.c_str());
        if (v.is_null()) {
            if (py_truthy(get(f, "required"))) errs[path] = "비울 수 없습니다";
            continue;
        }
        errs.update(check_value(f, v, path));
    }
    return errs;
}

Json check_config(const Json& schema, const Json& config) {
    Json errs = Json::object();
    if (!py_truthy(schema)) return errs;
    for (const auto& [section, fields] : sections_of(schema)) {
        if (section.is_null()) {
            errs.update(check_object(fields, config));
            continue;
        }
        if (!section.is_string()) continue;
        const std::string sec = section.get<std::string>();
        Json sub = get(config, sec.c_str());
        if (sub.is_null()) sub = Json::object();
        if (!sub.is_object()) {
            errs[sec] = "객체여야 합니다";
            continue;
        }
        errs.update(check_object(fields, sub, sec + "."));
    }
    return errs;
}

Json worker_refs(const Json& schema, const Json& config) {
    Json out = Json::object();
    if (!py_truthy(schema) || !config.is_object()) return out;
    std::function<void(const std::vector<Json>&, const Json&, const std::string&)> walk =
        [&](const std::vector<Json>& fields, const Json& obj, const std::string& prefix) {
            if (!obj.is_object()) return;
            for (const auto& f : fields) {
                const Json k = get(f, "key");
                if (!k.is_string()) continue;
                const Json v = get(obj, k.get<std::string>().c_str());
                if (v.is_null()) continue;
                const std::string t = str_of(get(f, "type"));
                if (t == "string" && str_of(get(f, "format")) == "worker_id" && v.is_string() &&
                    !v.get<std::string>().empty())
                    out[prefix + k.get<std::string>()] = v;
                else if (t == "list" && v.is_array())
                    for (size_t i = 0; i < v.size(); ++i)
                        walk(list_of(get(f, "fields")), v[i],
                             prefix + k.get<std::string>() + "[" + std::to_string(i) + "].");
            }
        };
    for (const auto& [section, fields] : sections_of(schema)) {
        if (section.is_null()) {
            walk(fields, config, "");
        } else if (section.is_string()) {
            Json sub = get(config, section.get<std::string>().c_str());
            if (!py_truthy(sub)) sub = Json::object();
            walk(fields, sub, section.get<std::string>() + ".");
        }
    }
    return out;
}

}  // namespace pm::schema
