// 파일 읽기(FileSystem 포트 구현)와 파일 쓰기 실행(pm.store 스레드가 부름).
#pragma once

#include <string>
#include <vector>

#include "ports.hpp"

namespace pm {

class PosixFs : public FileSystem {
public:
    Read read(const std::string& path, std::string& out, std::string& err) override;
    bool exists(const std::string& path) override;
    bool is_file(const std::string& path) override;
    bool is_dir(const std::string& path) override;
    std::vector<std::string> list(const std::string& dir, bool* missing = nullptr) override;
};

// Python OSError 글과 같은 모양: "[Errno 2] No such file or directory: '/a/b'"
std::string errno_msg(int err, const std::string& path);
// os.makedirs(exist_ok=True)
bool mkdir_p(const std::string& dir, std::string* err = nullptr);
// FileOp 차례로(앞 것이 실패하면 멈춤, ignore_error는 기록만). 성공이면 true
bool run_file_ops(const std::vector<FileOp>& ops, std::string& err);

}  // namespace pm
