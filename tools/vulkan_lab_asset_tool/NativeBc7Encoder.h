#pragma once

#include "assets/DerivedTextureManifest.h"

#include <atomic>
#include <filesystem>

namespace vkr::assettool {

void encodeNativeBc7Ktx2(const std::filesystem::path &rgbaKtx2,
                         const std::filesystem::path &outputKtx2,
                         TextureSemantic semantic, bool exhaustive,
                         const std::atomic_bool &cancelRequested);

} // namespace vkr::assettool
