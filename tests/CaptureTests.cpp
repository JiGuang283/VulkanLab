#include "diagnostics/CaptureTaskQueue.h"
#include "diagnostics/CaptureTypes.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace {

void requireCapture(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Exception, typename Callback>
void requireCaptureThrows(Callback callback, const char *message) {
    try {
        callback();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error(message);
}

void completeNext(vkr::CaptureTaskQueue &queue) {
    const auto taskId = queue.beginNext();
    requireCapture(taskId.has_value(), "capture queue did not start a task");
    queue.transition(*taskId, vkr::CaptureTaskState::WaitingForGpu);
    queue.transition(*taskId, vkr::CaptureTaskState::Encoding);
    queue.transition(*taskId, vkr::CaptureTaskState::Completed);
}

void testCaptureStateTransitions() {
    using vkr::CaptureTaskState;
    requireCapture(vkr::isValidCaptureTaskTransition(
                       CaptureTaskState::Queued,
                       CaptureTaskState::Recording),
                   "queued capture could not begin recording");
    requireCapture(vkr::isValidCaptureTaskTransition(
                       CaptureTaskState::WaitingForGpu,
                       CaptureTaskState::Encoding),
                   "GPU-complete capture could not begin encoding");
    requireCapture(vkr::isValidCaptureTaskTransition(
                       CaptureTaskState::Cancelling,
                       CaptureTaskState::Cancelled),
                   "capture cancellation could not finish");
    requireCapture(!vkr::isValidCaptureTaskTransition(
                       CaptureTaskState::Queued,
                       CaptureTaskState::Completed),
                   "capture skipped required states");
    requireCapture(vkr::isTerminalCaptureTaskState(
                       CaptureTaskState::Completed) &&
                       vkr::isTerminalCaptureTaskState(
                           CaptureTaskState::Failed) &&
                       vkr::isTerminalCaptureTaskState(
                           CaptureTaskState::Cancelled),
                   "capture terminal-state classification changed");
}

void testCaptureQueueBoundsAndHistory() {
    vkr::CaptureTaskQueue bounded(2, 2);
    const uint64_t first = bounded.enqueue("first.png", false);
    const uint64_t second = bounded.enqueue("second.png", true);
    requireCapture(first == vkr::kCaptureTaskIdBase && second == first + 1,
                   "capture task IDs are not monotonic in their namespace");
    requireCaptureThrows<std::length_error>(
        [&bounded] { bounded.enqueue("full.png", false); },
        "capture queue accepted more than its active-task limit");

    const auto recording = bounded.beginNext();
    requireCapture(recording == first,
                   "capture queue did not preserve FIFO order");
    requireCapture(bounded.cancel(first),
                   "recording capture could not be cancelled");
    requireCapture(bounded.find(first)->state ==
                       vkr::CaptureTaskState::Cancelling,
                   "recording capture cancellation completed before GPU work");
    bounded.transition(first, vkr::CaptureTaskState::Cancelled);
    requireCapture(bounded.cancel(second),
                   "queued capture could not be cancelled");
    requireCapture(bounded.activeCount() == 0,
                   "terminal captures remained active");

    vkr::CaptureTaskQueue history(1, 2);
    const uint64_t oldest = history.enqueue("one.png", false);
    completeNext(history);
    history.enqueue("two.png", false);
    completeNext(history);
    history.enqueue("three.png", false);
    completeNext(history);
    requireCapture(history.terminalCount() == 2 &&
                       history.snapshots().size() == 2,
                   "capture terminal history was not bounded");
    requireCapture(history.find(oldest) == nullptr,
                   "oldest capture history entry was not pruned");
}

void testCapturePathValidation() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "vulkan-lab-captures";
    const std::filesystem::path output =
        vkr::resolveCaptureOutputPath(root, "suite/frame.PNG");
    requireCapture(output ==
                       (std::filesystem::absolute(root) / "suite/frame.PNG")
                           .lexically_normal(),
                   "valid capture output path resolved incorrectly");

    requireCaptureThrows<std::invalid_argument>(
        [&root] { vkr::resolveCaptureOutputPath(root, "../escape.png"); },
        "capture path traversal was accepted");
    requireCaptureThrows<std::invalid_argument>(
        [&root] {
            vkr::resolveCaptureOutputPath(
                root, std::filesystem::absolute(root / "absolute.png"));
        },
        "absolute capture path was accepted");
    requireCaptureThrows<std::invalid_argument>(
        [&root] { vkr::resolveCaptureOutputPath(root, "frame.jpg"); },
        "non-PNG capture output was accepted");
    requireCaptureThrows<std::invalid_argument>(
        [&root] { vkr::resolveCaptureOutputPath(root, {}); },
        "empty capture output was accepted");
}

void testCaptureSizeAndFormatValidation() {
    requireCapture(vkr::checkedCaptureByteSize(800, 600) == 1'920'000,
                   "capture byte count is wrong");
    requireCaptureThrows<std::invalid_argument>(
        [] { vkr::checkedCaptureByteSize(0, 1); },
        "zero-width capture was accepted");
    requireCaptureThrows<std::length_error>(
        [] { vkr::checkedCaptureByteSize(2, 2, 15); },
        "capture byte limit was not enforced");
    requireCaptureThrows<std::overflow_error>(
        [] {
            vkr::checkedCaptureByteSize(UINT32_MAX, UINT32_MAX,
                                        UINT64_MAX);
        },
        "capture extent overflow was not rejected");

    requireCapture(
        vkr::describeCaptureFormat(VK_FORMAT_R8G8B8A8_SRGB).supported &&
            vkr::describeCaptureFormat(VK_FORMAT_B8G8R8A8_UNORM).supported,
        "supported capture formats were rejected");
    requireCapture(
        !vkr::describeCaptureFormat(VK_FORMAT_R16G16B16A16_SFLOAT)
             .supported,
        "unsupported capture format was accepted");
}

void testCapturePixelConversionAndOrientation() {
    const std::array<uint8_t, 16> bgra = {
        30,  20,  10,  40,  70,  60,  50,  80,
        110, 100, 90,  120, 150, 140, 130, 160,
    };
    const std::vector<uint8_t> rgba = vkr::convertCapturePixelsToRgba(
        bgra.data(), bgra.size(), 2, 2, VK_FORMAT_B8G8R8A8_UNORM);
    const std::vector<uint8_t> expected = {
        10, 20, 30, 40, 50, 60, 70, 80,
        90, 100, 110, 120, 130, 140, 150, 160,
    };
    requireCapture(rgba == expected,
                   "BGRA conversion changed channels, alpha, or row order");

    const std::vector<uint8_t> unchanged = vkr::convertCapturePixelsToRgba(
        expected.data(), expected.size(), 2, 2, VK_FORMAT_R8G8B8A8_SRGB);
    requireCapture(unchanged == expected,
                   "RGBA conversion unexpectedly changed the payload");
    requireCaptureThrows<std::invalid_argument>(
        [&expected] {
            vkr::convertCapturePixelsToRgba(
                expected.data(), expected.size() - 1, 2, 2,
                VK_FORMAT_R8G8B8A8_UNORM);
        },
        "capture conversion accepted the wrong byte count");
}

} // namespace

void runCaptureTests() {
    testCaptureStateTransitions();
    testCaptureQueueBoundsAndHistory();
    testCapturePathValidation();
    testCaptureSizeAndFormatValidation();
    testCapturePixelConversionAndOrientation();
}
