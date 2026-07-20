#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace vkr::control {

inline constexpr wchar_t kPipeName[] = LR"(\\.\pipe\VulkanLab)";
inline constexpr char kPipeNameUtf8[] = R"(\\.\pipe\VulkanLab)";
inline constexpr uint32_t kProtocolVersion = 3;
inline constexpr uint32_t kMaxMessageBytes = 64u * 1024u;
inline constexpr size_t kMaxPipeSuffixLength = 64;

struct RuntimeControlEndpoint {
    std::string suffix;
    std::string nameUtf8;
    std::wstring name;
};

bool isValidRuntimePipeSuffix(std::string_view suffix);
RuntimeControlEndpoint
makeRuntimeControlEndpoint(std::string_view suffix = {});

} // namespace vkr::control
