// core가 바깥과 닿는 곳(포트). core는 시스템 호출을 모르고 이것만 부른다.
// 실제 구현은 src/(시계, fork/exec, pm.store 스레드, 파일 읽기), 단위 시험은 가짜(가짜 시계·가짜 spawn).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pm {

struct Clock {
    virtual ~Clock() = default;
    virtual double mono() = 0;             // 초, 단조 증가
    virtual std::string now_iso() = 0;     // 지역 시각 "2026-11-20T10:00:00+09:00"
};

struct SpawnSpec {
    std::vector<std::string> argv;
    std::vector<std::string> env;          // "KEY=VALUE"
    std::string log_path;                  // stdout·stderr를 이어 쓸 파일
    std::optional<int> nice;
    std::vector<int> cpus;
};

struct Spawner {
    virtual ~Spawner() = default;
    // 새 세션(프로세스 그룹 리더)으로 띄운다. 실패하면 -1, err에 이유
    virtual int spawn(const SpawnSpec& spec, std::string& err) = 0;
    virtual void signal(int pid, int sig) = 0;       // 워커에게만
    virtual void kill_group(int pgid, int sig) = 0;  // 프로세스 그룹 전체
    virtual bool executable(const std::string& path) = 0;
};

// 파일 쓰기 한 건. 쓰기는 임시 파일 → fsync → rename
struct FileOp {
    enum class Kind { Write, Rename, Remove, RotateLogs };
    Kind kind = Kind::Write;
    std::string path;
    std::string to;            // Rename 대상
    std::string text;          // Write 내용
    bool sync_dir = false;     // 폴더까지 fsync(실행 기록 파일)
    bool missing_ok = false;   // Remove: 없어도 성공
    bool ignore_error = false; // 실패해도 다음 op 계속(정리용, 기록만)
    long long max_bytes = 0;   // RotateLogs
    int backups = 0;
};

// done(ok, 오류 글)은 늘 루프 스레드에서, 낸 순서대로 불린다
using StoreDone = std::function<void(bool ok, const std::string& err)>;

struct Store {
    virtual ~Store() = default;
    // ops를 차례로 한다(앞 것이 실패하면 멈춤). 큐가 차거나 시간이 넘으면 done(false, ...)
    virtual void submit(std::vector<FileOp> ops, StoreDone done) = 0;
};

// 파일 읽기(작은 설정 파일). 루프에서 바로 읽는다(쓰기·fsync만 pm.store로 뺌)
struct FileSystem {
    enum class Read { Ok, Missing, Error };
    virtual ~FileSystem() = default;
    virtual Read read(const std::string& path, std::string& out, std::string& err) = 0;
    virtual bool exists(const std::string& path) = 0;
    virtual bool is_file(const std::string& path) = 0;
    virtual bool is_dir(const std::string& path) = 0;
    // 이름 목록(정렬 안 함). 폴더가 없으면 missing = true
    virtual std::vector<std::string> list(const std::string& dir, bool* missing = nullptr) = 0;
};

}  // namespace pm
