#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vkr {

inline constexpr uint64_t kCaptureTaskIdBase = uint64_t{1} << 62;
inline constexpr uint64_t kMaxCaptureBytes = uint64_t{256} * 1024 * 1024;

enum class CaptureTaskState {
    Queued,
    Recording,
    WaitingForGpu,
    Encoding,
    Completed,
    Failed,
    Cancelling,
    Cancelled,
};

enum class CaptureSourceKind {
    Viewport,
    Workspace,
    Hdr,
};

struct CaptureImageSource {
    CaptureSourceKind kind = CaptureSourceKind::Viewport;
    VkImage image = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags sourceStage = 0;
    VkAccessFlags sourceAccess = 0;
    VkPipelineStageFlags restoreStage = 0;
    VkAccessFlags restoreAccess = 0;
    bool supported = false;
    std::string unsupportedReason;
};

struct CaptureTimings {
    double recordingMs = 0.0;
    double gpuWaitMs = 0.0;
    double cpuCopyMs = 0.0;
    double encodeMs = 0.0;
    double totalMs = 0.0;
};

struct CaptureRequest {
    uint64_t taskId = 0;
    std::filesystem::path relativeOutputPath;
    bool includeGui = false;
    CaptureSourceKind source = CaptureSourceKind::Viewport;
};

struct CaptureResult {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    CaptureSourceKind source = CaptureSourceKind::Viewport;
    uint64_t frameSerial = 0;
    std::filesystem::path outputPath;
    std::string sha256;
    CaptureTimings timings;
    std::string error;
};

struct CaptureTaskSnapshot {
    CaptureRequest request;
    CaptureTaskState state = CaptureTaskState::Queued;
    CaptureResult result;
};

enum class CaptureChannelOrder { Rgba, Bgra };
enum class CapturePixelEncoding { Unorm8, Float16, Float32 };

struct CaptureFormatDescription {
    bool supported = false;
    CaptureChannelOrder channelOrder = CaptureChannelOrder::Rgba;
    CapturePixelEncoding encoding = CapturePixelEncoding::Unorm8;
    uint32_t bytesPerPixel = 0;
    const char *name = "Unsupported";
};

const char *captureTaskStateName(CaptureTaskState state);
const char *captureSourceKindName(CaptureSourceKind source);
bool isTerminalCaptureTaskState(CaptureTaskState state);
bool isValidCaptureTaskTransition(CaptureTaskState from,
                                  CaptureTaskState to);

CaptureFormatDescription describeCaptureFormat(VkFormat format);
uint64_t checkedCaptureByteSize(uint32_t width, uint32_t height,
                                uint64_t maximumBytes = kMaxCaptureBytes,
                                uint32_t bytesPerPixel = 4);
std::vector<uint8_t>
convertCapturePixelsToRgba(const uint8_t *pixels, size_t byteCount,
                           uint32_t width, uint32_t height, VkFormat format);
std::vector<float>
convertCapturePixelsToRgbFloat(const uint8_t *pixels, size_t byteCount,
                               uint32_t width, uint32_t height,
                               VkFormat format);

std::filesystem::path resolveCaptureOutputPath(
    const std::filesystem::path &captureRoot,
    const std::filesystem::path &relativeOutputPath,
    CaptureSourceKind source = CaptureSourceKind::Viewport);

} // namespace vkr
