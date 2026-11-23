// core 단위 시험(시스템 호출 없음): Python 판과 같은 결과인지 작은 예로 확인한다.
//   글·JSON(py_dumps, splitlines, utf8_replace, int/float), 경로 표, 화면 규칙(normpath, Content-Type, Host),
//   SSE 합치기(SseMerge·UpstreamState), 지난 지표(시각 해석·칸 평균·출력), 비밀값 가림·env 설명, ps·stat·빌드 줄, 토큰 검사,
//   4단계: SHA-256·HMAC·PBKDF2 공개 벡터, 비밀번호 해시 형식, 사용자 파일, 역할 표, 세션 만료(가짜 시계), 로그인 실패 제한, 기록 줄
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

#include "access.hpp"
#include "board_parse.hpp"
#include "crypto.hpp"
#include "history.hpp"
#include "pipeline.hpp"
#include "profile_rules.hpp"
#include "routes.hpp"
#include "stream_state.hpp"
#include "text.hpp"
#include "ui_rules.hpp"

using namespace gw;

namespace {

int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_fail;                                                              \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                \
        ++g_checks;                                                                                     \
        const auto va = (a);                                                                            \
        const auto vb = (b);                                                                            \
        if (!(va == vb)) {                                                                              \
            ++g_fail;                                                                                   \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b "\n  got: " << va \
                      << "\n  want: " << vb << "\n";                                                    \
        }                                                                                               \
    } while (0)

void test_text() {
    Json o = Json::parse(R"({"b": [1, 2.5, true, null], "a": "가\n\u0001", "f": 1.0, "e": {}})");
    CHECK_EQ(py_dumps(o), std::string(R"({"b": [1, 2.5, true, null], "a": "가\n\u0001", "f": 1.0, "e": {}})"));
    CHECK_EQ(compact(o), std::string(R"({"b":[1,2.5,true,null],"a":"가\n\u0001","f":1.0,"e":{}})"));
    CHECK_EQ(compact(Json{{"도", 1}}, true), std::string("{\"\\ub3c4\":1}"));
    CHECK(!parse_json("\xEF\xBB\xBF{}"));
    CHECK(parse_json("[1]"));
    CHECK_EQ(utf8_replace("a\xff" "b\xe2\x82"), std::string("a\xEF\xBF\xBD" "b\xEF\xBF\xBD"));
    CHECK_EQ(utf8_replace("\xed\xa0\x80"), std::string("\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));  // 서로게이트
    auto l = splitlines("a\r\nb\rc\n\nd\xe2\x80\xa8" "e\n");
    CHECK_EQ(l.size(), 6u);
    CHECK_EQ(l[3], std::string(""));
    CHECK_EQ(l[4], std::string("d"));
    CHECK_EQ(l[5], std::string("e"));
    CHECK(splitlines("").empty());
    CHECK_EQ(universal_newlines("a\r\nb\rc"), std::string("a\nb\nc"));
    long long n;
    CHECK(py_int(" 1_000 ", n) && n == 1000);
    CHECK(!py_int("1__0", n));
    CHECK(!py_int("12.5", n));
    CHECK(py_int("-7", n) && n == -7);
    double d;
    CHECK(py_float("1e3", d) && d == 1000);
    CHECK(py_float("-inf", d) && d < 0);
    CHECK(!py_float("0x10", d));
    CHECK(!py_float("abc", d));
    CHECK_EQ(py_round(2.675, 2), 2.67);  // 2진 값이 2.67499..이라 Python도 2.67
    CHECK_EQ(py_round(0.25, 1), 0.2);    // 딱 절반은 짝수 쪽
    CHECK_EQ(fmt_fixed(12.25, 1), std::string("12.2"));
    CHECK_EQ(py_str(Json(true)), std::string("True"));
    CHECK_EQ(py_str(Json(1.5)), std::string("1.5"));
    CHECK_EQ(utf8_prefix("가나다", 2), std::string("가나"));
    CHECK_EQ(utf8_count("가a"), 2u);
    CHECK_EQ(errno_text(2), std::string("[Errno 2] No such file or directory"));
}

void test_routes() {
    auto r = route("GET", "/workers/w1/", false);
    CHECK(r.kind == RouteKind::Manager && r.op == "get" && r.id == "w1");
    r = route("POST", "/workers/w1/config/apply", false);
    CHECK(r.kind == RouteKind::Manager && r.op == "set_config" && r.save_false && r.body_key == "config" && r.ok_status == 202);
    r = route("POST", "/workers/w1/presets/p-1/use", false);
    CHECK(r.kind == RouteKind::Manager && r.op == "use_preset" && r.name == "p-1");
    CHECK(route("POST", "/workers/w1/presets/BAD/use", false).kind == RouteKind::None);
    CHECK(route("DELETE", "/workers/w1", false).kind == RouteKind::MethodNotAllowed);
    CHECK(route("GET", "/workers//", false).kind == RouteKind::None);
    CHECK(route("GET", "/workers/w1/stream", false).kind == RouteKind::Stream);
    CHECK(route("PUT", "/workers/w1/stream", false).kind == RouteKind::MethodNotAllowed);
    CHECK(route("POST", "/workers/w1/command/", false).kind == RouteKind::Command);
    CHECK(route("GET", "/health///", false).kind == RouteKind::Health);
    CHECK(route("POST", "/health", false).kind == RouteKind::None);
    r = route("GET", "/system/product-build/check/", false);
    CHECK(r.kind == RouteKind::ProductBuild && r.sub == "/check");
    CHECK(route("GET", "/system/product-build", true).kind == RouteKind::ProductOff);
    r = route("GET", "/workers/x/tasks", false);
    CHECK(r.kind == RouteKind::Read && r.read == "worker_tasks" && r.id == "x");
    CHECK(route("POST", "/logs/service", false).kind == RouteKind::None);
    CHECK(route("GET", "/history/a/b", false).kind == RouteKind::None);
    auto q = parse_qs("a=1&a=2&b=&c&d=x+y%2B%ED%95%9C&=z");
    CHECK_EQ(q["a"], std::string("2"));
    CHECK(!q.count("b") && !q.count("c"));
    CHECK_EQ(q["d"], std::string("x y+한"));
    CHECK_EQ(q[""], std::string("z"));
    CHECK(valid_worker_id("sys-mon.1"));
    CHECK(valid_worker_id("a\n"));
    CHECK(!valid_worker_id("-a"));
    CHECK(!valid_worker_id(std::string(65, 'a')));
    CHECK(valid_profile("p_1-x") && !valid_profile("a b") && !valid_power_mode("a-b"));
    CHECK(reads_body("PUT", route("PUT", "/workers/w1/config", false)));
    CHECK(!reads_body("POST", route("POST", "/workers/w1/start", false)));
    CHECK(ui_route("GET", "/api/remote/x").kind == UiKind::Remote);
    CHECK(ui_route("POST", "/api/board/x").kind == UiKind::Board);
    CHECK(ui_route("POST", "/api/panels").kind == UiKind::None);
}

void test_ui_rules() {
    CHECK_EQ(normpath("/a/b/../c/./d//e/"), std::string("/a/c/d/e"));
    CHECK_EQ(normpath("/../x"), std::string("/x"));
    CHECK_EQ(normpath("//a"), std::string("//a"));
    CHECK_EQ(normpath("a/../.."), std::string(".."));
    CHECK_EQ(contained_path("/w/web", "../web2/x"), std::string());
    CHECK_EQ(contained_path("/w/web", "/etc/passwd"), std::string());
    CHECK_EQ(contained_path("/w/web", "a/./b"), std::string("/w/web/a/b"));
    CHECK_EQ(relpath("/w/install/profiles/p1", "/w"), std::string("install/profiles/p1"));
    CHECK_EQ(relpath("/x/y", "/w/z"), std::string("../../x/y"));
    CHECK_EQ(content_type_for("/a/b.js"), std::string("text/javascript; charset=utf-8"));
    CHECK_EQ(content_type_for("/a/b.PNG"), std::string("image/png"));
    CHECK_EQ(content_type_for("/a/b.svg"), std::string("image/svg+xml; charset=utf-8"));
    CHECK_EQ(content_type_for("/a/b.json"), std::string("application/json; charset=utf-8"));
    CHECK_EQ(content_type_for("/a/noext"), std::string("application/octet-stream"));
    CHECK_EQ(content_type_for("/a/x.css.gz"), std::string("text/css; charset=utf-8"));
    CHECK_EQ(request_host("Example.COM:8080"), std::string("example.com"));
    CHECK_EQ(request_host("[::1]:80"), std::string("[::1]"));
    CHECK_EQ(request_host(""), std::string("127.0.0.1"));
    CHECK_EQ(request_host("u@h"), std::string("h"));
    Json panels = Json::parse(R"([{"id":"b","order":5},{"id":"a"},{"id":"c","order":1},{"id":"0"}])");
    sort_panels(panels);
    CHECK_EQ(panels[0]["id"].get<std::string>(), std::string("c"));
    CHECK_EQ(panels[2]["id"].get<std::string>(), std::string("0"));
    CHECK_EQ(panels[3]["id"].get<std::string>(), std::string("a"));
    std::string path;
    Params q;
    CHECK(remote_read_path("worker_log", Params{{"id", "w"}}, path, q) && path == "/logs/workers/w" && q["lines"] == "200");
    CHECK(remote_read_path("metrics_history", Params{{"id", "w"}, {"csv", "1"}, {"x", "y"}}, path, q) && q.size() == 1);
    CHECK(!remote_read_path("workspace", Params{}, path, q));
    auto lines = remote_lines("processes", Json::parse(R"({"processes":[{"pid":1,"name":"a b","cpu":1.25,"mem":0.0,"rss_kb":9}]})"));
    CHECK_EQ(lines[0], std::string("1 a_b 1.2 0.0 9"));
    lines = remote_lines("power_modes", Json::parse(R"({"modes":{"10":"B","2":"A"}})"));
    CHECK_EQ(lines[0], std::string("< POWER_MODEL ID=2 NAME=A >"));
}

void test_stream_state() {
    SseMerge m;
    m.offer(Json::parse(R"({"topics":{"v":{"r":0}},"events":[{"seq":1}],"stream":"up","t":1,"events_next":2})"), true);
    m.offer(Json::parse(R"({"topics":{"v":{"r":1},"l":{}},"events":[{"seq":2}],"t":2})"), false);
    CHECK(m.dirty());
    Json out = m.take();
    CHECK_EQ(out["seq"].get<int>(), 1);
    CHECK(out["kf"].get<bool>());
    CHECK_EQ(out["topics"]["v"]["r"].get<int>(), 1);
    CHECK_EQ(out["events"].size(), 2u);
    CHECK_EQ(out["t"].get<int>(), 2);
    std::string keys;
    for (auto it = out.begin(); it != out.end(); ++it) keys += it.key() + ",";
    CHECK_EQ(keys, std::string("seq,kf,topics,events,t,events_next,stream,"));
    CHECK(!m.dirty());
    m.offer(Json{{"stream", "down"}}, false);
    out = m.take();
    CHECK_EQ(compact(out), std::string(R"({"seq":2,"kf":false,"topics":{},"stream":"down"})"));
    for (int i = 0; i < 300; ++i) m.offer(Json{{"events", Json::array({Json{{"seq", i}}})}}, false);
    out = m.take();
    CHECK_EQ(out["events"].size(), 200u);
    CHECK_EQ(out["events"][0]["seq"].get<int>(), 100);

    UpstreamState u;
    Json kf = u.apply(Json::parse(R"({"kf":true,"topics":{"a":1},"events":[{"seq":1},{"seq":2}],"t":5,"events_next":3})"), true);
    CHECK_EQ(compact(kf), std::string(R"({"topics":{"a":1},"events":[{"seq":1},{"seq":2}],"stream":"up","t":5,"events_next":3})"));
    Json d = u.apply(Json::parse(R"({"kf":false,"topics":{"b":2},"events":[{"seq":2},{"seq":3}],"t":6})"), false);
    CHECK_EQ(compact(d), std::string(R"({"topics":{"b":2},"events":[{"seq":3}],"t":6})"));
    Json s = u.snapshot(false);
    CHECK_EQ(compact(s), std::string(R"({"topics":{"a":1,"b":2},"events":[{"seq":1},{"seq":2},{"seq":3}],"stream":"down","t":6,"events_next":3})"));
    // 다시 붙은 뒤: 상태를 비우고 이벤트 번호도 처음부터
    kf = u.apply(Json::parse(R"({"kf":true,"topics":{"c":1},"events":[{"seq":1}]})"), true);
    CHECK_EQ(kf["events"].size(), 1u);
    CHECK(!kf["topics"].contains("a"));
}

void test_history() {
    setenv("TZ", "TST-9", 1);  // 고정 +09:00(시간대 자료 없이)
    tzset();
    double t;
    CHECK(parse_iso_ts("2026-11-05T17:21:48.720+09:00", t) && std::abs(t - 1793866908.72) < 1e-6);
    CHECK(parse_iso_ts("2026-11-05T08:21:48Z", t) && std::abs(t - 1793866908.0) < 1e-6);
    CHECK(parse_iso_ts("2026-11-05 17:21:48", t) && std::abs(t - 1793866908.0) < 1e-6);  // 지역 시각
    CHECK(parse_iso_ts("2026-11-05", t));
    CHECK(!parse_iso_ts("2026-13-05T00:00:00", t));
    CHECK(!parse_iso_ts("2026-11-05T17:21:48+09:00x", t));
    CHECK_EQ(iso_local(1793866908.9999996), std::string("2026-11-05T17:21:49+09:00"));
    CHECK_EQ(iso_local(1793866908.4), std::string("2026-11-05T17:21:48+09:00"));

    const double since = 1793866000;
    HistRow row;
    const std::string needle = "\"id\":\"w\"";
    CHECK(!history_row(R"({"t":"2026-11-05T17:21:48+09:00","type":"system","id":"w","metrics":{"a":1}})", needle, since, nullptr, row));
    CHECK(history_row(R"({"t":"2026-11-05T17:21:48+09:00","type":"worker","id":"w","metrics":{"a":1,"s":"x","b":true},"resources":{"cpu":2},"restart_count":3})",
                      needle, since, nullptr, row));
    CHECK_EQ(compact(row.m), std::string(R"({"a":1,"b":true,"res.cpu":2,"restart_count":3})"));
    std::vector<std::string> keys = {"res.cpu", "zz", "a"};
    CHECK(history_row(R"({"t":"2026-11-05T17:21:48+09:00","type":"worker","id":"w","metrics":{"a":1},"resources":{"cpu":2}})",
                      needle, since, &keys, row));
    CHECK_EQ(compact(row.m), std::string(R"({"res.cpu":2,"a":1})"));
    CHECK(!history_row(R"({"t":"2026-11-05T17:00:00+09:00","type":"worker","id":"w","metrics":{"a":1}})", needle, since + 3600, nullptr, row));

    // 칸 평균: 기간 60 s, 4칸(15 s), 행 5개
    std::vector<HistRow> rows;
    for (double off : {1.0, 2.0, 16.0, 31.0, 50.0}) {
        HistRow r;
        r.t = since + off;
        r.m = Json{{"x", off}, {"b", true}};
        rows.push_back(r);
    }
    auto red = history_reduce(rows, since + 60, since, 4);
    CHECK_EQ(red.size(), 4u);
    CHECK_EQ(red[0].m["x"].get<double>(), 1.5);
    CHECK_EQ(red[0].t, since + 7.5);
    CHECK_EQ(red[0].m["b"].get<double>(), 1.0);
    CHECK_EQ(history_reduce(rows, since + 60, since, 0).size(), 5u);
    auto csv = history_render(red, nullptr, true);
    CHECK_EQ(csv[0], std::string("time,b,x"));
    CHECK_EQ(csv[1].substr(csv[1].find(',')), std::string(",1.0,1.5"));
    auto js = history_render(red, nullptr, false);
    CHECK_EQ(js[0].substr(0, 6), std::string("{\"t\":\""));
}

void test_profile_rules() {
    CHECK_EQ(mask_text("token: abc\n  db_password = q\n\"api_key\": \"x\",\nnothing\npass=\npass= \n{\"password\": 1}"),
             std::string("token: \xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\n  db_password = \xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\n"
                         "\"api_key\": \xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\nnothing\npass=\npass=\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\n{\"password\": 1}"));
    CHECK(secret_name("MY_API_KEY") && secret_name("PASSWD") && !secret_name("PATH"));
    Json doc = env_doc("## 화면\n# 제목\nTITLE=x\n\n# 주석 예\n# OPT=y\n## 묶음2\nA=1\n");
    CHECK_EQ(doc.size(), 3u);
    CHECK_EQ(compact(doc[0]), std::string(R"({"key":"TITLE","default":"x","example":null,"desc":"제목","group":"화면"})"));
    CHECK_EQ(compact(doc[1]), std::string(R"({"key":"OPT","default":null,"example":"y","desc":"주석 예","group":"화면"})"));
    CHECK_EQ(doc[2]["group"].get<std::string>(), std::string("묶음2"));
    CHECK_EQ(doc[2]["desc"].get<std::string>(), std::string(""));
    Json vals = env_values("A=1\n# B=2\nA=3\nC=\n");
    CHECK_EQ(compact(vals), std::string(R"({"A":"3","C":""})"));
    CHECK(profile_file_name("app.json") && !profile_file_name("README") && !profile_file_name("x.md") &&
          !profile_file_name(".h") && !profile_file_name("product.json"));
    CHECK_EQ(file_origin(std::nullopt, std::nullopt, std::nullopt, false), std::string("missing"));
    CHECK_EQ(file_origin(std::string("a"), std::string("a"), std::nullopt, true), std::string("profile"));
    CHECK_EQ(file_origin(std::string("a"), std::nullopt, std::string("a"), true), std::string("default"));
    CHECK_EQ(file_origin(std::string("a"), std::nullopt, std::nullopt, false), std::string("unknown"));
    Json env = Json::parse(R"([{"key":"API_KEY","default":"d"},{"key":"X","default":null}])");
    env_finish(env, Json{{"API_KEY", "v"}}, Json{{"X", "1"}});
    CHECK_EQ(env[0]["value"].get<std::string>(), std::string(kMask));
    CHECK_EQ(env[0]["default"].get<std::string>(), std::string(kMask));
    CHECK(env[1]["value"].is_null() && env[1]["in_profile"].get<bool>());
}

void test_board_parse() {
    Json p = parse_ps("  1 systemd  0.0  0.1 1234\n 22 my proc 12.5 1.0 99\nbad\n 3 x 1 2 3\n", 2);
    CHECK_EQ(p.size(), 2u);
    CHECK_EQ(compact(p[1]), std::string(R"({"pid":22,"name":"my proc","cpu":12.5,"mem":1.0,"rss_kb":99})"));
    bool threw = false;
    try {
        parse_ps("x a b c d\n", 5);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(compact(parse_nvpmodel("< POWER_MODEL ID=0 NAME=MAXN >\n< POWER_MODEL ID=1 NAME=15W >\nx\n")),
             std::string(R"({"0":"MAXN","1":"15W"})"));
    std::string comm;
    std::vector<std::string> f;
    CHECK(parse_proc_stat("123 (a (b) c) S 1 123 123 0", comm, f) && comm == "a (b) c" && f[0] == "S" && f[2] == "123");
    CHECK_EQ(policy_name(1), std::string("realtime(fifo)"));
    BuildState b;
    b.st = Json{{"state", "running"}, {"pct", 0}, {"step", ""}, {"error", nullptr}, {"result", nullptr}};
    b.apply_line("##PROGRESS 40 짓는 중");
    b.apply_line("##PROGRESS x");
    b.apply_line("   ");
    b.apply_line("plain");
    b.apply_line("##RESULT {\"file\": \"a\"}");
    CHECK_EQ(b.st["pct"].get<int>(), 40);
    CHECK_EQ(b.log.size(), 3u);
    CHECK_EQ(b.log[1], std::string("\xC2\xB7 "));
    b.finish(0, 10);
    CHECK_EQ(b.st["state"].get<std::string>(), std::string("done"));
    BuildState c;
    c.st = Json{{"state", "running"}, {"error", nullptr}, {"result", nullptr}};
    c.finish(-15, 1);
    CHECK_EQ(c.st["error"].get<std::string>(), std::string("exit -15"));
    for (int i = 0; i < 400; ++i) c.apply_line("l" + std::to_string(i));
    CHECK_EQ(c.log.size(), kBuildLogLines);
}

void test_auth() {
    auto hdr = [](std::map<std::string, std::string> h) {
        return [h](const char* k) {
            auto it = h.find(k);
            return it == h.end() ? std::string() : it->second;
        };
    };
    AuthOff open("", true), tok("s3", true), noui("s3", false);
    RequestView rq{"GET", "/workers", "", hdr({})};
    CHECK(open.authorize(Identity{}, rq).allowed);
    CHECK(!tok.authorize(Identity{}, rq).allowed);
    CHECK_EQ(tok.authorize(Identity{}, rq).status, 401);
    rq.header = hdr({{"Authorization", "Bearer s3 "}});
    CHECK(tok.authorize(Identity{}, rq).allowed);
    rq.header = hdr({{"Cookie", "a=1; workspace_token=s3"}});
    CHECK(tok.authorize(Identity{}, rq).allowed);
    rq.header = hdr({});
    rq.query = "token=s3";
    CHECK(tok.authorize(Identity{}, rq).allowed);
    rq.query = "";
    rq.path = "/kit.js";
    CHECK(tok.authorize(Identity{}, rq).allowed);
    CHECK(!noui.authorize(Identity{}, rq).allowed);
    rq.path = "/api/board/health/";
    CHECK(noui.authorize(Identity{}, rq).allowed);
    rq.path = "/logs/a.js";
    CHECK(!tok.authorize(Identity{}, rq).allowed);
    CHECK(constant_time_equal("abc", "abc") && !constant_time_equal("abc", "abd") && !constant_time_equal("ab", "abc"));
    CHECK(constant_time_equal("", "") && !constant_time_equal("", "a") && !constant_time_equal("abc", "ab"));
}

std::string unhex(const std::string& h) {
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out += static_cast<char>(std::stoi(h.substr(i, 2), nullptr, 16));
    return out;
}

// 공개 시험 벡터: NIST SHA-256(FIPS 180-2 예), RFC 4231 HMAC-SHA256, RFC 7914 11장·RFC 6070 꼴 PBKDF2-HMAC-SHA256
void test_crypto() {
    CHECK_EQ(sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(sha256_hex(std::string(1000000, 'a')),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    {
        // 조각으로 나눠 넣어도 같은 값(블록 경계 55·56·63·64·65)
        const std::string s(200, 'x');
        for (size_t cut : {1, 55, 56, 63, 64, 65, 119, 128}) {
            Sha256 h;
            h.update(s.substr(0, cut));
            h.update(s.substr(cut));
            const auto d = h.finish();
            CHECK_EQ(to_hex(std::string(reinterpret_cast<const char*>(d.data()), 32)), sha256_hex(s));
        }
    }
    CHECK_EQ(to_hex(hmac_sha256(std::string(20, '\x0b'), "Hi There")),
             std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    CHECK_EQ(to_hex(hmac_sha256("Jefe", "what do ya want for nothing?")),
             std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    CHECK_EQ(to_hex(hmac_sha256(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")),
             std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
    CHECK_EQ(to_hex(pbkdf2_sha256("passwd", "salt", 1, 64)),
             std::string("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                         "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));
    CHECK_EQ(to_hex(pbkdf2_sha256("Password", "NaCl", 80000, 64)),
             std::string("4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
                         "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d"));
    CHECK_EQ(to_hex(pbkdf2_sha256("password", "salt", 1, 32)),
             std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    CHECK_EQ(to_hex(pbkdf2_sha256("password", "salt", 2, 32)),
             std::string("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
    CHECK_EQ(to_hex(pbkdf2_sha256("password", "salt", 4096, 32)),
             std::string("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
    CHECK_EQ(to_hex(pbkdf2_sha256("passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 40)),
             std::string("348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9"));
    // base64(RFC 4648 10장)
    const char* b64[][2] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="},
                            {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (auto& v : b64) {
        CHECK_EQ(base64_encode(v[0]), std::string(v[1]));
        std::string d;
        CHECK(base64_decode(v[1], d) && d == v[0]);
    }
    std::string d;
    CHECK(!base64_decode("Zm9", d));
    CHECK(!base64_decode("Zm=v", d));
    CHECK(!base64_decode("Z!9v", d));
    CHECK(!base64_decode("Zg==Zg==", d));
    const std::string bin = unhex("00ff10e0");
    CHECK(base64_decode(base64_encode(bin), d) && d == bin);
}

void test_password_hash() {
    const std::string salt = "0123456789abcdef";
    const std::string h = make_password_hash("correct horse", salt, 1000);
    CHECK(starts_with(h, "pbkdf2-sha256$1000$MDEyMzQ1Njc4OWFiY2RlZg==$"));
    PasswordHash p;
    CHECK(parse_password_hash(h, p));
    CHECK_EQ(p.iterations, 1000u);
    CHECK_EQ(p.salt, salt);
    CHECK_EQ(p.hash, pbkdf2_sha256("correct horse", salt, 1000, 32));
    CHECK(verify_password(h, "correct horse"));
    CHECK(!verify_password(h, "correct horsE"));
    CHECK(!verify_password(h, ""));
    // 형식 오류는 모두 거절(로그인 실패로)
    for (const std::string& bad : {std::string(""), std::string("scrypt$1000$AAAA$AAAA"), std::string("pbkdf2-sha256$1000$MDEy"),
                                  std::string("pbkdf2-sha256$999$MDEyMzQ1Njc4OWFiY2RlZg==$") + base64_encode(std::string(32, 'x')),
                                  std::string("pbkdf2-sha256$99999999$MDEyMzQ1Njc4OWFiY2RlZg==$") + base64_encode(std::string(32, 'x')),
                                  std::string("pbkdf2-sha256$1x00$MDEyMzQ1Njc4OWFiY2RlZg==$") + base64_encode(std::string(32, 'x')),
                                  std::string("pbkdf2-sha256$1000$MDEyMzQ1Njc4OWFiY2RlZg==$") + base64_encode(std::string(31, 'x')),
                                  std::string("pbkdf2-sha256$1000$MDEy$") + base64_encode(std::string(32, 'x')),
                                  h + "$x"}) {
        CHECK(!parse_password_hash(bad, p));
        CHECK(!verify_password(bad, "correct horse"));
    }
    // 기본 반복 수 그대로 만든 값도 맞는다
    const std::string hd = make_password_hash("pw-default", salt, kPbkdf2Iterations);
    CHECK(starts_with(hd, "pbkdf2-sha256$200000$"));
    CHECK(verify_password(hd, "pw-default"));
}

void test_users_file() {
    const std::string tok = "abc123";
    const std::string text = std::string(R"({"users": [
        {"name": "op1", "role": "operator", "password_hash": ")") + make_password_hash("pw-op1", "saltsaltsalt", 1000) +
        R"(", "tokens": [{"name": "ext", "hash": ")" + sha256_hex(tok) + R"("}]},
        {"name": "v.1", "role": "viewer", "password_hash": "", "tokens": []}]})";
    UsersDb db;
    std::string err;
    CHECK(parse_users(text, db, err));
    CHECK_EQ(db.users.size(), 2u);
    CHECK(db.find("op1") && db.find("op1")->role == Role::Operator);
    std::string tn;
    CHECK(db.by_token(tok, &tn) == db.find("op1") && tn == "ext");
    CHECK(!db.by_token("abc124"));
    CHECK(!db.by_token(""));
    // 다시 쓰고 읽어도 같다
    UsersDb again;
    CHECK(parse_users(dump_users(db), again, err));
    CHECK_EQ(dump_users(again), dump_users(db));
    CHECK(dump_users(db).find(tok) == std::string::npos);  // 토큰 원문은 파일에 없다
    for (const char* bad : {"[]", R"({"users": {}})", R"({"users": [{"name": "a b", "role": "viewer"}]})",
                            R"({"users": [{"name": "a", "role": "admin"}]})",
                            R"({"users": [{"name": "a", "role": "viewer"}, {"name": "a", "role": "viewer"}]})",
                            R"({"users": [{"name": "a", "role": "viewer", "password_hash": "x"}]})",
                            R"({"users": [{"name": "a", "role": "viewer", "tokens": [{"name": "t", "hash": "ABC"}]}]})",
                            "{not json"}) {
        UsersDb x;
        CHECK(!parse_users(bad, x, err));
    }
    CHECK(valid_user_name("op1") && valid_user_name("a.b-c_d") && !valid_user_name("") && !valid_user_name("-a") &&
          !valid_user_name("a/b") && !valid_user_name(std::string(65, 'a')));
}

// 역할 표: 경로 종류마다 viewer·operator·maintainer 허용·거절
void test_roles() {
    struct Case {
        const char* method;
        const char* path;
        Need need;
    };
    const Case cases[] = {
        {"GET", "/health", Need::Open},
        {"GET", "/api/board/health/", Need::Open},
        {"GET", "/", Need::Open},
        {"GET", "/kit.js", Need::Open},
        {"GET", "/panels/board/panel.js", Need::Open},
        {"POST", "/api/auth/login", Need::Open},
        {"POST", "/api/board/auth/logout", Need::Open},
        {"GET", "/api/auth/me", Need::Open},
        {"GET", "/workers", Need::Read},
        {"GET", "/api/board/workers/w1", Need::Read},
        {"GET", "/system", Need::Read},
        {"GET", "/workers/w1/stream", Need::Read},
        {"GET", "/workers/w1/io", Need::Read},
        {"GET", "/logs/workers/w1", Need::Read},
        {"GET", "/api/remote/commands", Need::Read},
        {"GET", "/api/panels", Need::Read},
        {"GET", "/api/ui", Need::Read},
        {"GET", "/api/mode", Need::Read},
        {"GET", "/workers/w1/panel.js", Need::Read},  // 워커 경로 아래는 화면 파일로 보지 않음
        {"POST", "/workers/w1/command", Need::Operate},
        {"POST", "/api/board/workers/w1/command/", Need::Operate},
        {"POST", "/workers/w1/start", Need::Maintain},
        {"POST", "/workers/w1/stop", Need::Maintain},
        {"PUT", "/workers/w1/config", Need::Maintain},
        {"POST", "/workers/w1/config/apply", Need::Maintain},
        {"POST", "/workers/w1/presets", Need::Maintain},
        {"DELETE", "/workers/w1/presets/a", Need::Maintain},
        {"PUT", "/workers/w1/autostart", Need::Maintain},
        {"GET", "/system/product-build", Need::Maintain},
        {"POST", "/api/board/system/product-build", Need::Maintain},
        {"GET", "/system/product-build/download", Need::Maintain},
        {"PUT", "/api/mode", Need::Maintain},
        {"PUT", "/api/board/mode", Need::Maintain},
        {"POST", "/workers/w1/command/x", Need::Maintain},  // 명령 경로가 아님(없는 경로, 쓰기로 봄)
        {"POST", "/kit.js", Need::Maintain},
    };
    for (const auto& c : cases) {
        const Need n = classify(c.method, c.path, true);
        if (n != c.need) std::cerr << "  classify " << c.method << " " << c.path << " -> " << need_name(n) << "\n";
        CHECK(n == c.need);
    }
    CHECK(classify("GET", "/kit.js", false) == Need::Read);  // 화면 끔이면 화면 파일도 열지 않음
    const Role roles[] = {Role::None, Role::Viewer, Role::Operator, Role::Maintainer};
    const bool expect[4][4] = {  // [역할][Open, Read, Operate, Maintain]
        {true, false, false, false},
        {true, true, false, false},
        {true, true, true, false},
        {true, true, true, true}};
    const Need needs[] = {Need::Open, Need::Read, Need::Operate, Need::Maintain};
    for (int r = 0; r < 4; ++r)
        for (int n = 0; n < 4; ++n) CHECK(allows(roles[r], needs[n]) == expect[r][n]);
    Role role;
    CHECK(parse_role("operator", role) && role == Role::Operator && !parse_role("admin", role));
    CHECK_EQ(std::string(role_name(Role::Maintainer)), std::string("maintainer"));
    // 게이트웨이가 직접 답하는 경로
    CHECK(own_route("POST", "/api/auth/login") == Own::Login);
    CHECK(own_route("POST", "/api/board/auth/login") == Own::Login);
    CHECK(own_route("GET", "/api/auth/login") == Own::MethodNotAllowed);
    CHECK(own_route("GET", "/api/auth/me/") == Own::Me);
    CHECK(own_route("POST", "/auth/logout") == Own::Logout);
    CHECK(own_route("GET", "/api/mode") == Own::ModeGet);
    CHECK(own_route("PUT", "/api/board/mode") == Own::ModeSet);
    CHECK(own_route("DELETE", "/api/mode") == Own::MethodNotAllowed);
    CHECK(own_route("GET", "/workers") == Own::None);
    CHECK(own_route("GET", "/api/board/workers") == Own::None);
}

void test_sessions() {
    Sessions s(100, 1000, 3);  // 쉼 100 s, 절대 1000 s, 최대 3개(가짜 시계: 시각을 넘김)
    s.add("a", "u1", "h1", 0);
    CHECK(s.touch("a", 50).has_value());
    CHECK(s.touch("a", 149).has_value());   // 마지막 사용(50) 뒤 99 s
    CHECK(!s.touch("a", 249).has_value());  // 마지막 사용(149) 뒤 100 s: 쉼 만료
    CHECK(!s.touch("a", 250).has_value());  // 지워짐
    s.add("b", "u2", "h2", 0);
    for (double t = 90; t < 1000; t += 90) CHECK(s.touch("b", t).has_value());
    CHECK(!s.touch("b", 1000).has_value());  // 계속 써도 절대 만료(7일 꼴)
    auto x = Sessions(100, 1000, 3);
    x.add("1", "u", "h", 0);
    x.add("2", "u", "h", 1);
    x.add("3", "u", "h", 2);
    CHECK(x.touch("1", 3).has_value());  // 1이 가장 최근에 씀
    x.add("4", "u", "h", 4);             // 넘침: 가장 오래 안 쓴 2가 빠짐
    CHECK(x.size(5) == 3u);
    CHECK(!x.touch("2", 5).has_value());
    CHECK(x.touch("1", 5).has_value() && x.touch("4", 5).has_value());
    const auto info = x.touch("4", 6);
    CHECK(info && info->user == "u" && info->stamp == "h" && info->created == 4);
    x.remove("4");
    CHECK(!x.touch("4", 7).has_value());
    CHECK(x.size(500) == 0u);  // 모두 쉼 만료
}

void test_limiter() {
    LoginLimiter l(5, 30, 4);
    for (int i = 0; i < 4; ++i) l.failure("a", 10);
    CHECK(l.locked_for("a", 10) == 0);
    l.failure("a", 11);  // 다섯 번째
    CHECK(l.locked_for("a", 11) > 29.9);
    CHECK(l.locked_for("b", 11) == 0);  // 다른 주소는 그대로
    CHECK(l.locked_for("a", 40.9) > 0);
    CHECK(l.locked_for("a", 41.1) == 0);  // 30 s 뒤 풀림
    for (int i = 0; i < 4; ++i) l.failure("c", 50);
    l.success("c");  // 성공하면 셈을 지움
    l.failure("c", 51);
    CHECK(l.locked_for("c", 51) == 0);
    for (int i = 0; i < 4; ++i) l.failure("d", 100);
    l.failure("d", 100 + 11 * 60);  // 10분 넘게 지난 실패는 잊음
    CHECK(l.locked_for("d", 100 + 11 * 60) == 0);
    // 표가 차도(주소를 바꿔 가며) 막힌 주소는 남는다
    for (int i = 0; i < 5; ++i) l.failure("z", 2000);
    for (int i = 0; i < 20; ++i) l.failure("x" + std::to_string(i), 2001);
    CHECK(l.locked_for("z", 2002) > 0);
}

void test_record_line() {
    RecordFields f;
    f.t = "2026-11-23T10:00:00.123+09:00";
    f.user = "op1";
    f.role = "operator";
    f.mode = "operation";
    f.remote = "10.0.0.1";
    f.method = "POST";
    f.path = "/workers/w1/command";
    f.cmd = "source.set_rate";
    f.code = 200;
    f.ms = 1.234;
    CHECK_EQ(record_line(f), std::string(R"({"t":"2026-11-23T10:00:00.123+09:00","user":"op1","role":"operator",)"
                                         R"("mode":"operation","remote":"10.0.0.1","method":"POST","path":"/workers/w1/command",)"
                                         R"("cmd":"source.set_rate","code":200,"ms":1.2})" "\n"));
    f.cmd.clear();
    f.path = std::string(2000, 'p');
    const std::string l = record_line(f);
    CHECK(l.find("\"cmd\":null") != std::string::npos);
    CHECK(l.size() < 1000);  // 긴 칸은 자름
    CHECK(l.back() == '\n' && std::count(l.begin(), l.end(), '\n') == 1);
    // 기록 줄에는 본문·쿼리·헤더 칸이 없다(비밀번호·토큰이 들어갈 자리가 없음)
    CHECK(Json::parse(l).size() == 10u);
    CHECK_EQ(redact_query("a=1&token=s3cret&x=&password=pw&TOKEN=z"), std::string("a=1&token=***&x=&password=***&TOKEN=***"));
    CHECK_EQ(redact_query(""), std::string(""));
    CHECK_EQ(cookie_value("a=1; workspace_session=abc; b=2", "workspace_session"), std::string("abc"));
    CHECK_EQ(cookie_value("workspace_session=a; workspace_session=b", "workspace_session"), std::string("b"));
    CHECK_EQ(cookie_value("x_workspace_session=a", "workspace_session"), std::string(""));
    CHECK_EQ(bearer_token("Bearer  abc "), std::string("abc"));
    CHECK_EQ(bearer_token("Basic abc"), std::string(""));
}

}  // namespace

int main() {
    test_text();
    test_routes();
    test_ui_rules();
    test_stream_state();
    test_history();
    test_profile_rules();
    test_board_parse();
    test_auth();
    test_crypto();
    test_password_hash();
    test_users_file();
    test_roles();
    test_sessions();
    test_limiter();
    test_record_line();
    std::cout << (g_fail ? "FAILED " : "OK ") << g_checks - g_fail << "/" << g_checks << " checks\n";
    return g_fail ? 1 : 0;
}
