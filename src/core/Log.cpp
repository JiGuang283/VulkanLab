#include "Log.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vkr::log {

namespace {

std::mutex                                                       loggerMutex;
std::vector<spdlog::sink_ptr>                                    sinks;
std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> loggers;
bool initialized = false;

std::string lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool truthy(const char *value) {
    if (!value)
        return false;
    std::string normalized = lower(value);
    return normalized == "1" || normalized == "true" || normalized == "yes" ||
           normalized == "on";
}

Level parseLevel(const char *value, Level fallback) {
    if (!value)
        return fallback;

    const std::string normalized = lower(value);
    if (normalized == "trace")
        return Level::Trace;
    if (normalized == "debug")
        return Level::Debug;
    if (normalized == "info")
        return Level::Info;
    if (normalized == "warn" || normalized == "warning")
        return Level::Warn;
    if (normalized == "error" || normalized == "err")
        return Level::Error;
    if (normalized == "critical")
        return Level::Critical;
    if (normalized == "off")
        return Level::Off;
    return fallback;
}

spdlog::level::level_enum toSpdlog(Level level) {
    switch (level) {
    case Level::Trace:
        return spdlog::level::trace;
    case Level::Debug:
        return spdlog::level::debug;
    case Level::Info:
        return spdlog::level::info;
    case Level::Warn:
        return spdlog::level::warn;
    case Level::Error:
        return spdlog::level::err;
    case Level::Critical:
        return spdlog::level::critical;
    case Level::Off:
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

spdlog::level::level_enum minLevel(Level lhs, Level rhs) {
    return std::min(toSpdlog(lhs), toSpdlog(rhs));
}

void applyLoggerLevel(const std::shared_ptr<spdlog::logger> &value,
                      const Settings                        &settings) {
    value->set_level(settings.enableFile
                         ? minLevel(settings.consoleLevel, settings.fileLevel)
                         : toSpdlog(settings.consoleLevel));
    value->flush_on(spdlog::level::warn);
}

Settings settingsFromEnv(Settings settings) {
    settings.consoleLevel =
        parseLevel(std::getenv("VKR_LOG_LEVEL"), settings.consoleLevel);
    settings.fileLevel =
        parseLevel(std::getenv("VKR_LOG_FILE_LEVEL"), settings.fileLevel);
    if (const char *file = std::getenv("VKR_LOG_FILE"))
        settings.filePath = file;
    if (truthy(std::getenv("VKR_LOG_NO_COLOR")))
        settings.enableColor = false;
    return settings;
}

} // namespace

void init(const Settings &inputSettings) {
    std::lock_guard<std::mutex> lock(loggerMutex);
    if (initialized)
        return;

    Settings settings = settingsFromEnv(inputSettings);

    spdlog::sink_ptr consoleSink;
    if (settings.enableColor) {
        consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    } else {
        consoleSink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
    }
    consoleSink->set_level(toSpdlog(settings.consoleLevel));
    consoleSink->set_pattern("%^[%H:%M:%S.%e] [%l] [%n] %v%$");
    sinks.push_back(std::move(consoleSink));

    if (settings.enableFile) {
        const std::filesystem::path logPath(settings.filePath);
        if (logPath.has_parent_path())
            std::filesystem::create_directories(logPath.parent_path());

        auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            settings.filePath, 1024 * 1024 * 5, 3, false);
        fileSink->set_level(toSpdlog(settings.fileLevel));
        fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [%s:%# %!] %v");
        sinks.push_back(std::move(fileSink));
    }

    auto root =
        std::make_shared<spdlog::logger>("App", sinks.begin(), sinks.end());
    applyLoggerLevel(root, settings);
    spdlog::set_default_logger(root);
    spdlog::register_or_replace(root);
    loggers.emplace("App", std::move(root));

    initialized = true;
}

void shutdown() {
    std::lock_guard<std::mutex> lock(loggerMutex);
    loggers.clear();
    sinks.clear();
    spdlog::shutdown();
    initialized = false;
}

void setConsoleLevel(Level level) {
    std::lock_guard<std::mutex> lock(loggerMutex);
    if (sinks.empty())
        return;
    sinks.front()->set_level(toSpdlog(level));
}

std::shared_ptr<spdlog::logger> logger(std::string_view tag) {
    bool needsInit = false;
    {
        std::lock_guard<std::mutex> lock(loggerMutex);
        needsInit = !initialized;
    }

    if (needsInit)
        init();

    std::lock_guard<std::mutex> lock(loggerMutex);

    std::string name(tag);
    auto        found = loggers.find(name);
    if (found != loggers.end())
        return found->second;

    auto value =
        std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    value->set_level(spdlog::level::trace);
    value->flush_on(spdlog::level::warn);
    spdlog::register_or_replace(value);
    loggers.emplace(std::move(name), value);
    return value;
}

} // namespace vkr::log