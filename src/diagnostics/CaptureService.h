#pragma once

#include "CaptureTypes.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class SwapChain;

struct CaptureFrameSelection {
    uint64_t taskId = 0;
    bool includeGui = false;
};

class CaptureService {
  public:
    CaptureService(Device &device, std::filesystem::path captureRoot);
    ~CaptureService();

    CaptureService(const CaptureService &) = delete;
    CaptureService &operator=(const CaptureService &) = delete;

    uint64_t request(std::filesystem::path relativeOutputPath = {},
                     bool includeGui = false);
    bool cancel(uint64_t taskId);

    std::optional<CaptureTaskSnapshot> task(uint64_t taskId) const;
    std::vector<CaptureTaskSnapshot> tasks() const;

    std::optional<CaptureFrameSelection>
    prepareFrame(const SwapChain &swapChain);
    void recordCopy(VkCommandBuffer commandBuffer, VkImage swapchainImage);
    void frameSubmitted(uint64_t submissionSerial);
    void update(uint64_t completedSubmissionSerial);

    // Both methods require the caller to have completed all submitted GPU
    // work and to pass the corresponding completed submission serial.
    void onSwapChainRecreated(uint64_t completedSubmissionSerial);
    void shutdown(uint64_t completedSubmissionSerial);

    const std::filesystem::path &captureRoot() const;
    bool acceptingRequests() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vkr
