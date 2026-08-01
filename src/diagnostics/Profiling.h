#pragma once

#include <BuildFeatures.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#if VKL_ENABLE_TRACY
#include <tracy/Tracy.hpp>
#include <common/TracySystem.hpp>
#endif

namespace vkr {

class TracyProfiler;

class ManualProfileZone {
  public:
    ManualProfileZone(uint32_t line, const char *source,
                      const char *function, std::string_view name) {
#if VKL_ENABLE_TRACY
        zone_.emplace(line, source, std::strlen(source), function,
                      std::strlen(function), name.data(), name.size(), 0, -1,
                      true);
#else
        (void)line;
        (void)source;
        (void)function;
        (void)name;
#endif
    }

    void finish() {
#if VKL_ENABLE_TRACY
        zone_.reset();
#endif
    }

  private:
#if VKL_ENABLE_TRACY
    std::optional<tracy::ScopedZone> zone_;
#endif
};

inline void profileSetThreadName(const char *name) {
#if VKL_ENABLE_TRACY
    tracy::SetThreadName(name);
#else
    (void)name;
#endif
}

inline bool profileConnected() {
#if VKL_ENABLE_TRACY
    return TracyIsConnected;
#else
    return false;
#endif
}

inline void profileFrameMark() {
#if VKL_ENABLE_TRACY
    FrameMark;
#endif
}

inline void profilePlotNumber(const char *name, double value) {
#if VKL_ENABLE_TRACY
    TracyPlot(name, value);
#else
    (void)name;
    (void)value;
#endif
}

inline void profilePlotNumber(const char *name, int64_t value) {
#if VKL_ENABLE_TRACY
    TracyPlot(name, value);
#else
    (void)name;
    (void)value;
#endif
}

inline void profileConfigureMemoryPlot(const char *name) {
#if VKL_ENABLE_TRACY
    TracyPlotConfig(name, tracy::PlotFormatType::Memory, false, true, 0);
#else
    (void)name;
#endif
}

inline void profilePlotMemory(const char *name, int64_t value) {
#if VKL_ENABLE_TRACY
    TracyPlot(name, value);
#else
    (void)name;
    (void)value;
#endif
}

} // namespace vkr

#if VKL_ENABLE_TRACY
#define VKL_PROFILE_ZONE(name) ZoneScopedN(name)
#define VKL_PROFILE_TEXT(text) ZoneText((text).data(), (text).size())
#else
#define VKL_PROFILE_ZONE(name) ((void)0)
#define VKL_PROFILE_TEXT(text) ((void)0)
#endif

#define VKL_PROFILE_BEGIN(var, name)                                        \
    vkr::ManualProfileZone var(__LINE__, __FILE__, __func__, (name))
#define VKL_PROFILE_END(var) (var).finish()
