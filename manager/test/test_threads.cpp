// pm.store·pm.pub·pm.res 스레드 시험. ThreadSanitizer 빌드로도 돌린다(dev/check.sh와 같은 방식).
//   - pm.store: 낸 순서대로 done, 큐가 차면 그 요청만 실패, 끝낼 때 남은 요청을 다 씀
//   - pm.pub: 첫 줄(구독 응답) → 이벤트, 키마다 최신값만, 안 읽는 구독자만 끊고 다른 구독자는 계속 받음
//   - pm.res: 자기 프로세스 그룹을 재면 프로세스 1개 이상
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "posix_fs.hpp"
#include "publisher.hpp"
#include "resources.hpp"
#include "store.hpp"

namespace {

int g_fail = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ++g_fail;                                                              \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        }                                                                          \
    } while (0)

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string tmpdir() {
    char tmpl[] = "/tmp/pm-threads-XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    return d ? d : "/tmp";
}

void test_store() {
    const std::string dir = tmpdir();
    pm::StoreThread store(2.0, 64);
    store.start();
    std::vector<int> order;
    int fails = 0;
    for (int i = 0; i < 200; ++i) {
        pm::FileOp op;
        op.path = dir + "/sub/f" + std::to_string(i % 10) + ".json";
        op.text = "{\"i\": " + std::to_string(i) + "}\n";
        op.sync_dir = i % 2 == 0;
        store.submit({op}, [&order, &fails, i](bool ok, const std::string&) {
            if (ok) order.push_back(i);
            else ++fails;
        });
    }
    const double end = now() + 60;
    while ((static_cast<int>(order.size()) + fails) < 200 && now() < end) {
        pollfd p{store.wake_fd(), POLLIN, 0};
        ::poll(&p, 1, 50);
        store.poll(now());
    }
    CHECK(static_cast<int>(order.size()) + fails == 200);
    CHECK(fails > 0);  // 큐 64: 한 번에 200을 넣으면 일부는 "queue full"
    bool sorted = true;
    for (size_t i = 1; i < order.size(); ++i) sorted = sorted && order[i - 1] < order[i];
    CHECK(sorted);  // 낸 순서대로
    // 끝낼 때 남은 요청을 다 쓴다
    for (int i = 0; i < 10; ++i) {
        pm::FileOp op;
        op.path = dir + "/last.json";
        op.text = std::to_string(i);
        store.submit({op}, nullptr);
    }
    store.stop(60.0);  // fsync가 느린 디스크(WSL ~70 ms)도 다 쓸 만큼
    pm::PosixFs fs;
    std::string text, err;
    CHECK(fs.read(dir + "/last.json", text, err) == pm::FileSystem::Read::Ok && text == "9");
    CHECK(!fs.exists(dir + "/last.json.tmp"));
    if (std::system(("rm -rf '" + dir + "'").c_str()) != 0) std::cerr << "cleanup failed\n";
}

std::vector<std::string> read_lines(int fd, double timeout_s, size_t want) {
    std::vector<std::string> out;
    std::string buf;
    const double end = now() + timeout_s;
    while (now() < end && out.size() < want) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 50) <= 0) continue;
        char tmp[65536];
        const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            out.push_back(buf.substr(0, nl));
            buf.erase(0, nl + 1);
        }
    }
    return out;
}

void test_publisher() {
    worker::ThreadWatch watch(std::chrono::milliseconds(3000));
    pm::Publisher pub(watch, 0.5);
    pub.start();
    int fast[2], slow[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fast) == 0);
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, slow) == 0);
    const int small = 4096;
    ::setsockopt(slow[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    CHECK(pub.add(1, fast[0], {"workers"}, "{\"ok\": true}\n"));
    CHECK(pub.add(2, slow[0], {"workers"}, "{\"ok\": true}\n"));
    pub.offer({"system", "s", "ignored\n"});  // 구독하지 않은 topic
    pub.offer({"workers", "k1", "a1\n"});
    auto lines = read_lines(fast[1], 2, 2);
    CHECK(lines.size() == 2 && lines[0] == "{\"ok\": true}" && lines[1] == "a1");
    // 큰 줄을 계속: 안 읽는 구독자(2)만 끊기고 1은 계속 받는다
    const std::string big(8000, 'x');
    std::vector<std::string> got;
    const double end = now() + 3;
    bool dropped = false;
    int n = 0;
    while (now() < end) {
        pub.offer({"workers", "k" + std::to_string(n % 3), big + std::to_string(n) + "\n"});
        ++n;
        auto l = read_lines(fast[1], 0.02, 1000);
        got.insert(got.end(), l.begin(), l.end());
        for (int id : pub.take_dropped()) dropped = dropped || id == 2;
        if (dropped && got.size() > 20) break;
    }
    CHECK(dropped);
    CHECK(got.size() > 20);
    // 끊긴 쪽은 쌓인 것을 읽고 나면 EOF
    ::fcntl(slow[1], F_SETFL, O_NONBLOCK);
    char tmp[65536];
    ssize_t r;
    const double end2 = now() + 2;
    bool eof = false;
    while (now() < end2) {
        r = ::recv(slow[1], tmp, sizeof tmp, 0);
        if (r == 0) {
            eof = true;
            break;
        }
        if (r < 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(eof);
    pub.stop();
    ::close(fast[1]);
    ::close(slow[1]);
    std::string stalled;
    CHECK(watch.all_alive(worker::ThreadWatch::Clock::now(), &stalled));
}

void test_resources() {
    worker::ThreadWatch watch(std::chrono::milliseconds(3000));
    pm::Resources res(watch);
    res.set_pgids({static_cast<int>(::getpgrp())});
    res.start();
    std::map<int, pm::Json> got;
    int samples = 0;
    const double end = now() + 4;
    while (now() < end && samples < 2) {
        pollfd p{res.wake_fd(), POLLIN, 0};
        ::poll(&p, 1, 100);
        std::map<int, pm::Json> m;
        if (res.take(m)) {
            ++samples;
            got = m;
        }
    }
    res.stop();
    CHECK(samples == 2);
    const auto it = got.find(static_cast<int>(::getpgrp()));
    CHECK(it != got.end());
    if (it != got.end()) {
        CHECK(it->second["procs"].get<long long>() >= 1);
        CHECK(it->second["threads"].get<long long>() >= 2);  // 이 시험 + pm.res
        CHECK(it->second["cpu_pct"].is_number());            // 두 번째 표본
    }
}

// 전체 훑기는 처음·kFullScanS마다·새 그룹·사라진 pid일 때만, 그 사이는 기억한 pid만 읽어도 같은 값
void test_sampler_cache() {
    pm::ResourceSampler s;
    const int g = static_cast<int>(::getpgrp());
    auto a = s.sample({g}, 100.0);
    CHECK(s.full_scans() == 1);
    auto b = s.sample({g}, 101.0);  // 기억한 pid만
    CHECK(s.full_scans() == 1);
    CHECK(a[g]["procs"] == b[g]["procs"] && b[g]["threads"].get<long long>() >= 1 && b[g]["cpu_pct"].is_number());
    s.sample({g}, 100.0 + pm::ResourceSampler::kFullScanS + 0.1);  // 주기가 됨
    CHECK(s.full_scans() == 2);
    s.sample({g, 999999}, 106.0);  // 새 그룹(없는 그룹이라도 처음엔 훑음)
    CHECK(s.full_scans() == 3);
    // 자식을 하나 만들어 끝내면(기억한 pid가 사라짐) 다음 표본이 전체 훑기로 바뀜
    const pid_t child = ::fork();
    if (child == 0) {
        ::pause();
        ::_exit(0);
    }
    s.sample({g}, 112.0);  // 주기: 자식까지 기억
    const int before = s.full_scans();
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    s.sample({g}, 113.0);
    CHECK(s.full_scans() == before + 1);
}

}  // namespace

int main() {
    test_store();
    std::cout << "store done\n";
    test_publisher();
    std::cout << "publisher done\n";
    test_resources();
    test_sampler_cache();
    std::cout << "resources done\n";
    std::cout << (g_fail ? "FAILED" : "OK") << " (" << g_fail << " failures)\n";
    return g_fail ? 1 : 0;
}
