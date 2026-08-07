#include "CaptureTypes.h"

#include "assets/SceneCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace vkr {

const char *captureTaskStateName(CaptureTaskState state) {
    switch (state) {
    case CaptureTaskState::Queued:
        return "Queued";
    case CaptureTaskState::Recording:
        return "Recording";
    case CaptureTaskState::WaitingForGpu:
        return "WaitingForGpu";
    case CaptureTaskState::Encoding:
        return "Encoding";
    case CaptureTaskState::Completed:
        return "Completed";
    case CaptureTaskState::Failed:
        return "Failed";
    case CaptureTaskState::Cancelling:
        return "Cancelling";
    case CaptureTaskState::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

const char *captureSourceKindName(CaptureSourceKind source) {
    switch (source) {
    case CaptureSourceKind::Viewport:
        return "Viewport";
    case CaptureSourceKind::Workspace:
        return "Workspace";
    case CaptureSourceKind::Hdr:
        return "HDR";
    }
    return "Unknown";
}

bool isTerminalCaptureTaskState(CaptureTaskState state) {
    return state == CaptureTaskState::Completed ||
           state == CaptureTaskState::Failed ||
           state == CaptureTaskState::Cancelled;
}

bool isValidCaptureTaskTransition(CaptureTaskState from,
                                  CaptureTaskState to) {
    switch (from) {
    case CaptureTaskState::Queued:
        return to == CaptureTaskState::Recording ||
               to == CaptureTaskState::Failed ||
               to == CaptureTaskState::Cancelled;
    case CaptureTaskState::Recording:
        return to == CaptureTaskState::WaitingForGpu ||
               to == CaptureTaskState::Cancelling ||
               to == CaptureTaskState::Failed;
    case CaptureTaskState::WaitingForGpu:
        return to == CaptureTaskState::Encoding ||
               to == CaptureTaskState::Cancelling ||
               to == CaptureTaskState::Failed;
    case CaptureTaskState::Encoding:
        return to == CaptureTaskState::Completed ||
               to == CaptureTaskState::Cancelling ||
               to == CaptureTaskState::Failed;
    case CaptureTaskState::Cancelling:
        return to == CaptureTaskState::Cancelled ||
               to == CaptureTaskState::Failed;
    case CaptureTaskState::Completed:
    case CaptureTaskState::Failed:
    case CaptureTaskState::Cancelled:
        return false;
    }
    return false;
}

CaptureFormatDescription describeCaptureFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        return {true, CaptureChannelOrder::Rgba,
                CapturePixelEncoding::Unorm8, 4, "R8G8B8A8_UNORM"};
    case VK_FORMAT_R8G8B8A8_SRGB:
        return {true, CaptureChannelOrder::Rgba,
                CapturePixelEncoding::Unorm8, 4, "R8G8B8A8_SRGB"};
    case VK_FORMAT_B8G8R8A8_UNORM:
        return {true, CaptureChannelOrder::Bgra,
                CapturePixelEncoding::Unorm8, 4, "B8G8R8A8_UNORM"};
    case VK_FORMAT_B8G8R8A8_SRGB:
        return {true, CaptureChannelOrder::Bgra,
                CapturePixelEncoding::Unorm8, 4, "B8G8R8A8_SRGB"};
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return {true, CaptureChannelOrder::Rgba,
                CapturePixelEncoding::Float16, 8,
                "R16G16B16A16_SFLOAT"};
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return {true, CaptureChannelOrder::Rgba,
                CapturePixelEncoding::Float32, 16,
                "R32G32B32A32_SFLOAT"};
    default:
        return {};
    }
}

uint64_t checkedCaptureByteSize(uint32_t width, uint32_t height,
                                uint64_t maximumBytes,
                                uint32_t bytesPerPixel) {
    if (width == 0 || height == 0)
        throw std::invalid_argument("capture extent must be non-zero");
    if (maximumBytes == 0)
        throw std::invalid_argument("capture byte limit must be non-zero");

    if (bytesPerPixel == 0)
        throw std::invalid_argument("capture bytes per pixel is zero");
    const uint64_t width64 = width;
    const uint64_t height64 = height;
    if (width64 > std::numeric_limits<uint64_t>::max() / height64 ||
        width64 * height64 >
            std::numeric_limits<uint64_t>::max() / bytesPerPixel) {
        throw std::overflow_error("capture byte size overflow");
    }

    const uint64_t bytes = width64 * height64 * bytesPerPixel;
    if (bytes > maximumBytes)
        throw std::length_error("capture exceeds the readback byte limit");
    if (bytes > std::numeric_limits<size_t>::max())
        throw std::length_error("capture does not fit in CPU address space");
    return bytes;
}

namespace {
float halfToFloat(uint16_t half) {
    const uint32_t sign = uint32_t(half & 0x8000u) << 16u;
    const uint32_t exponent = (half >> 10u) & 0x1fu;
    uint32_t mantissa = half & 0x03ffu;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int32_t unbiasedExponent = -14;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1u;
                --unbiasedExponent;
            }
            mantissa &= 0x03ffu;
            bits = sign |
                   (uint32_t(unbiasedExponent + 127) << 23u) |
                   (mantissa << 13u);
        }
    } else if (exponent == 31u) {
        bits = sign | 0x7f800000u | (mantissa << 13u);
    } else {
        bits = sign | ((exponent + 112u) << 23u) |
               (mantissa << 13u);
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
} // namespace

std::vector<float>
convertCapturePixelsToRgbFloat(const uint8_t *pixels, size_t byteCount,
                               uint32_t width, uint32_t height,
                               VkFormat format) {
    const CaptureFormatDescription description = describeCaptureFormat(format);
    if (!description.supported ||
        description.encoding == CapturePixelEncoding::Unorm8)
        throw std::invalid_argument("capture format is not floating point");
    const uint64_t expected = checkedCaptureByteSize(
        width, height, kMaxCaptureBytes, description.bytesPerPixel);
    if (!pixels || byteCount != expected)
        throw std::invalid_argument(
            "capture pixel payload does not match its extent");

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<float> rgb(pixelCount * 3u);
    if (description.encoding == CapturePixelEncoding::Float16) {
        for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
            uint16_t rgba[4]{};
            std::memcpy(rgba, pixels + pixel * 8u, sizeof(rgba));
            rgb[pixel * 3u + 0u] = halfToFloat(rgba[0]);
            rgb[pixel * 3u + 1u] = halfToFloat(rgba[1]);
            rgb[pixel * 3u + 2u] = halfToFloat(rgba[2]);
        }
    } else {
        for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
            float rgba[4]{};
            std::memcpy(rgba, pixels + pixel * 16u, sizeof(rgba));
            std::copy_n(rgba, 3, rgb.data() + pixel * 3u);
        }
    }
    return rgb;
}

std::vector<uint8_t>
convertCapturePixelsToRgba(const uint8_t *pixels, size_t byteCount,
                           uint32_t width, uint32_t height, VkFormat format) {
    const CaptureFormatDescription description = describeCaptureFormat(format);
    if (!description.supported)
        throw std::invalid_argument("unsupported capture pixel format");

    const uint64_t expected = checkedCaptureByteSize(width, height);
    if (!pixels || byteCount != expected)
        throw std::invalid_argument(
            "capture pixel payload does not match its extent");

    std::vector<uint8_t> rgba(static_cast<size_t>(expected));
    std::memcpy(rgba.data(), pixels, rgba.size());
    if (description.channelOrder == CaptureChannelOrder::Bgra) {
        for (size_t offset = 0; offset < rgba.size(); offset += 4)
            std::swap(rgba[offset], rgba[offset + 2]);
    }
    return rgba;
}

std::filesystem::path resolveCaptureOutputPath(
    const std::filesystem::path &captureRoot,
    const std::filesystem::path &relativeOutputPath,
    CaptureSourceKind source) {
    if (captureRoot.empty())
        throw std::invalid_argument("capture root is empty");
    if (relativeOutputPath.empty() || relativeOutputPath.is_absolute() ||
        relativeOutputPath.has_root_name() ||
        relativeOutputPath.has_root_directory()) {
        throw std::invalid_argument(
            "capture output must be a non-empty relative path");
    }

    const std::filesystem::path relative =
        relativeOutputPath.lexically_normal();
    if (relative.empty() || relative == ".")
        throw std::invalid_argument("capture output path is empty");
    for (const auto &component : relative) {
        if (component == "..")
            throw std::invalid_argument(
                "capture output path escapes the capture root");
    }

    std::string extension = relative.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    const std::string expectedExtension =
        source == CaptureSourceKind::Hdr ? ".hdr" : ".png";
    if (extension != expectedExtension)
        throw std::invalid_argument("capture output must use a " +
                                    expectedExtension + " extension");

    const std::filesystem::path root =
        std::filesystem::absolute(captureRoot).lexically_normal();
    const std::filesystem::path output = (root / relative).lexically_normal();
    if (!pathIsWithin(root, output))
        throw std::invalid_argument(
            "capture output path escapes the capture root");
    return output;
}

} // namespace vkr
