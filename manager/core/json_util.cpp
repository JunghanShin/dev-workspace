#include "json_util.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pm {

bool is_num(const Json& v) { return v.is_number(); }  // nlohmann: bool은 number가 아니다

namespace {
bool numlike(const Json& v) { return v.is_number() || v.is_boolean(); }

long double as_ld(const Json& v) {
    if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    if (v.is_number_unsigned()) return static_cast<long double>(v.get<uint64_t>());
    if (v.is_number_integer()) return static_cast<long double>(v.get<int64_t>());
    return static_cast<long double>(v.get<double>());
}
}  // namespace

bool py_eq(const Json& a, const Json& b) {
    if (numlike(a) && numlike(b)) {
        if (a.is_number_integer() && b.is_number_integer() && !a.is_number_unsigned() && !b.is_number_unsigned())
            return a.get<int64_t>() == b.get<int64_t>();
        return as_ld(a) == as_ld(b);
    }
    if (a.type() != b.type()) return false;
    if (a.is_object()) {
        if (a.size() != b.size()) return false;
        for (auto it = a.begin(); it != a.end(); ++it) {
            auto jt = b.find(it.key());
            if (jt == b.end() || !py_eq(*it, *jt)) return false;
        }
        return true;
    }
    if (a.is_array()) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!py_eq(a[i], b[i])) return false;
        return true;
    }
    return a == b;
}

bool py_truthy(const Json& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return as_ld(v) != 0;
    if (v.is_string()) return !v.get_ref<const std::string&>().empty();
    if (v.is_object() || v.is_array()) return !v.empty();
    return true;
}

std::string fmt_g(double x) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", x);
    return buf;
}

std::string py_quote(const std::string& s) {
    // Python repr: 작은따옴표, 글에 '만 있고 "가 없으면 큰따옴표
    const bool sq = s.find('\'') != std::string::npos, dq = s.find('"') != std::string::npos;
    const char q = (sq && !dq) ? '"' : '\'';
    std::string out(1, q);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == static_cast<unsigned char>(q)) { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    out += q;
    return out;
}

std::string py_str(const Json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "None";
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (std::isnan(d)) return "nan";
        if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
        return v.dump();  // 가장 짧은 표기, 1.0은 "1.0"(Python repr와 같음)
    }
    if (v.is_number()) return v.dump();
    return py_repr(v);
}

std::string py_repr(const Json& v) {
    if (v.is_string()) return py_quote(v.get<std::string>());
    if (v.is_array()) {
        std::string out = "[";
        for (size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + py_repr(v[i]);
        return out + "]";
    }
    if (v.is_object()) {
        std::string out = "{";
        bool first = true;
        for (auto it = v.begin(); it != v.end(); ++it) {
            out += (first ? "" : ", ") + py_quote(it.key()) + ": " + py_repr(*it);
            first = false;
        }
        return out + "}";
    }
    return py_str(v);
}

std::string dump_line(const Json& v) { return v.dump(-1, ' ', false, Json::error_handler_t::replace); }

std::string dump_file(const Json& v) { return v.dump(2, ' ', false, Json::error_handler_t::replace) + "\n"; }

std::optional<Json> parse(const std::string& text, std::string* err) {
    try {
        return Json::parse(text);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return std::nullopt;
    }
}

namespace {
std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// 숫자 사이 밑줄(1_000)은 Python이 받는다. 숫자 사이가 아닌 밑줄이면 false
bool drop_underscores(const std::string& s, std::string& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_') {
            if (i == 0 || i + 1 == s.size() || !std::isdigit(static_cast<unsigned char>(s[i - 1])) ||
                !std::isdigit(static_cast<unsigned char>(s[i + 1])))
                return false;
            continue;
        }
        out += s[i];
    }
    return true;
}
}  // namespace

bool py_int(const std::string& raw, long long& out) {
    std::string s;
    if (!drop_underscores(strip(raw), s) || s.empty()) return false;
    size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
    if (i == s.size()) return false;
    for (size_t j = i; j < s.size(); ++j)
        if (!std::isdigit(static_cast<unsigned char>(s[j]))) return false;
    errno = 0;
    char* end = nullptr;
    out = std::strtoll(s.c_str(), &end, 10);
    return errno == 0 && end && *end == '\0';
}

Json py_number(const std::string& raw) {
    long long i;
    if (py_int(raw, i)) return i;
    std::string s;
    if (drop_underscores(strip(raw), s) && !s.empty() && s.find_first_of("xXpP(") == std::string::npos) {
        char* end = nullptr;
        const double d = std::strtod(s.c_str(), &end);
        if (end && *end == '\0') return d;
    }
    return raw;
}

}  // namespace pm
