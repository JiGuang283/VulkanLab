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
        return {true, CaptureChannelOrder::Rgba, "R8G8B8A8_UNORM"};
    case VK_FORMAT_R8G8B8A8_SRGB:
        return {true, CaptureChannelOrder::Rgba, "R8G8B8A8_SRGB"};
    case VK_FORMAT_B8G8R8A8_UNORM:
        return {true, CaptureChannelOrder::Bgra, "B8G8R8A8_UNORM"};
    case VK_FORMAT_B8G8R8A8_SRGB:
        return {true, CaptureChannelOrder::Bgra, "B8G8R8A8_SRGB"};
    default:
        return {};
    }
}

uint64_t checkedCaptureByteSize(uint32_t width, uint32_t height,
                                uint64_t maximumBytes) {
    if (width == 0 || height == 0)
        throw std::invalid_argument("capture extent must be non-zero");
    if (maximumBytes == 0)
        throw std::invalid_argument("capture byte limit must be non-zero");

    constexpr uint64_t bytesPerPixel = 4;
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
    const std::filesystem::path &relativeOutputPath) {
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
    if (extension != ".png")
        throw std::invalid_argument("capture output must use a .png extension");

    const std::filesystem::path root =
        std::filesystem::absolute(captureRoot).lexically_normal();
    const std::filesystem::path output = (root / relative).lexically_normal();
    if (!pathIsWithin(root, output))
        throw std::invalid_argument(
            "capture output path escapes the capture root");
    return output;
}

} // namespace vkr
