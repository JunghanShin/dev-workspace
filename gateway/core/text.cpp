#include "text.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gw {

namespace {

void dump_string(std::string& out, const std::string& raw) {
    const std::string s = utf8_replace(raw);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

std::string float_repr(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    return Json(d).dump();  // 가장 짧은 표기, 1.0은 "1.0"(Python repr와 같음)
}

void dump_py(std::string& out, const Json& v) {
    switch (v.type()) {
        case Json::value_t::object: {
            out += '{';
            bool first = true;
            for (auto it = v.begin(); it != v.end(); ++it) {
                if (!first) out += ", ";
                first = false;
                dump_string(out, it.key());
                out += ": ";
                dump_py(out, *it);
            }
            out += '}';
            break;
        }
        case Json::value_t::array: {
            out += '[';
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) out += ", ";
                dump_py(out, v[i]);
            }
            out += ']';
            break;
        }
        case Json::value_t::string: dump_string(out, v.get_ref<const std::string&>()); break;
        case Json::value_t::number_float: out += float_repr(v.get<double>()); break;
        default: out += v.dump(); break;  // null, bool, 정수
    }
}

bool is_ws(unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f); }

// 숫자 사이의 밑줄 하나는 허용(Python int·float). 밑줄을 뺀 글을 돌려줌, 틀리면 false
bool strip_underscores(const std::string& s, std::string& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_') {
            const bool ok = i > 0 && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i - 1])) &&
                            std::isdigit(static_cast<unsigned char>(s[i + 1]));
            if (!ok) return false;
            continue;
        }
        out += s[i];
    }
    return true;
}

}  // namespace

std::string py_dumps(const Json& v) {
    std::string out;
    dump_py(out, v);
    return out;
}

std::string compact(const Json& v, bool ensure_ascii) {
    return v.dump(-1, ' ', ensure_ascii, Json::error_handler_t::replace);
}

std::optional<Json> parse_json(const std::string& text, std::string* err) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        if (err) *err = "Unexpected UTF-8 BOM (decode using utf-8-sig)";
        return std::nullopt;
    }
    try {
        return Json::parse(text);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return std::nullopt;
    }
}

bool py_truthy(const Json& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_float()) return v.get<double>() != 0;
    if (v.is_number()) return v.get<int64_t>() != 0;
    if (v.is_string()) return !v.get_ref<const std::string&>().empty();
    if (v.is_object() || v.is_array()) return !v.empty();
    return true;
}

std::string py_str(const Json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "None";
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (std::isnan(d)) return "nan";
        if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
        return Json(d).dump();
    }
    if (v.is_number()) return v.dump();
    return py_dumps(v);
}

std::string fmt_fixed(double x, int nd) {
    const int n = std::snprintf(nullptr, 0, "%.*f", nd, x);
    std::string s(static_cast<size_t>(n) + 1, '\0');
    std::snprintf(&s[0], s.size(), "%.*f", nd, x);
    s.resize(static_cast<size_t>(n));
    return s;
}

double py_round(double x, int nd) {
    if (!std::isfinite(x)) return x;
    // printf는 2진 값을 정확히 반올림한다(딱 절반일 때 짝수 쪽) → Python round와 같음
    return std::strtod(fmt_fixed(x, nd).c_str(), nullptr);
}

bool py_int(const std::string& raw, long long& out) {
    std::string s = strip(raw), t;
    if (s.empty()) return false;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = s[0] == '-';
        s = s.substr(1);
    }
    if (s.empty() || !strip_underscores(s, t) || t.empty()) return false;
    long long v = 0;
    for (char c : t) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        // 넘치면 가장 큰 값에 붙인다(쓰는 곳이 범위로 자르므로 결과가 같음)
        if (v > (9223372036854775807LL - 9) / 10) v = 9223372036854775807LL / 10;
        v = v * 10 + (c - '0');
    }
    out = neg ? -v : v;
    return true;
}

bool py_float(const std::string& raw, double& out) {
    std::string s = strip(raw);
    if (s.empty()) return false;
    std::string body = s;
    bool neg = false;
    if (body[0] == '+' || body[0] == '-') {
        neg = body[0] == '-';
        body = body.substr(1);
    }
    const std::string low = to_lower(body);
    if (low == "inf" || low == "infinity") {
        out = neg ? -HUGE_VAL : HUGE_VAL;
        return true;
    }
    if (low == "nan") {
        out = std::nan("");
        return true;
    }
    std::string t;
    if (!strip_underscores(body, t) || t.empty()) return false;
    // 10진 표기만: 숫자 [. 숫자] [e[+-]숫자]
    size_t i = 0, digits = 0;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i, ++digits;
    if (i < t.size() && t[i] == '.') {
        ++i;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i, ++digits;
    }
    if (!digits) return false;
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
        size_t e = 0;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i, ++e;
        if (!e) return false;
    }
    if (i != t.size()) return false;
    out = std::strtod(t.c_str(), nullptr);
    if (neg) out = -out;
    return true;
}

double py_min(double a, double b) { return b < a ? b : a; }
double py_max(double a, double b) { return b > a ? b : a; }

std::string utf8_replace(const std::string& s) {
    // 잘못된 곳은 "가장 긴 올바른 앞부분"을 U+FFFD 하나로(Python decode errors="replace"와 같음)
    std::string out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t i = 0;
    auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    while (i < n) {
        const unsigned char c = byte(i);
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        int need = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c == 0xE0) need = 2, lo = 0xA0;
        else if (c >= 0xE1 && c <= 0xEC) need = 2;
        else if (c == 0xED) need = 2, hi = 0x9F;
        else if (c >= 0xEE && c <= 0xEF) need = 2;
        else if (c == 0xF0) need = 3, lo = 0x90;
        else if (c >= 0xF1 && c <= 0xF3) need = 3;
        else if (c == 0xF4) need = 3, hi = 0x8F;
        if (!need) {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        size_t k = 1;
        bool ok = true;
        for (; k <= static_cast<size_t>(need); ++k) {
            if (i + k >= n) { ok = false; break; }
            const unsigned char d = byte(i + k);
            const unsigned char l = k == 1 ? lo : 0x80, h = k == 1 ? hi : 0xBF;
            if (d < l || d > h) { ok = false; break; }
        }
        if (ok) {
            out.append(s, i, static_cast<size_t>(need) + 1);
            i += static_cast<size_t>(need) + 1;
        } else {
            out += "\xEF\xBF\xBD";
            i += k;  // 올바른 앞부분까지 건너뜀(적어도 1)
        }
    }
    return out;
}

size_t utf8_count(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string utf8_prefix(const std::string& s, size_t chars) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (n == chars) return s.substr(0, i);
            ++n;
        }
    }
    return s;
}

std::vector<std::string> splitlines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    const size_t n = s.size();
    size_t i = 0;
    bool open = false;  // cur에 쓰던 줄이 있음(끝 줄바꿈 뒤에는 빈 줄을 더하지 않음)
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t brk = 0;
        if (c == '\n' || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d || c == 0x1e) brk = 1;
        else if (c == '\r') brk = (i + 1 < n && s[i + 1] == '\n') ? 2 : 1;
        else if (c == 0xC2 && i + 1 < n && static_cast<unsigned char>(s[i + 1]) == 0x85) brk = 2;
        else if (c == 0xE2 && i + 2 < n && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                 (static_cast<unsigned char>(s[i + 2]) == 0xA8 || static_cast<unsigned char>(s[i + 2]) == 0xA9)) brk = 3;
        if (brk) {
            out.push_back(cur);
            cur.clear();
            open = false;
            i += brk;
        } else {
            cur += static_cast<char>(c);
            open = true;
            ++i;
        }
    }
    if (open) out.push_back(cur);
    return out;
}

std::string universal_newlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            out += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : s) {
        if (is_ws(c)) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += static_cast<char>(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_ws(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && is_ws(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string rstrip_char(const std::string& s, char c) {
    size_t b = s.size();
    while (b > 0 && s[b - 1] == c) --b;
    return s.substr(0, b);
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string drop_final_newline(const std::string& s) {
    return (!s.empty() && s.back() == '\n') ? s.substr(0, s.size() - 1) : s;
}

std::string errno_text(int err) {
    char buf[256];
    const char* msg = strerror_r(err, buf, sizeof buf);  // GNU: 돌려준 글을 씀
    return "[Errno " + std::to_string(err) + "] " + msg;
}

}  // namespace gw
