#pragma once

#include <filesystem>
#include <optional>

namespace vkr {

std::optional<std::filesystem::path>
openGltfFileDialog(void *ownerWindow = nullptr);

} // namespace vkr
