// 화면의 "제품 빌드"(design.md 11.4, Python workspace_rest/productbuild.py). 개발 모드에서만(제품 모드면 404).
//
// 빌드는 dev/build_product.py(PC 명령과 같은 코드)를 --local로 자식 프로세스로 돌리고, 표준 출력의
// "##PROGRESS <0~100> <글>", "##RESULT <JSON>", "##ERROR <글>" 줄로 진행 상황을 만든다. 한 번에 하나만.
// 결과는 <데이터>/dist/에 마지막 하나만(--replace).
//
// 스레드: 출력 읽기 gw.build(빌드 하나에 하나). 상태(BuildState)는 mu_ 하나로 지키고 잠금 안에서 I/O 없음.
#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "board_parse.hpp"

namespace gw {

class Busy : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ProductBuild {
public:
    ProductBuild(std::string script, std::string data_dir, std::string python = "python3");
    ~ProductBuild();

    Json status();
    Json check(const std::string& profile);
    // login: "on"/"off"(제품에 로그인을 넣을지, 빈 글이면 product.json). 진행 중이면 Busy
    Json start(const std::string& profile, const std::string& power_mode, const std::string& login = "");
    bool cancel();
    // 마지막 묶음 {file, path, mb, created} 또는 null
    Json last() const;
    void shutdown();  // 출력 읽기 스레드 정리(게이트웨이 끝날 때)

private:
    Json profiles();
    void read_output(pid_t pid, int fd);
    std::vector<std::string> env_extra() const;

    const std::string script_, out_, python_;
    std::mutex mu_;  // st_, pid_, 프로필 캐시
    BuildState st_;
    pid_t pid_ = -1;
    double profiles_t_ = -1e18;
    Json profiles_ = Json::array();
    std::thread reader_;
    std::atomic<bool> reading_{false};
    std::atomic<bool> stop_{false};
};

}  // namespace gw
