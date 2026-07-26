#pragma once

#include <filesystem>
#include <optional>

namespace vkr {

std::optional<std::filesystem::path>
openGltfFileDialog(void *ownerWindow = nullptr);
std::optional<std::filesystem::path>
openHdrFileDialog(void *ownerWindow = nullptr);

} // namespace vkr
