#include "TextureData.h"

#include <algorithm>
#include <cmath>

namespace vkr {

namespace {

uint8_t sampleBilinearChannel(const uint8_t *src, uint32_t srcWidth,
                              uint32_t srcHeight, float x, float y,
                              uint32_t channel) {
    x = std::clamp(x, 0.0f, static_cast<float>(srcWidth - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(srcHeight - 1));

    const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
    const uint32_t x1 = std::min(x0 + 1, srcWidth - 1);
    const uint32_t y1 = std::min(y0 + 1, srcHeight - 1);
    const float    tx = x - static_cast<float>(x0);
    const float    ty = y - static_cast<float>(y0);

    const auto at = [&](uint32_t px, uint32_t py) {
        return static_cast<float>(src[(static_cast<size_t>(py) * srcWidth +
                                       px) *
                                          4 +
                                      channel]);
    };

    const float top = at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx;
    const float value = top * (1.0f - ty) + bottom * ty;
    return static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
}

} // namespace

std::vector<uint8_t> resizeRgba8Bilinear(const uint8_t *src,
                                         uint32_t srcWidth,
                                         uint32_t srcHeight,
                                         uint32_t dstWidth,
                                         uint32_t dstHeight) {
    std::vector<uint8_t> dst(static_cast<size_t>(dstWidth) * dstHeight * 4);
    const float scaleX = static_cast<float>(srcWidth) / dstWidth;
    const float scaleY = static_cast<float>(srcHeight) / dstHeight;

    for (uint32_t y = 0; y < dstHeight; ++y) {
        const float srcY = (static_cast<float>(y) + 0.5f) * scaleY - 0.5f;
        for (uint32_t x = 0; x < dstWidth; ++x) {
            const float srcX =
                (static_cast<float>(x) + 0.5f) * scaleX - 0.5f;
            const size_t dstOffset =
                (static_cast<size_t>(y) * dstWidth + x) * 4;
            for (uint32_t c = 0; c < 4; ++c) {
                dst[dstOffset + c] = sampleBilinearChannel(
                    src, srcWidth, srcHeight, srcX, srcY, c);
            }
        }
    }

    return dst;
}

bool limitedTextureExtent(uint32_t width, uint32_t height,
                          uint32_t maxExtent, uint32_t &outWidth,
                          uint32_t &outHeight) {
    const uint32_t maxSourceExtent = std::max(width, height);
    if (maxExtent == 0 || maxSourceExtent <= maxExtent) {
        outWidth = width;
        outHeight = height;
        return false;
    }

    const float scale = static_cast<float>(maxExtent) /
                        static_cast<float>(maxSourceExtent);
    outWidth = std::max(
        1u, static_cast<uint32_t>(
                std::lround(static_cast<float>(width) * scale)));
    outHeight = std::max(
        1u, static_cast<uint32_t>(
                std::lround(static_cast<float>(height) * scale)));
    return outWidth != width || outHeight != height;
}

uint64_t rgba8MipChainBytes(uint32_t width, uint32_t height,
                            uint32_t mipLevels) {
    uint64_t total = 0;
    for (uint32_t level = 0; level < mipLevels; ++level) {
        total += static_cast<uint64_t>(width) * height * 4;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    return total;
}

} // namespace vkr
