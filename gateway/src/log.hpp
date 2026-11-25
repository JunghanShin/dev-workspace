// 기록(stderr). Python REST와 같은 모양: "2026-11-20 10:00:00,123 INFO workspace.gateway: ..."
#pragma once

#include <string>

namespace gw {

enum class Level { Debug = 10, Info = 20, Warning = 30, Error = 40 };

void set_log_level(const std::string& name);  // "DEBUG" | "INFO" | "WARNING" | "ERROR"(실행 중에도, 설정 탭)
std::string log_level_name();                 // 지금 수준 이름
bool log_enabled(Level level);
void log(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define GW_DEBUG(...) ::gw::log(::gw::Level::Debug, __VA_ARGS__)
#define GW_INFO(...) ::gw::log(::gw::Level::Info, __VA_ARGS__)
#define GW_WARN(...) ::gw::log(::gw::Level::Warning, __VA_ARGS__)
#define GW_ERROR(...) ::gw::log(::gw::Level::Error, __VA_ARGS__)

}  // namespace gw
