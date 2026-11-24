// 모듈 목록(managers/payload/design.md 2장). 시스템 호출을 모른다.
//
//   모듈 = 매니저(kind manager)가 아닌 워커. 칸: roles(계약 provides), state·desired·autostart(process-manager),
//          health·why(status-manager 주제 system, 없으면 null), groups(규칙 groups에서 든 묶음)
//   묶음 = 규칙 groups {"<이름>": ["<워커 id 글롭>", ...]}
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace payload {

using Json = nlohmann::json;
using Groups = std::map<std::string, std::vector<std::string>>;

// 규칙 → 묶음(틀리면 std::invalid_argument)
Groups parse_groups(const Json& groups);

class Modules {
public:
    explicit Modules(Groups groups = {}) : groups_(std::move(groups)) {}

    void on_worker(const Json& data);  // process-manager worker_status의 data
    void on_removed(const std::string& id);
    void set_roles(const std::string& id, std::vector<std::string> roles);
    void set_health(const Json& system);  // status-manager 주제 system(null이면 건강을 모름)

    bool known(const std::string& id) const { return workers_.count(id) > 0; }
    bool is_manager(const std::string& id) const;
    bool is_module(const std::string& id) const { return known(id) && !is_manager(id); }
    bool has_group(const std::string& name) const { return groups_.count(name) > 0; }
    // 묶음에 든 모듈(지금 있는 것 중 글롭에 맞는 것, 매니저 빼고, 이름순)
    std::vector<std::string> members(const std::string& group) const;
    std::vector<std::string> ids() const;

    Json snapshot() const;  // {"modules": {...}, "groups": {...}}
    size_t count() const;
    size_t running() const;

private:
    struct Entry {
        Json data;
        std::vector<std::string> roles;
    };
    Groups groups_;
    std::map<std::string, Entry> workers_;
    Json health_;  // system.workers
};

}  // namespace payload
