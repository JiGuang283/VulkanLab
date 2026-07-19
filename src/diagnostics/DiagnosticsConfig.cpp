#include "DiagnosticsConfig.h"

#include <charconv>
#include <cmath>
#include <stdexcept>

namespace vkr {
namespace {

uint32_t parseDimension(std::string_view value, const char *name) {
    uint32_t result = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() ||
        result == 0 || result > 16384) {
        throw std::invalid_argument(std::string("invalid ") + name +
                                    " in --window-size; expected 1..16384");
    }
    return result;
}

} // namespace

DiagnosticWindowSize parseDiagnosticWindowSize(std::string_view value) {
    const size_t separator = value.find_first_of("xX");
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= value.size() ||
        value.find_first_of("xX", separator + 1) != std::string_view::npos) {
        throw std::invalid_argument(
            "--window-size must use the form WIDTHxHEIGHT");
    }

    return {parseDimension(value.substr(0, separator), "width"),
            parseDimension(value.substr(separator + 1), "height")};
}

float parseDiagnosticFixedDelta(std::string_view value) {
    float result = 0.0f;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() ||
        !std::isfinite(result) || result <= 0.0f || result > 1.0f) {
        throw std::invalid_argument(
            "--fixed-delta must be a finite value in (0, 1]");
    }
    return result;
}

} // namespace vkr
