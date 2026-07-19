#include "diagnostics/BuildInfo.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace {

void requireBuildInfo(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testGeneratedBuildInfo() {
    const vkr::BuildInfo &info = vkr::currentBuildInfo();
    requireBuildInfo(info.configuration == "Debug" ||
                         info.configuration == "Release" ||
                         info.configuration == "RelWithDebInfo" ||
                         info.configuration == "MinSizeRel",
                     "build configuration metadata is invalid");
    requireBuildInfo(!info.compiler.empty() && info.compiler != "unknown",
                     "compiler metadata is missing");
    requireBuildInfo(!info.vulkanSdk.empty(),
                     "Vulkan SDK metadata is missing");
    requireBuildInfo(!info.glslc.empty(), "glslc metadata is missing");
    if (info.revision != "unknown") {
        requireBuildInfo(info.revision.size() == 40 &&
                             std::all_of(info.revision.begin(),
                                         info.revision.end(), [](char value) {
                                             return std::isxdigit(
                                                        static_cast<unsigned char>(
                                                            value)) != 0;
                                         }),
                         "Git revision metadata is invalid");
    }
}

} // namespace

void runBuildInfoTests() { testGeneratedBuildInfo(); }
