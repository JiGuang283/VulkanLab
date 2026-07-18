#pragma once

#include <cstdint>

namespace vkr::control {

inline constexpr wchar_t kPipeName[] = LR"(\\.\pipe\VulkanLab)";
inline constexpr char kPipeNameUtf8[] = R"(\\.\pipe\VulkanLab)";
inline constexpr uint32_t kProtocolVersion = 2;
inline constexpr uint32_t kMaxMessageBytes = 64u * 1024u;

} // namespace vkr::control
