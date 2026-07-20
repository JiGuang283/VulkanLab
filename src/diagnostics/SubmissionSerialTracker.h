#pragma once

#include <cstdint>
#include <vector>

namespace vkr {

class SubmissionSerialTracker {
  public:
    explicit SubmissionSerialTracker(uint32_t frameSlotCount);

    uint64_t recordSubmission(uint32_t frameSlot);
    void completeFrameSlot(uint32_t frameSlot);
    void completeAll();

    uint64_t completedSerial() const { return completedSerial_; }
    uint64_t lastSubmittedSerial() const { return nextSerial_ - 1; }
    uint64_t frameSlotSerial(uint32_t frameSlot) const;

  private:
    void validateFrameSlot(uint32_t frameSlot) const;

    std::vector<uint64_t> frameSlotSerials_;
    uint64_t nextSerial_ = 1;
    uint64_t completedSerial_ = 0;
};

} // namespace vkr
