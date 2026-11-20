// core 단위 시험(시스템 호출 없음): Python 판과 같은 결과인지 작은 예로 확인한다.
//   글·JSON(py_dumps, splitlines, utf8_replace, int/float), 경로 표, 화면 규칙(normpath, Content-Type, Host),
//   SSE 합치기(SseMerge·UpstreamState), 지난 지표(시각 해석·칸 평균·출력), 비밀값 가림·env 설명, ps·stat·빌드 줄, 토큰 검사
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

#include "board_parse.hpp"
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
    std::cout << (g_fail ? "FAILED " : "OK ") << g_checks - g_fail << "/" << g_checks << " checks\n";
    return g_fail ? 1 : 0;
}
