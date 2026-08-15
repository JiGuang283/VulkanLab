#pragma once

#include <stdexcept>
#include <string_view>

namespace vkr {

enum class MaterialBindingMode { Auto, Legacy, Bindless };

inline const char *materialBindingModeName(MaterialBindingMode mode) {
    switch (mode) {
    case MaterialBindingMode::Auto:
        return "auto";
    case MaterialBindingMode::Legacy:
        return "legacy";
    case MaterialBindingMode::Bindless:
        return "bindless";
    }
    return "unknown";
}

inline MaterialBindingMode parseMaterialBindingMode(std::string_view value) {
    if (value == "auto")
        return MaterialBindingMode::Auto;
    if (value == "legacy")
        return MaterialBindingMode::Legacy;
    if (value == "bindless")
        return MaterialBindingMode::Bindless;
    throw std::invalid_argument(
        "material binding mode must be auto, legacy, or bindless");
}

} // namespace vkr
