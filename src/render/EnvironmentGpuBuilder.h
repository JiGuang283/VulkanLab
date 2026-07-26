#pragma once

#include "EnvironmentGpuResources.h"

#include <chrono>
#include <cstdint>
#include <memory>

namespace vkr {

class Device;
class IncrementalUploadQueue;
struct EnvironmentLoadTask;
struct PreparedEnvironmentData;

class EnvironmentGpuBuilder {
  public:
    struct Budget {
        uint64_t maxUploadBytes = 32ull * 1024ull * 1024ull;
        double maxRecordMs = 2.0;
    };

    EnvironmentGpuBuilder(
        Device &device, std::shared_ptr<EnvironmentLoadTask> task,
        std::unique_ptr<PreparedEnvironmentData> prepared);
    ~EnvironmentGpuBuilder();

    void pump(const Budget &budget = {});
    void cancel();
    bool ready() const;
    bool finished() const;
    std::shared_ptr<EnvironmentGpuResources> takeResources();
    std::shared_ptr<EnvironmentLoadTask> task() const { return task_; }

  private:
    enum class Phase {
        Images,
        WaitingForGpu,
        Ready,
        Cancelling,
        Cancelled,
        Failed,
    };

    void submitRecorded();
    void fail(const std::exception &error);

    Device *device_ = nullptr;
    std::shared_ptr<EnvironmentLoadTask> task_;
    std::unique_ptr<PreparedEnvironmentData> prepared_;
    std::unique_ptr<IncrementalUploadQueue> uploadQueue_;
    std::shared_ptr<EnvironmentGpuResources> resources_;
    uint32_t imageIndex_ = 0;
    Phase phase_ = Phase::Images;
    bool failurePending_ = false;
};

} // namespace vkr
