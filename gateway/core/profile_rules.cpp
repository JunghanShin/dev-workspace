#include "profile_rules.hpp"

#include <vector>

namespace gw {

const char* const kMask = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";  // ••••

namespace {

bool is_space(unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f); }

// [\w.\-"] — Python \w는 유니코드 글자도 포함하므로 ASCII 밖 글자는 모두 \w로 본다(덜 가리는 일이 없게)
bool is_key_char(unsigned char c) {
    return c >= 0x80 || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-' || c == '"';
}

bool has_secret_word(const std::string& lower) {
    for (const char* w : {"pass", "token", "secret", "apikey", "api_key", "privatekey", "private_key"})
        if (lower.find(w) != std::string::npos) return true;
    return false;
}

// ^(\s*[\w.\-"]*(비밀어)[\w.\-"]*\s*[:=]\s*)(.+)$ 의 앞부분 길이. 안 맞으면 npos
size_t secret_prefix(const std::string& line) {
    const size_t n = line.size();
    size_t i = 0;
    while (i < n && is_space(static_cast<unsigned char>(line[i]))) ++i;
    size_t j = i;
    while (j < n && is_key_char(static_cast<unsigned char>(line[j]))) ++j;
    if (j == i || !has_secret_word(to_lower(line.substr(i, j - i)))) return std::string::npos;
    size_t k = j;
    while (k < n && is_space(static_cast<unsigned char>(line[k]))) ++k;
    if (k >= n || (line[k] != ':' && line[k] != '=')) return std::string::npos;
    size_t m = k + 1;
    while (m < n && is_space(static_cast<unsigned char>(line[m]))) ++m;
    if (m < n) return m;
    // 값 자리에 공백만 있으면 정규식은 마지막 공백 하나를 값으로 본다
    if (m > k + 1) return m - 1;
    return std::string::npos;
}

// ^\s*(#\s*)?([A-Z][A-Z0-9_]*)=(.*)$
bool env_line(const std::string& line, bool& commented, std::string& key, std::string& value) {
    size_t i = 0;
    const size_t n = line.size();
    while (i < n && is_space(static_cast<unsigned char>(line[i]))) ++i;
    commented = false;
    if (i < n && line[i] == '#') {
        commented = true;
        ++i;
        while (i < n && is_space(static_cast<unsigned char>(line[i]))) ++i;
    }
    if (i >= n || line[i] < 'A' || line[i] > 'Z') return false;
    size_t j = i;
    while (j < n && ((line[j] >= 'A' && line[j] <= 'Z') || (line[j] >= '0' && line[j] <= '9') || line[j] == '_')) ++j;
    if (j >= n || line[j] != '=') return false;
    key = line.substr(i, j - i);
    value = line.substr(j + 1);
    return true;
}

std::string lstrip_hash(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == '#') ++i;
    return s.substr(i);
}

}  // namespace

bool secret_name(const std::string& key) { return has_secret_word(to_lower(key)); }

std::string mask_text(const std::string& text) {
    std::string out;
    size_t start = 0;
    while (true) {
        const size_t nl = text.find('\n', start);
        const std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        const size_t p = secret_prefix(line);
        out += p == std::string::npos ? line : line.substr(0, p) + kMask;
        if (nl == std::string::npos) break;
        out += '\n';
        start = nl + 1;
    }
    return out;
}

Json env_doc(const std::string& text) {
    Json out = Json::array();
    std::vector<std::string> desc;
    std::string group;
    for (const auto& line : splitlines(text)) {
        bool commented;
        std::string key, value;
        if (env_line(line, commented, key, value)) {
            std::string d;
            for (size_t i = 0; i < desc.size(); ++i) d += (i ? " " : "") + desc[i];
            Json e = Json::object();
            e["key"] = key;
            e["default"] = commented ? Json(nullptr) : Json(value);
            e["example"] = commented ? Json(value) : Json(nullptr);
            e["desc"] = d;
            e["group"] = group;
            out.push_back(e);
            desc.clear();
        } else if (starts_with(line, "##")) {  // 묶음 제목: 아래 키들에 이어서 붙는다
            group = strip(lstrip_hash(line));
            desc.clear();
        } else if (starts_with(line, "#")) {
            desc.push_back(strip(lstrip_hash(line)));
        } else {
            desc.clear();
        }
    }
    return out;
}

Json env_values(const std::string& text) {
    Json vals = Json::object();
    for (const auto& line : splitlines(text)) {
        bool commented;
        std::string key, value;
        if (env_line(line, commented, key, value) && !commented) vals[key] = value;
    }
    return vals;
}

bool profile_file_name(const std::string& name) {
    return !starts_with(name, ".") && !starts_with(name, "README") && !ends_with(name, ".md") &&
           name != "worker-types.json" && name != "product.json" && name != "workspace-settings.json";
}

std::string file_origin(const std::optional<std::string>& board, const std::optional<std::string>& prof,
                        const std::optional<std::string>& dflt, bool has_src) {
    if (!board) return "missing";
    if (prof) return *board == *prof ? "profile" : "changed";
    if (dflt && *board == *dflt) return "default";
    return (has_src || dflt) ? "changed" : "unknown";
}

void env_finish(Json& env, const Json& board, const Json& prof) {
    for (auto& e : env) {
        const std::string k = e["key"].get<std::string>();
        const bool secret = secret_name(k);
        auto it = board.find(k);
        e["value"] = it == board.end() ? Json(nullptr) : secret ? Json(kMask) : *it;
        e["in_profile"] = prof.contains(k);
        if (secret && e.contains("default") && py_truthy(e["default"])) e["default"] = kMask;
    }
}

}  // namespace gw
