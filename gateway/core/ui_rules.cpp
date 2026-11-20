#include "ui_rules.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

namespace gw {

std::string normpath(const std::string& path) {
    if (path.empty()) return ".";
    int initial = path[0] == '/' ? 1 : 0;
    if (initial && path.compare(0, 2, "//") == 0 && path.compare(0, 3, "///") != 0) initial = 2;
    std::vector<std::string> comps;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.size();
        const std::string comp = path.substr(start, slash - start);
        start = slash + 1;
        if (comp.empty() || comp == ".") continue;
        if (comp != ".." || (!initial && comps.empty()) || (!comps.empty() && comps.back() == "..")) comps.push_back(comp);
        else if (!comps.empty()) comps.pop_back();
    }
    std::string out(static_cast<size_t>(initial), '/');
    for (size_t i = 0; i < comps.size(); ++i) out += (i ? "/" : "") + comps[i];
    return out.empty() ? "." : out;
}

std::string path_join(const std::string& a, const std::string& b) {
    if (!b.empty() && b[0] == '/') return b;
    if (a.empty() || a.back() == '/') return a + b;
    return a + "/" + b;
}

namespace {
std::vector<std::string> parts(const std::string& p) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= p.size()) {
        size_t slash = p.find('/', start);
        if (slash == std::string::npos) slash = p.size();
        if (slash > start) out.push_back(p.substr(start, slash - start));
        start = slash + 1;
    }
    return out;
}
}  // namespace

std::string relpath(const std::string& path, const std::string& start) {
    const auto a = parts(normpath(path)), b = parts(normpath(start));
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    std::string out;
    for (size_t k = i; k < b.size(); ++k) out += out.empty() ? ".." : "/..";
    for (size_t k = i; k < a.size(); ++k) out += (out.empty() ? "" : "/") + a[k];
    return out.empty() ? "." : out;
}

std::string basename(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string contained_path(const std::string& base, const std::string& rel) {
    const std::string p = normpath(path_join(base, rel));
    return starts_with(p, base + "/") ? p : "";
}

std::string content_type_for(const std::string& path) {
    static const std::map<std::string, std::string> types = {
        {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"}, {"js", "text/javascript"},
        {"mjs", "text/javascript"}, {"json", "application/json"}, {"svg", "image/svg+xml"}, {"png", "image/png"},
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"jpe", "image/jpeg"}, {"gif", "image/gif"},
        {"ico", "image/vnd.microsoft.icon"}, {"webp", "image/webp"}, {"bmp", "image/bmp"}, {"avif", "image/avif"},
        {"woff", "font/woff"}, {"woff2", "font/woff2"}, {"ttf", "font/ttf"}, {"otf", "font/otf"},
        {"txt", "text/plain"}, {"md", "text/markdown"}, {"csv", "text/csv"}, {"xml", "application/xml"},
        {"pdf", "application/pdf"}, {"wasm", "application/wasm"}, {"mp4", "video/mp4"}, {"webm", "video/webm"},
        {"mp3", "audio/mpeg"}, {"wav", "audio/x-wav"}, {"zip", "application/zip"}, {"tar", "application/x-tar"},
    };
    static const std::map<std::string, std::string> suffix = {
        {"svgz", "svg.gz"}, {"tgz", "tar.gz"}, {"taz", "tar.gz"}, {"tz", "tar.gz"}, {"tbz2", "tar.bz2"}, {"txz", "tar.xz"}};
    std::string name = basename(path);
    auto ext_of = [](const std::string& n) {
        const size_t dot = n.rfind('.');
        return (dot == std::string::npos || dot == 0) ? std::string() : n.substr(dot + 1);
    };
    std::string ext = to_lower(ext_of(name));
    auto sm = suffix.find(ext);
    if (sm != suffix.end()) {
        name = name.substr(0, name.size() - ext.size()) + sm->second;
        ext = to_lower(ext_of(name));
    }
    // 압축 꼬리(.gz 등)는 떼고 안쪽 확장자로(mimetypes encodings_map)
    if (ext == "gz" || ext == "bz2" || ext == "xz" || ext == "br" || ext == "z") {
        name = name.substr(0, name.size() - ext.size() - 1);
        ext = to_lower(ext_of(name));
    }
    auto it = types.find(ext);
    std::string ctype = it == types.end() ? "application/octet-stream" : it->second;
    if (ends_with(path, ".js")) ctype = "text/javascript";
    if (starts_with(ctype, "text") || ctype == "application/json" || ctype == "image/svg+xml") ctype += "; charset=utf-8";
    return ctype;
}

std::string request_host(const std::string& host_header) {
    // urlsplit("//" + host).hostname
    std::string netloc = host_header;
    const size_t cut = netloc.find_first_of("/?#");
    if (cut != std::string::npos) netloc = netloc.substr(0, cut);
    const size_t at = netloc.rfind('@');
    const std::string hostinfo = at == std::string::npos ? netloc : netloc.substr(at + 1);
    std::string host;
    const size_t br = hostinfo.find('[');
    if (br != std::string::npos) {
        const std::string rest = hostinfo.substr(br + 1);
        host = rest.substr(0, rest.find(']'));
    } else {
        host = hostinfo.substr(0, hostinfo.find(':'));
    }
    host = to_lower(host);
    if (host.empty()) host = "127.0.0.1";
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

void sort_panels(Json& panels) {
    std::vector<Json> v(panels.begin(), panels.end());
    auto order = [](const Json& p) {
        auto it = p.find("order");
        return (it != p.end() && it->is_number()) ? it->get<double>() : 100.0;
    };
    std::stable_sort(v.begin(), v.end(), [&](const Json& a, const Json& b) {
        const double oa = order(a), ob = order(b);
        if (oa != ob) return oa < ob;
        return a["id"].get<std::string>() < b["id"].get<std::string>();
    });
    panels = Json::array();
    for (auto& p : v) panels.push_back(std::move(p));
}

bool remote_read_path(const std::string& name, const Params& params, std::string& path, Params& query) {
    auto get = [&](const char* k, const char* dflt) {
        auto it = params.find(k);
        return it == params.end() ? std::string(dflt) : it->second;
    };
    const std::string wid = get("id", "");
    query.clear();
    if (name == "worker_log") {
        path = "/logs/workers/" + wid;
        query["lines"] = get("lines", "200");
    } else if (name == "service_log") {
        path = "/logs/service";
        query["lines"] = get("lines", "200");
    } else if (name == "commands") {
        path = "/records/commands";
        query["lines"] = get("lines", "200");
    } else if (name == "processes") {
        path = "/system/processes";
        query["lines"] = get("lines", "12");
    } else if (name == "power_modes") {
        path = "/system/power_modes";
    } else if (name == "metrics_history") {
        path = "/history/" + wid;
        for (const char* k : {"minutes", "points", "keys", "csv"}) {
            auto it = params.find(k);
            if (it != params.end()) query[k] = it->second;
        }
    } else {
        return false;
    }
    return true;
}

std::vector<std::string> remote_lines(const std::string& name, const Json& obj) {
    std::vector<std::string> out;
    if (name == "processes") {
        for (const auto& p : obj.value("processes", Json::array())) {
            std::string pname = p["name"].get<std::string>();
            std::replace(pname.begin(), pname.end(), ' ', '_');
            out.push_back(std::to_string(p["pid"].get<long long>()) + " " + pname + " " +
                          fmt_fixed(p["cpu"].get<double>(), 1) + " " + fmt_fixed(p["mem"].get<double>(), 1) + " " +
                          std::to_string(p["rss_kb"].get<long long>()));
        }
        return out;
    }
    if (name == "power_modes") {
        std::vector<std::pair<long long, std::pair<std::string, std::string>>> modes;
        for (auto it = obj["modes"].begin(); it != obj["modes"].end(); ++it)
            modes.push_back({std::stoll(it.key()), {it.key(), it->get<std::string>()}});
        std::stable_sort(modes.begin(), modes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& m : modes) out.push_back("< POWER_MODEL ID=" + m.second.first + " NAME=" + m.second.second + " >");
        return out;
    }
    for (const auto& l : obj.value("lines", Json::array())) out.push_back(l.get<std::string>());
    return out;
}

}  // namespace gw
