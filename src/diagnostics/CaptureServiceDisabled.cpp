#include "CaptureService.h"

#include <stdexcept>

namespace vkr {

class CaptureService::Impl {};

CaptureService::CaptureService(Device &, std::filesystem::path) {}

CaptureService::~CaptureService() = default;

uint64_t CaptureService::request(std::filesystem::path, bool) {
    throw std::runtime_error("capture support was not compiled");
}

bool CaptureService::cancel(uint64_t) { return false; }

std::optional<CaptureTaskSnapshot> CaptureService::task(uint64_t) const {
    return std::nullopt;
}

std::vector<CaptureTaskSnapshot> CaptureService::tasks() const { return {}; }

std::optional<CaptureFrameSelection>
CaptureService::prepareFrame(const SwapChain &) {
    return std::nullopt;
}

void CaptureService::recordCopy(VkCommandBuffer, VkImage) {}

void CaptureService::frameSubmitted(uint64_t) {}

void CaptureService::update(uint64_t) {}

void CaptureService::onSwapChainRecreated(uint64_t) {}

void CaptureService::shutdown(uint64_t) {}

const std::filesystem::path &CaptureService::captureRoot() const {
    static const std::filesystem::path empty;
    return empty;
}

bool CaptureService::acceptingRequests() const { return false; }

} // namespace vkr
