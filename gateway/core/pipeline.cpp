#include "pipeline.hpp"

#include "routes.hpp"
#include "text.hpp"

namespace gw {

std::string bearer_token(const std::string& authorization) {
    return starts_with(authorization, "Bearer ") ? strip(authorization.substr(7)) : "";
}

Decision AuthOff::authorize(const Identity&, const RequestView& rq) {
    Decision d;
    if (token_.empty() || open_path(rq.path, ui_enabled_)) return d;
    std::string got = bearer_token(rq.header("Authorization"));
    // Cookie: workspace_token=<토큰>(같은 이름이 여럿이면 마지막)
    if (got.empty()) got = cookie_value(rq.header("Cookie"), "workspace_token");
    if (got.empty()) {
        // EventSource는 헤더를 못 붙인다(같은 출처면 쿠키로도 됨)
        const Params q = parse_qs(rq.query);
        auto it = q.find("token");
        if (it != q.end()) got = it->second;
    }
    if (constant_time_equal(got, token_)) return d;
    d.allowed = false;
    d.status = 401;
    d.code = "unauthorized";
    d.message = "token required (Authorization: Bearer <token>)";
    return d;
}

}  // namespace gw
