// 기록(stderr). Python 판과 같은 모양: "2026-11-20 10:00:00,123 INFO workspace.manager: ..."
#pragma once

#include <string>

namespace pm {

enum class Level { Debug = 10, Info = 20, Warning = 30, Error = 40 };

void set_log_level(const std::string& name);  // "DEBUG" | "INFO" | "WARNING" | "ERROR"
void log(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define PM_DEBUG(...) ::pm::log(::pm::Level::Debug, __VA_ARGS__)
#define PM_INFO(...) ::pm::log(::pm::Level::Info, __VA_ARGS__)
#define PM_WARN(...) ::pm::log(::pm::Level::Warning, __VA_ARGS__)
#define PM_ERROR(...) ::pm::log(::pm::Level::Error, __VA_ARGS__)

}  // namespace pm
