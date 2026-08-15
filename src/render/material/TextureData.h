#pragma once

#include <cstdint>
#include <vector>

namespace vkr {

bool limitedTextureExtent(uint32_t width, uint32_t height,
                          uint32_t maxExtent, uint32_t &outWidth,
                          uint32_t &outHeight);

std::vector<uint8_t> resizeRgba8Bilinear(const uint8_t *src,
                                         uint32_t srcWidth,
                                         uint32_t srcHeight,
                                         uint32_t dstWidth,
                                         uint32_t dstHeight);

uint64_t rgba8MipChainBytes(uint32_t width, uint32_t height,
                            uint32_t mipLevels);

} // namespace vkr
