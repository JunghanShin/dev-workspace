#include "posix_fs.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "json_util.hpp"
#include "log.hpp"

namespace pm {

std::string errno_msg(int err, const std::string& path) {
    return "[Errno " + std::to_string(err) + "] " + std::strerror(err) + ": " + py_quote(path);
}

FileSystem::Read PosixFs::read(const std::string& path, std::string& out, std::string& err) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return Read::Missing;
        err = errno_msg(errno, path);
        return Read::Error;
    }
    char buf[16384];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = errno_msg(errno, path);
            ::close(fd);
            return Read::Error;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > (64u << 20)) {  // 설정 파일이 이만큼 크면 잘못된 것
            err = path + ": file too large";
            ::close(fd);
            return Read::Error;
        }
    }
    ::close(fd);
    return Read::Ok;
}

bool PosixFs::exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

bool PosixFs::is_file(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool PosixFs::is_dir(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> PosixFs::list(const std::string& dir, bool* missing) {
    std::vector<std::string> out;
    if (missing) *missing = false;
    DIR* d = ::opendir(dir.c_str());
    if (!d) {
        if (missing) *missing = true;
        return out;
    }
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") out.push_back(n);
    }
    ::closedir(d);
    return out;
}

bool mkdir_p(const std::string& dir, std::string* err) {
    if (dir.empty()) return true;
    std::string cur;
    size_t i = 0;
    while (i <= dir.size()) {
        size_t j = dir.find('/', i);
        if (j == std::string::npos) j = dir.size();
        cur = dir.substr(0, j);
        if (!cur.empty() && ::mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) {
            if (err) *err = errno_msg(errno, cur);
            return false;
        }
        i = j + 1;
    }
    return true;
}

namespace {

std::string parent(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? "." : (s == 0 ? "/" : p.substr(0, s));
}

bool write_all(int fd, const std::string& text) {
    size_t off = 0;
    while (off < text.size()) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

// 임시 파일 → fsync → rename(→ 폴더 fsync). 전원이 끊겨도 반쯤 쓴 파일이 남지 않게
bool atomic_write(const FileOp& op, std::string& err) {
    if (!mkdir_p(parent(op.path), &err)) return false;
    const std::string tmp = op.path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        err = errno_msg(errno, tmp);
        return false;
    }
    if (!write_all(fd, op.text) || ::fsync(fd) != 0) {
        err = errno_msg(errno, tmp);
        ::close(fd);
        return false;
    }
    ::close(fd);
    if (::rename(tmp.c_str(), op.path.c_str()) != 0) {
        err = errno_msg(errno, tmp);
        return false;
    }
    if (op.sync_dir) {
        const int dfd = ::open(parent(op.path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }
    return true;
}

bool copy_file(const std::string& from, const std::string& to) {
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out < 0) {
        ::close(in);
        return false;
    }
    char buf[65536];
    bool ok = true;
    while (true) {
        const ssize_t n = ::read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        if (!write_all(out, std::string(buf, static_cast<size_t>(n)))) {
            ok = false;
            break;
        }
    }
    ::close(in);
    ::close(out);
    return ok;
}

// 복사 후 비우기(copy-truncate). 워커는 O_APPEND로 쓰므로 비운 뒤에도 끝에 이어 쓴다
void rotate_logs(const FileOp& op) {
    DIR* d = ::opendir(op.path.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0) names.push_back(n);
    }
    ::closedir(d);
    for (const auto& n : names) {
        const std::string path = op.path + "/" + n;
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0 || st.st_size <= op.max_bytes) continue;
        for (int i = op.backups - 1; i > 0; --i) {
            const std::string src = path + "." + std::to_string(i);
            if (::access(src.c_str(), F_OK) == 0) ::rename(src.c_str(), (path + "." + std::to_string(i + 1)).c_str());
        }
        if (op.backups > 0 && !copy_file(path, path + ".1")) continue;
        if (::truncate(path.c_str(), 0) != 0) continue;
    }
}

}  // namespace

bool run_file_ops(const std::vector<FileOp>& ops, std::string& err) {
    for (const auto& op : ops) {
        std::string e;
        bool ok = true;
        switch (op.kind) {
        case FileOp::Kind::Write:
            ok = atomic_write(op, e);
            break;
        case FileOp::Kind::Rename:
            if (::rename(op.path.c_str(), op.to.c_str()) != 0) {
                e = "[Errno " + std::to_string(errno) + "] " + std::strerror(errno) + ": " + py_quote(op.path) +
                    " -> " + py_quote(op.to);
                ok = false;
            }
            break;
        case FileOp::Kind::Remove:
            if (::unlink(op.path.c_str()) != 0 && !(errno == ENOENT && op.missing_ok)) {
                e = errno_msg(errno, op.path);
                ok = false;
            }
            break;
        case FileOp::Kind::RotateLogs:
            rotate_logs(op);
            break;
        }
        if (!ok) {
            if (op.ignore_error) {
                PM_WARN("file op skipped: %s", e.c_str());
                continue;
            }
            err = e;
            return false;
        }
    }
    return true;
}

}  // namespace pm
