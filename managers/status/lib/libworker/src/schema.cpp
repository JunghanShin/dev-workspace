#include "worker/schema.hpp"

#include <cctype>

namespace worker {

using Json = nlohmann::json;

namespace {

bool type_ok(const Json& v, const std::string& t) {
    if (t == "integer") return v.is_number_integer();
    if (t == "number") return v.is_number();
    if (t == "null") return v.is_null();
    if (t == "object") return v.is_object();
    if (t == "array") return v.is_array();
    if (t == "string") return v.is_string();
    if (t == "boolean") return v.is_boolean();
    return false;
}

std::string kind_of(const Json& v) {
    switch (v.type()) {
    case Json::value_t::null: return "null";
    case Json::value_t::boolean: return "boolean";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned: return "integer";
    case Json::value_t::number_float: return "number";
    case Json::value_t::string: return "string";
    case Json::value_t::array: return "array";
    case Json::value_t::object: return "object";
    default: return "?";
    }
}

void add(std::vector<std::string>& errs, size_t max_errs, const std::string& e) {
    if (errs.size() < max_errs) errs.push_back(e);
}

}  // namespace

void validate_schema(const Json& v, const Json& s, const Json& defs, const std::string& path, std::vector<std::string>& errs,
                     size_t max_errs) {
    if (errs.size() >= max_errs || !s.is_object()) return;  // true·null·없음: 무엇이든
    if (s.contains("$ref") && s["$ref"].is_string()) {
        const std::string ref = s["$ref"].get<std::string>();
        const std::string name = ref.substr(ref.rfind('/') + 1);
        if (!defs.is_object() || !defs.contains(name)) return add(errs, max_errs, path + ": $ref " + ref + " not found");
        return validate_schema(v, defs[name], defs, path, errs, max_errs);
    }
    if (s.contains("type")) {
        const Json& t = s["type"];
        bool ok = false;
        std::string names;
        for (const auto& x : t.is_array() ? t : Json::array({t}))
            if (x.is_string()) {
                ok |= type_ok(v, x.get<std::string>());
                names += (names.empty() ? "" : "|") + x.get<std::string>();
            }
        if (!ok) return add(errs, max_errs, path + ": type is " + kind_of(v) + ", want " + names);
    }
    if (s.contains("enum") && s["enum"].is_array()) {
        bool in = false;
        for (const auto& e : s["enum"]) in |= e == v;
        if (!in) add(errs, max_errs, path + ": " + v.dump() + " not in enum");
    }
    if (v.is_number()) {
        const double x = v.get<double>();
        if (s.contains("minimum") && s["minimum"].is_number() && x < s["minimum"].get<double>())
            add(errs, max_errs, path + ": " + v.dump() + " < minimum " + s["minimum"].dump());
        if (s.contains("maximum") && s["maximum"].is_number() && x > s["maximum"].get<double>())
            add(errs, max_errs, path + ": " + v.dump() + " > maximum " + s["maximum"].dump());
        if (s.contains("exclusiveMinimum") && s["exclusiveMinimum"].is_number() && x <= s["exclusiveMinimum"].get<double>())
            add(errs, max_errs, path + ": " + v.dump() + " <= " + s["exclusiveMinimum"].dump());
        if (s.contains("exclusiveMaximum") && s["exclusiveMaximum"].is_number() && x >= s["exclusiveMaximum"].get<double>())
            add(errs, max_errs, path + ": " + v.dump() + " >= " + s["exclusiveMaximum"].dump());
    }
    if (v.is_object()) {
        const Json props = s.value("properties", Json::object());
        if (s.contains("required") && s["required"].is_array())
            for (const auto& k : s["required"])
                if (k.is_string() && !v.contains(k.get<std::string>())) add(errs, max_errs, path + "." + k.get<std::string>() + ": required");
        const Json extra = s.value("additionalProperties", Json(false));
        for (const auto& [k, sub] : v.items()) {
            if (props.is_object() && props.contains(k)) validate_schema(sub, props[k], defs, path + "." + k, errs, max_errs);
            else if (extra == Json(false)) add(errs, max_errs, path + "." + k + ": not in definition");
            else if (extra.is_object()) validate_schema(sub, extra, defs, path + "." + k, errs, max_errs);
        }
    }
    if (v.is_array() && s.contains("items"))
        for (size_t i = 0; i < v.size() && errs.size() < max_errs; ++i)
            validate_schema(v[i], s["items"], defs, path + "[" + std::to_string(i) + "]", errs, max_errs);
}

bool name_matches(const std::string& pattern, const std::string& name) {
    size_t p = 0, n = 0;
    while (p < pattern.size()) {
        if (pattern[p] == '<') {
            const size_t close = pattern.find('>', p);
            if (close == std::string::npos) return pattern == name;
            p = close + 1;
            // 자리: [a-z0-9_]+ 를 가능한 만큼, 뒤가 맞는지 되돌려 봄
            const size_t start = n;
            while (n < name.size() && (std::islower(static_cast<unsigned char>(name[n])) || std::isdigit(static_cast<unsigned char>(name[n])) || name[n] == '_')) ++n;
            for (size_t end = n; end > start; --end)
                if (name_matches(pattern.substr(p), name.substr(end))) return true;
            return false;
        }
        if (n >= name.size() || pattern[p] != name[n]) return false;
        ++p;
        ++n;
    }
    return n == name.size();
}

}  // namespace worker
