#include "config/core.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace config {

namespace {

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64];
    size_t n = 0;
    uint64_t total = 0;
    void block(const unsigned char* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const char* data, size_t len) {
        total += len;
        const auto* p = reinterpret_cast<const unsigned char*>(data);
        while (len) {
            const size_t k = std::min(len, 64 - n);
            std::copy(p, p + k, buf + n);
            n += k;
            p += k;
            len -= k;
            if (n == 64) {
                block(buf);
                n = 0;
            }
        }
    }
    std::string hex() {
        const uint64_t bits = total * 8;
        const char pad = static_cast<char>(0x80);
        update(&pad, 1);
        const char zero = 0;
        while (n != 56) update(&zero, 1);
        char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<char>(bits >> (56 - 8 * i));
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

std::vector<std::string> list_dir(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.empty() || n[0] == '.') continue;
            out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool is_file(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l)) out.push_back(l);
    return out;
}

void diff_json(const Json& a, const Json& b, const std::string& path, Json& out) {
    if (a == b) return;
    if (a.is_object() && b.is_object()) {
        std::set<std::string> keys;
        for (auto it = a.begin(); it != a.end(); ++it) keys.insert(it.key());
        for (auto it = b.begin(); it != b.end(); ++it) keys.insert(it.key());
        for (const auto& k : keys)
            diff_json(a.contains(k) ? a[k] : Json(), b.contains(k) ? b[k] : Json(), path.empty() ? k : path + "." + k, out);
        return;
    }
    out.push_back({{"path", path}, {"from", a}, {"to", b}});
}

}  // namespace

std::string sha256_hex(const std::string& data) {
    Sha s;
    s.update(data.data(), data.size());
    return s.hex();
}

std::string file_sha256(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    Sha s;
    char buf[65536];
    while (f) {
        f.read(buf, sizeof buf);
        if (f.gcount() > 0) s.update(buf, static_cast<size_t>(f.gcount()));
    }
    return s.hex();
}

Build parse_build(const std::string& text) {
    Build b;
    bool in_sha = false;
    for (const auto& raw : lines_of(text)) {
        if (raw.empty() || raw[0] == '#') continue;
        if (in_sha && raw.size() > 2 && raw[0] == ' ') {
            std::istringstream in(raw);
            std::string h, name;
            in >> h >> name;
            if (h.size() == 64 && !name.empty()) b.sha[name] = h;
            continue;
        }
        in_sha = false;
        const size_t c = raw.find(':');
        if (c == std::string::npos) continue;
        const std::string k = raw.substr(0, c);
        std::string v = raw.substr(c + 1);
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        if (k == "source_commit") b.commit = v;
        else if (k == "built") b.built = v;
        else if (k == "build_env") b.env = v;
        else if (k == "sha256") in_sha = true;
    }
    return b;
}

Json component_version(const std::string& bin_dir) {
    std::ifstream f(bin_dir + "/BUILD");
    if (!f) return nullptr;
    std::stringstream ss;
    ss << f.rdbuf();
    const Build b = parse_build(ss.str());
    Json files = Json::object();
    bool ok = !b.sha.empty();
    for (const auto& [name, h] : b.sha) {
        const std::string actual = file_sha256(bin_dir + "/" + name);
        const bool same = actual == h;
        ok = ok && same;
        files[name] = {{"ok", same}, {"sha", actual.empty() ? Json(nullptr) : Json(actual.substr(0, 12))}};
    }
    return {{"commit", b.commit}, {"built", b.built}, {"env", b.env}, {"files", files}, {"ok", ok}};
}

std::map<std::string, std::string> bin_dirs(const std::string& root) {
    std::map<std::string, std::string> out;
    for (const char* p : {"manager", "gateway", "monitor"}) out[p] = root + "/" + p + "/bin";
    for (const auto& m : list_dir(root + "/managers")) out["managers/" + m] = root + "/managers/" + m + "/bin";
    for (const auto& w : list_dir(root + "/workers")) {
        if (w[0] == '_') continue;
        const std::string ws = root + "/workers/" + w + "/workspace/bin";
        out["workers/" + w] = is_file(ws + "/BUILD") ? ws : root + "/workers/" + w + "/bin";
    }
    for (auto it = out.begin(); it != out.end();) it = is_file(it->second + "/BUILD") ? std::next(it) : out.erase(it);
    return out;
}

Json diff(const Json& from, const Json& to) {
    Json out = Json::array();
    const Json fw = from.value("workers", Json::object()), tw = to.value("workers", Json::object());
    diff_json(fw, tw, "workers", out);
    diff_json(from.value("connections", Json()), to.value("connections", Json()), "connections", out);
    const Json ff = from.value("files", Json::object()), tf = to.value("files", Json::object());
    std::set<std::string> names;
    for (auto it = ff.begin(); it != ff.end(); ++it) names.insert(it.key());
    for (auto it = tf.begin(); it != tf.end(); ++it) names.insert(it.key());
    for (const auto& n : names) {
        const std::string a = ff.contains(n) && ff[n].is_string() ? ff[n].get<std::string>() : "";
        const std::string b = tf.contains(n) && tf[n].is_string() ? tf[n].get<std::string>() : "";
        if (a == b) continue;
        const auto la = lines_of(a), lb = lines_of(b);
        const std::multiset<std::string> sa(la.begin(), la.end()), sb(lb.begin(), lb.end());
        Json removed = Json::array(), added = Json::array();
        for (const auto& l : la)
            if (!sb.count(l)) removed.push_back(l);
        for (const auto& l : lb)
            if (!sa.count(l)) added.push_back(l);
        out.push_back({{"path", "files." + n}, {"removed", removed}, {"added", added}});
    }
    Json fv = Json::object(), tv = Json::object();
    for (const auto& [src, dst] : {std::pair<const Json*, Json*>{&from, &fv}, {&to, &tv}}) {
        const Json v = src->value("versions", Json::object());
        for (auto it = v.begin(); it != v.end(); ++it) (*dst)[it.key()] = it->is_object() ? it->value("commit", Json()) : Json();
    }
    diff_json(fv, tv, "versions", out);
    return out;
}

bool same_config(const Json& a, const Json& b) {
    return a.value("workers", Json()) == b.value("workers", Json()) && a.value("connections", Json()) == b.value("connections", Json()) &&
           a.value("files", Json()) == b.value("files", Json());
}

std::vector<std::string> to_prune(const std::vector<SnapshotInfo>& infos, size_t keep) {
    std::vector<std::string> out;
    if (infos.size() <= keep) return out;
    size_t extra = infos.size() - keep;
    for (const auto& s : infos) {
        if (extra == 0) break;
        if (s.label == "auto") {
            out.push_back(s.id);
            --extra;
        }
    }
    return out;
}

}  // namespace config
