#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vkr {

std::string sha256Bytes(const uint8_t *data, size_t size);
std::string sha256Bytes(const std::vector<uint8_t> &bytes);
std::string sha256String(const std::string &text);
std::string sha256File(const std::filesystem::path &path);

} // namespace vkr
