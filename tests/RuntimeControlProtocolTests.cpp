#include "control/RuntimeControlProtocol.h"

#include <stdexcept>
#include <string>

namespace {

void requireProtocol(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Callback>
void requireInvalidProtocol(Callback callback, const char *message) {
    try {
        callback();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error(message);
}

void testDefaultEndpointCompatibility() {
    const auto endpoint = vkr::control::makeRuntimeControlEndpoint();
    requireProtocol(endpoint.suffix.empty(),
                    "default runtime endpoint gained a suffix");
    requireProtocol(endpoint.nameUtf8 == vkr::control::kPipeNameUtf8,
                    "default UTF-8 runtime pipe changed");
    requireProtocol(endpoint.name == vkr::control::kPipeName,
                    "default wide runtime pipe changed");
}

void testIsolatedEndpointSuffix() {
    const auto endpoint =
        vkr::control::makeRuntimeControlEndpoint("render_test-42_A");
    requireProtocol(endpoint.nameUtf8 ==
                        R"(\\.\pipe\VulkanLab.render_test-42_A)",
                    "isolated UTF-8 runtime endpoint is wrong");
    requireProtocol(endpoint.name ==
                        LR"(\\.\pipe\VulkanLab.render_test-42_A)",
                    "isolated wide runtime endpoint is wrong");

    requireProtocol(vkr::control::isValidRuntimePipeSuffix("A-z_0-9"),
                    "valid runtime pipe suffix was rejected");
    requireInvalidProtocol(
        [] { vkr::control::makeRuntimeControlEndpoint("bad.suffix"); },
        "runtime pipe suffix accepted punctuation");
    requireInvalidProtocol(
        [] { vkr::control::makeRuntimeControlEndpoint("bad/suffix"); },
        "runtime pipe suffix accepted a path separator");
    requireInvalidProtocol(
        [] {
            vkr::control::makeRuntimeControlEndpoint(
                std::string(vkr::control::kMaxPipeSuffixLength + 1, 'a'));
        },
        "runtime pipe suffix exceeded its length bound");
}

} // namespace

void runRuntimeControlProtocolTests() {
    testDefaultEndpointCompatibility();
    testIsolatedEndpointSuffix();
}
