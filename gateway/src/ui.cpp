#include "ui.hpp"

#include <algorithm>

#include "log.hpp"
#include "sys.hpp"
#include "ui_rules.hpp"

namespace gw {

namespace {

// panel.json 읽기(UTF-8, 엄격한 JSON, 객체만). 실패하면 nullopt와 이유
std::optional<Json> read_manifest(const std::string& path, std::string& why) {
    int err = 0;
    auto raw = read_file(path, 1 << 20, &err);
    if (!raw) {
        why = errno_text(err);
        return std::nullopt;
    }
    if (utf8_replace(*raw) != *raw) {
        why = "invalid UTF-8";
        return std::nullopt;
    }
    auto j = parse_json(*raw, &why);
    if (!j) return std::nullopt;
    if (!j->is_object()) {
        why = "panel.json is not an object";
        return std::nullopt;
    }
    return j;
}

std::vector<std::string> sorted_dir(const std::string& path) {
    std::vector<std::string> names = is_dir(path) ? list_dir(path) : std::vector<std::string>{};
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace

bool Ui::available() const { return is_file(path_join(cfg_.web, "shell.html")); }

std::map<std::string, std::string> Ui::worker_panel_dirs() const {
    // _로 시작하는 폴더(틀)는 뺀다
    std::map<std::string, std::string> out;
    for (const auto& name : sorted_dir(cfg_.workers)) {
        const std::string d = path_join(path_join(path_join(cfg_.workers, name), "workspace"), "panel");
        const std::string manifest = path_join(d, "panel.json");
        if (starts_with(name, "_") || !is_file(manifest)) continue;
        std::string why;
        auto j = read_manifest(manifest, why);
        if (!j) {
            GW_ERROR("worker panel %s skipped: %s", name.c_str(), why.c_str());
            continue;
        }
        auto it = j->find("id");
        const std::string pid = (it != j->end() && it->is_string() && !it->get<std::string>().empty())
                                    ? it->get<std::string>() : name;
        out[pid] = d;
    }
    return out;
}

std::optional<StaticFile> Ui::static_file(const std::string& path) const {
    size_t i = 0;
    while (i < path.size() && path[i] == '/') ++i;
    std::string rel = path.substr(i);
    std::string base = cfg_.web;
    // /panels/<id>/...: 워커가 가진 패널이면 그 워커 폴더에서
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t slash = rel.find('/', start);
        parts.push_back(rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (parts.size() >= 3 && parts[0] == "panels" && !is_dir(path_join(path_join(cfg_.web, "panels"), parts[1]))) {
        const auto dirs = worker_panel_dirs();
        auto it = dirs.find(parts[1]);
        if (it != dirs.end()) {
            base = it->second;
            rel.clear();
            for (size_t k = 2; k < parts.size(); ++k) rel += (k > 2 ? "/" : "") + parts[k];
        }
    }
    const std::string full = contained_path(base, rel);
    if (full.empty() || !is_file(full)) return std::nullopt;
    auto data = read_file(full);
    if (!data) return std::nullopt;
    return StaticFile{std::move(*data), content_type_for(full)};
}

Json Ui::panels() const {
    Json out = Json::array();
    const std::string root = path_join(cfg_.web, "panels");
    std::vector<std::string> have;
    for (const auto& name : sorted_dir(root)) {
        const std::string manifest = path_join(path_join(root, name), "panel.json");
        if (!is_file(manifest)) continue;
        std::string why;
        auto p = read_manifest(manifest, why);
        if (!p) {
            GW_ERROR("panel %s skipped: %s", name.c_str(), why.c_str());
            continue;
        }
        (*p)["id"] = name;
        have.push_back(name);
        out.push_back(*p);
    }
    // 워커가 가진 패널. 공용 패널과 id가 겹치면 공용 패널을 쓴다
    for (const auto& kv : worker_panel_dirs()) {
        if (std::find(have.begin(), have.end(), kv.first) != have.end()) {
            GW_ERROR("worker panel %s skipped: id already used by ui/web/panels", kv.first.c_str());
            continue;
        }
        std::string why;
        auto p = read_manifest(path_join(kv.second, "panel.json"), why);
        if (!p) continue;
        (*p)["id"] = kv.first;
        out.push_back(*p);
    }
    sort_panels(out);
    return out;
}

void Ui::set_title(const std::string& v) {
    std::lock_guard<std::mutex> lock(live_mu_);
    title_ = v;
}

void Ui::set_media_url(const std::string& v) {
    std::lock_guard<std::mutex> lock(live_mu_);
    media_url_ = v;
}

std::string Ui::title() const {
    std::lock_guard<std::mutex> lock(live_mu_);
    return title_;
}

std::string Ui::media_url() const {
    std::lock_guard<std::mutex> lock(live_mu_);
    return media_url_;
}

Json Ui::ui_info(const std::string& host_header, const std::string& data_dir, bool product, const std::string& proto) const {
    std::string url = media_url();
    // 상대 주소(/media/{path}/, docs/media-proxy.md)는 요청이 온 주소로 완전하게: 다른 곳에서 띄운 화면(별도 UI)이 읽어도
    // 이 보드로 간다. Host는 주소에 쓸 수 있는 글자만일 때만(아니면 상대 그대로)
    if (!url.empty() && url[0] == '/' && !host_header.empty() &&
        host_header.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:[]") == std::string::npos)
        url = std::string(proto == "https" ? "https" : "http") + "://" + host_header + url;
    const std::string host = request_host(host_header);
    for (size_t pos = 0; (pos = url.find("{host}", pos)) != std::string::npos; pos += host.size())
        url.replace(pos, 6, host);
    Json o = Json::object();
    o["title"] = title();
    o["product"] = product;  // 화면이 설정·워커 시작/정지 부품을 그리지 않는다(kit.js setProduct)
    o["refresh_ms"] = cfg_.refresh_ms;
    o["media"] = Json{{"url", url}};
    o["board"] = Json{{"data_dir", data_dir}};
    o["rest_last_ok"] = wall_now();
    o["rest_last_error"] = nullptr;
    return o;
}

std::pair<int, Json> Ui::remote(const std::string& name, const Params& params, Reads& reads) const {
    std::string path;
    Params query;
    if (!remote_read_path(name, params, path, query)) return {404, Json{{"error", "unknown remote command"}}};
    Route r;
    if (!match_read(path, r)) return {400, Json{{"error", "bad worker id"}}};
    ReadResult res;
    try {
        res = reads.handle(r.read, r.id, r.has_id, query);
    } catch (const BadRequest& e) {
        return {400, Json{{"error", e.what()}}};
    } catch (const std::exception& e) {  // 읽기 실패가 게이트웨이를 멈추지 않게
        GW_ERROR("ui read %s failed: %s", name.c_str(), e.what());
        return {200, Json{{"ok", false}, {"error", e.what()}}};
    }
    if (res.status != 200) {
        std::string msg;
        auto e = res.body.find("error");
        if (e != res.body.end() && e->is_object() && e->contains("message") && py_truthy((*e)["message"]))
            msg = (*e)["message"].get<std::string>();
        else
            msg = "read " + name + ": " + std::to_string(res.status);
        return {200, Json{{"ok", false}, {"error", msg}}};
    }
    Json lines = Json::array();
    for (auto& l : remote_lines(name, res.body)) lines.push_back(l);
    return {200, Json{{"ok", true}, {"lines", lines}}};
}

}  // namespace gw
