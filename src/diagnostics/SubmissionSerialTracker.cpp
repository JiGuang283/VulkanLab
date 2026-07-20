#include "SubmissionSerialTracker.h"

#include <algorithm>
#include <stdexcept>

namespace vkr {

SubmissionSerialTracker::SubmissionSerialTracker(uint32_t frameSlotCount)
    : frameSlotSerials_(frameSlotCount, 0) {
    if (frameSlotCount == 0)
        throw std::invalid_argument(
            "submission serial tracker requires at least one frame slot");
}

uint64_t SubmissionSerialTracker::recordSubmission(uint32_t frameSlot) {
    validateFrameSlot(frameSlot);
    if (frameSlotSerials_[frameSlot] > completedSerial_)
        throw std::logic_error(
            "frame slot reused before its previous submission completed");

    const uint64_t serial = nextSerial_++;
    frameSlotSerials_[frameSlot] = serial;
    return serial;
}

void SubmissionSerialTracker::completeFrameSlot(uint32_t frameSlot) {
    validateFrameSlot(frameSlot);
    completedSerial_ =
        std::max(completedSerial_, frameSlotSerials_[frameSlot]);
}

void SubmissionSerialTracker::completeAll() {
    completedSerial_ = std::max(completedSerial_, lastSubmittedSerial());
}

uint64_t
SubmissionSerialTracker::frameSlotSerial(uint32_t frameSlot) const {
    validateFrameSlot(frameSlot);
    return frameSlotSerials_[frameSlot];
}

void SubmissionSerialTracker::validateFrameSlot(uint32_t frameSlot) const {
    if (frameSlot >= frameSlotSerials_.size())
        throw std::out_of_range("submission frame slot is out of range");
}

} // namespace vkr
