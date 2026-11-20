// 지난 지표 (design.md 6.9, Python logger/workspace_history.py의 query·render를 옮김). 파일은 부르는 쪽이 읽는다.
//
// 기록 한 줄: {"t": "2026-11-05T17:21:48.720+09:00", "type": "worker", "id": ..., "metrics": {...}, "resources": {...}, ...}
// 지표 = metrics + Manager가 잰 자원(res.*) + restart_count. points > 0이면 기간을 points칸으로 나눠 칸마다 평균
#pragma once

#include <string>
#include <vector>

#include "text.hpp"

namespace gw {

struct HistRow {
    double t = 0;
    Json m = Json::object();  // 숫자(·bool) 지표만, 원래 순서
};

// datetime.fromisoformat(text).timestamp(). 시간대가 없으면 지역 시각
bool parse_iso_ts(const std::string& text, double& out);
// datetime.fromtimestamp(t).astimezone().isoformat(timespec="seconds")
std::string iso_local(double t);

// 기록 한 줄 → 행. needle = "\"id\":\"<워커>\"". keys가 있으면 그 지표만(그 순서)
bool history_row(const std::string& line, const std::string& needle, double since, const std::vector<std::string>* keys,
                 HistRow& out);
// 시각 순 정렬 + 칸 평균(points > 0이고 행이 더 많을 때)
std::vector<HistRow> history_reduce(std::vector<HistRow> rows, double now, double since, int points);
// 줄 목록: CSV(첫 줄 머리) 또는 한 줄에 {"t", "m"} JSON
std::vector<std::string> history_render(const std::vector<HistRow>& rows, const std::vector<std::string>* keys, bool csv);

}  // namespace gw
