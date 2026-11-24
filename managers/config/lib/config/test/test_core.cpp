// config 코어 단위 시험: sha256, BUILD 읽기·어긋남, 실행 파일 폴더 찾기, 스냅숏 diff·같은가, 남기기(auto부터)
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "config/core.hpp"

using config::Json;

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " #cond "\n"; \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void write(const std::string& p, const std::string& text) { std::ofstream(p) << text; }

static void test_sha() {
    CHECK(config::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(config::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string big(100000, 'a');
    CHECK(config::sha256_hex(big).size() == 64);
}

static void test_build_and_dirs() {
    char tmpl[] = "/tmp/cfg-test-XXXXXX";
    const std::string root = mkdtemp(tmpl);
    for (const char* d : {"/manager", "/manager/bin", "/managers", "/managers/rec", "/managers/rec/bin", "/workers", "/workers/cam",
                          "/workers/cam/workspace", "/workers/cam/workspace/bin", "/workers/_template", "/gateway"})
        ::mkdir((root + d).c_str(), 0755);
    write(root + "/manager/bin/process-manager", "binary-1");
    write(root + "/manager/bin/BUILD", "# comment\nsource_commit: abc123\nbuild_env: jetson-dev 2.1\nbuilt: 2026-11-24 10:00\nsha256:\n  " +
                                           config::sha256_hex("binary-1") + "  process-manager\n");
    write(root + "/managers/rec/bin/record-manager", "rec");
    write(root + "/managers/rec/bin/BUILD", "source_commit: def\nsha256:\n  " + std::string(64, '0') + "  record-manager\n");
    write(root + "/workers/cam/workspace/bin/BUILD", "source_commit: c1\nsha256:\n");
    const auto b = config::parse_build("source_commit: x1\nbuilt: t\nsha256:\n  " + std::string(64, 'a') + "  f\n");
    CHECK(b.commit == "x1" && b.built == "t" && b.sha.size() == 1 && b.sha.at("f") == std::string(64, 'a'));
    const auto dirs = config::bin_dirs(root);
    CHECK(dirs.size() == 3 && dirs.count("manager") && dirs.count("managers/rec") && dirs.count("workers/cam"));  // BUILD 없는 gateway·틀은 빠짐
    const Json pm = config::component_version(root + "/manager/bin");
    CHECK(pm["commit"] == "abc123" && pm["ok"] == true && pm["files"]["process-manager"]["ok"] == true);
    const Json rec = config::component_version(root + "/managers/rec/bin");
    CHECK(rec["ok"] == false && rec["files"]["record-manager"]["ok"] == false);  // 어긋남
    CHECK(config::component_version(root + "/gateway").is_null());
    CHECK(config::component_version(root + "/workers/cam/workspace/bin")["ok"] == false);  // sha 없음
    (void)!std::system(("rm -rf " + root).c_str());
}

static void test_diff() {
    const Json a = Json::parse(R"({"workers": {"cam1": {"preset": "default", "config": {"fps": 30, "dev": {"sn": "A"}}}, "old": {"config": {}}},
        "connections": {"connections": []}, "files": {"workspace.env": "A=1\nB=2\n"}, "versions": {"manager": {"commit": "c1"}}})");
    const Json b = Json::parse(R"({"workers": {"cam1": {"preset": "default", "config": {"fps": 15, "dev": {"sn": "A"}}}, "new": {"config": {}}},
        "connections": {"connections": [{"to": "a.b", "from": "c"}]}, "files": {"workspace.env": "A=1\nB=3\n"}, "versions": {"manager": {"commit": "c2"}}})");
    const Json d = config::diff(a, b);
    auto has = [&](const std::string& path) {
        for (const auto& c : d)
            if (c["path"] == path) return true;
        return false;
    };
    CHECK(has("workers.cam1.config.fps") && has("workers.old") && has("workers.new") && has("connections.connections"));
    CHECK(has("files.workspace.env") && has("versions.manager") && !has("workers.cam1.config.dev.sn"));
    for (const auto& c : d)
        if (c["path"] == "files.workspace.env") CHECK(c["removed"] == Json::parse(R"(["B=2"])") && c["added"] == Json::parse(R"(["B=3"])"));
    CHECK(config::diff(a, a).empty());
    Json a2 = a;
    a2["versions"]["manager"]["commit"] = "zzz";  // 버전만 다름: 설정은 같음
    a2["t"] = 5;
    CHECK(config::same_config(a, a2) && !config::same_config(a, b));
}

static void test_prune() {
    std::vector<config::SnapshotInfo> s = {{"1", "auto"}, {"2", "before-flight"}, {"3", "auto"}, {"4", "auto"}, {"5", ""}};
    CHECK(config::to_prune(s, 5).empty());
    CHECK((config::to_prune(s, 3) == std::vector<std::string>{"1", "3"}));  // 이름 붙인 것(2)은 건너뜀
    CHECK((config::to_prune(s, 1) == std::vector<std::string>{"1", "3", "4"}));  // auto가 다 지워져도 이름 붙인 것은 남김
}

int main() {
    test_sha();
    test_build_and_dirs();
    test_diff();
    test_prune();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "config core: all tests passed\n";
    return 0;
}
