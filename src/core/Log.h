#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace vkr::log {

enum class Level {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

struct Settings {
    Level       consoleLevel = Level::Info;
    Level       fileLevel = Level::Trace;
    bool        enableFile = true;
    bool        enableColor = true;
    std::string filePath = "logs/VulkanLab.log";
};

void init(const Settings &settings = {});
void shutdown();
void setConsoleLevel(Level level);

std::shared_ptr<spdlog::logger> logger(std::string_view tag);

} // namespace vkr::log

#define VKR_LOG_TRACE(tag, ...)                                                \
    SPDLOG_LOGGER_TRACE(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_DEBUG(tag, ...)                                                \
    SPDLOG_LOGGER_DEBUG(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_INFO(tag, ...)                                                 \
    SPDLOG_LOGGER_INFO(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_WARN(tag, ...)                                                 \
    SPDLOG_LOGGER_WARN(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_ERROR(tag, ...)                                                \
    SPDLOG_LOGGER_ERROR(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_CRITICAL(tag, ...)                                             \
    SPDLOG_LOGGER_CRITICAL(::vkr::log::logger(tag), __VA_ARGS__)