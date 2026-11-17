#include "replay/recording.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>

#include "nlohmann/json.hpp"

namespace replay {

namespace {

constexpr char kMagic[8] = {'F', 'B', 'R', 'E', 'C', '1', '\n', '\0'};
constexpr size_t kHeader = 24;
constexpr uint32_t kMaxRecord = 256u * 1024 * 1024;  // 레코드 하나 상한(잘못된 파일 방어)

std::string err(const std::string& what) { return what + ": " + std::strerror(errno); }

void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

void mkdirs(const std::string& dir) {
    for (size_t i = 1; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '/') ::mkdir(dir.substr(0, i).c_str(), 0755);
}

nlohmann::json to_json(const Meta& m) {
    return {{"width", m.width}, {"height", m.height}, {"stride", m.stride}, {"slot_bytes", m.slot_bytes},
            {"pixel", m.pixel}, {"source", m.source}, {"created", m.created}, {"frames", m.frames},
            {"bytes", m.bytes}, {"duration_ns", m.duration_ns}};
}

bool from_json(const std::string& path, Meta* m) {
    std::ifstream in(path);
    if (!in) return false;
    try {
        nlohmann::json j;
        in >> j;
        m->width = j.value("width", 0u);
        m->height = j.value("height", 0u);
        m->stride = j.value("stride", 0u);
        m->slot_bytes = j.value("slot_bytes", 0u);
        m->pixel = j.value("pixel", std::string());
        m->source = j.value("source", std::string());
        m->created = j.value("created", std::string());
        m->frames = j.value("frames", 0ull);
        m->bytes = j.value("bytes", 0ull);
        m->duration_ns = j.value("duration_ns", 0ll);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string now_iso() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

}  // namespace

bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
    return true;
}

// ---- Writer -------------------------------------------------------------------------------

Writer::~Writer() { close(); }

bool Writer::open(const std::string& root, const std::string& name, const Meta& meta, std::string* error) {
    if (!valid_name(name)) {
        if (error) *error = "bad recording name (a-z A-Z 0-9 - _, 1~64): " + name;
        return false;
    }
    mkdirs(root);
    dir_ = root + "/" + name;
    if (::mkdir(dir_.c_str(), 0755) != 0) {
        if (error) *error = errno == EEXIST ? "recording already exists: " + name : err("mkdir " + dir_);
        return false;
    }
    f_ = std::fopen((dir_ + "/frames.bin").c_str(), "wb");
    if (!f_ || std::fwrite(kMagic, 1, sizeof kMagic, f_) != sizeof kMagic) {
        if (error) *error = err("create " + dir_ + "/frames.bin");
        if (f_) std::fclose(f_);
        f_ = nullptr;
        return false;
    }
    meta_ = meta;
    meta_.frames = meta_.bytes = 0;
    meta_.duration_ns = 0;
    meta_.created = now_iso();
    have_first_ = false;
    return write_meta(error);
}

bool Writer::write(uint64_t frame_id, int64_t ts_ns, const uint8_t* data, size_t n) {
    if (!f_ || n > kMaxRecord) return false;
    uint8_t h[kHeader] = {};
    put_u64(h, frame_id);
    put_u64(h + 8, static_cast<uint64_t>(ts_ns));
    put_u32(h + 16, static_cast<uint32_t>(n));
    if (std::fwrite(h, 1, kHeader, f_) != kHeader || (n && std::fwrite(data, 1, n, f_) != n)) return false;
    if (!have_first_) {
        first_ts_ = ts_ns;
        have_first_ = true;
    }
    ++meta_.frames;
    meta_.bytes += n;
    meta_.duration_ns = ts_ns - first_ts_;
    return true;
}

bool Writer::write_meta(std::string* error) {
    const std::string tmp = dir_ + "/meta.json.tmp";
    {
        std::ofstream out(tmp);
        out << to_json(meta_).dump(2) << "\n";
        if (!out) {
            if (error) *error = err("write " + tmp);
            return false;
        }
    }
    if (std::rename(tmp.c_str(), (dir_ + "/meta.json").c_str()) != 0) {
        if (error) *error = err("rename meta.json");
        return false;
    }
    return true;
}

bool Writer::close(std::string* error) {
    if (!f_) return true;
    const bool ok = std::fflush(f_) == 0 && ::fsync(fileno(f_)) == 0;
    std::fclose(f_);
    f_ = nullptr;
    return write_meta(error) && ok;
}

// ---- Reader -------------------------------------------------------------------------------

Reader::~Reader() { close(); }

bool Reader::open(const std::string& root, const std::string& name, std::string* error) {
    close();
    if (!valid_name(name)) {
        if (error) *error = "bad recording name: " + name;
        return false;
    }
    const std::string dir = root + "/" + name;
    if (!from_json(dir + "/meta.json", &meta_)) {
        if (error) *error = "no recording (meta.json): " + name;
        return false;
    }
    f_ = std::fopen((dir + "/frames.bin").c_str(), "rb");
    char magic[sizeof kMagic];
    if (!f_ || std::fread(magic, 1, sizeof magic, f_) != sizeof magic || std::memcmp(magic, kMagic, sizeof kMagic) != 0) {
        if (error) *error = "bad frames.bin: " + name;
        close();
        return false;
    }
    return true;
}

bool Reader::next(Record* out) {
    if (!f_) return false;
    uint8_t h[kHeader];
    if (std::fread(h, 1, kHeader, f_) != kHeader) return false;
    const uint32_t n = get_u32(h + 16);
    if (n > kMaxRecord) return false;
    out->frame_id = get_u64(h);
    out->ts_ns = static_cast<int64_t>(get_u64(h + 8));
    out->data.resize(n);
    return n == 0 || std::fread(out->data.data(), 1, n, f_) == n;
}

void Reader::rewind() {
    if (f_) std::fseek(f_, sizeof kMagic, SEEK_SET);
}

void Reader::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
}

// ---- 목록·삭제 ----------------------------------------------------------------------------

std::vector<Info> list(const std::string& root) {
    std::vector<Info> out;
    DIR* d = ::opendir(root.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (!valid_name(name)) continue;
        Info i;
        i.name = name;
        if (!from_json(root + "/" + name + "/meta.json", &i.meta)) continue;
        struct stat st{};
        if (::stat((root + "/" + name + "/frames.bin").c_str(), &st) == 0) i.file_bytes = static_cast<uint64_t>(st.st_size);
        out.push_back(std::move(i));
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return a.name < b.name; });
    return out;
}

uint64_t total_bytes(const std::string& root) {
    uint64_t sum = 0;
    for (const auto& i : list(root)) sum += i.file_bytes;
    return sum;
}

bool remove(const std::string& root, const std::string& name, std::string* error) {
    if (!valid_name(name)) {
        if (error) *error = "bad recording name: " + name;
        return false;
    }
    const std::string dir = root + "/" + name;
    for (const char* f : {"/frames.bin", "/meta.json", "/meta.json.tmp"}) ::unlink((dir + f).c_str());
    if (::rmdir(dir.c_str()) != 0) {
        if (error) *error = err("remove " + dir);
        return false;
    }
    return true;
}

}  // namespace replay
