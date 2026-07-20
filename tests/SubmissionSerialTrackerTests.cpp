#include "diagnostics/SubmissionSerialTracker.h"

#include <stdexcept>

namespace {

void requireSerial(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Exception, typename Callback>
void requireSerialThrows(Callback callback, const char *message) {
    try {
        callback();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error(message);
}

void testFrameSlotReuse() {
    vkr::SubmissionSerialTracker tracker(2);
    requireSerial(tracker.completedSerial() == 0 &&
                      tracker.lastSubmittedSerial() == 0,
                  "submission serial tracker did not start at zero");

    const uint64_t first = tracker.recordSubmission(0);
    const uint64_t second = tracker.recordSubmission(1);
    requireSerial(first == 1 && second == 2,
                  "submission serials are not monotonic");

    tracker.completeFrameSlot(0);
    requireSerial(tracker.completedSerial() == first,
                  "completed serial did not advance after a fence wait");

    const uint64_t third = tracker.recordSubmission(0);
    requireSerial(third == 3 && tracker.frameSlotSerial(0) == third,
                  "completed frame slot could not be reused");

    tracker.completeFrameSlot(1);
    requireSerial(tracker.completedSerial() == second,
                  "outstanding second slot was not completed in order");
    tracker.completeFrameSlot(0);
    requireSerial(tracker.completedSerial() == third,
                  "reused frame slot did not advance completion");
}

void testInvalidReuseAndIdleCompletion() {
    vkr::SubmissionSerialTracker tracker(2);
    tracker.recordSubmission(0);
    requireSerialThrows<std::logic_error>(
        [&tracker]() { tracker.recordSubmission(0); },
        "an in-flight frame slot was reused");

    tracker.recordSubmission(1);
    tracker.completeAll();
    requireSerial(tracker.completedSerial() == 2,
                  "known device-idle completion did not include all submits");

    requireSerialThrows<std::out_of_range>(
        [&tracker]() { tracker.completeFrameSlot(2); },
        "an invalid frame slot was accepted");
    requireSerialThrows<std::invalid_argument>(
        []() { vkr::SubmissionSerialTracker invalid(0); },
        "a zero-slot tracker was accepted");
}

} // namespace

void runSubmissionSerialTrackerTests() {
    testFrameSlotReuse();
    testInvalidReuseAndIdleCompletion();
}
