#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace vkr {

struct AllocatorMemorySnapshot {
    uint64_t allocationCount = 0;
    uint64_t allocationBytes = 0;
    uint64_t blockBytes = 0;
};

struct ResourceLoadStats {
    double textureResizeMs = 0.0;
    double textureUploadMs = 0.0;
    double meshUploadMs = 0.0;
    double derivedTextureReadMs = 0.0;
    double derivedTextureTranscodeMs = 0.0;
    double batchSubmitWaitMs = 0.0;
    double maxUploadPumpMs = 0.0;

    uint64_t textureDecodeCount = 0;
    uint64_t gpuTextureCount = 0;
    uint64_t resizedTextureCount = 0;
    uint64_t derivedTextureLookups = 0;
    uint64_t derivedTextureHits = 0;
    uint64_t derivedTextureMisses = 0;
    uint64_t derivedTextureInvalid = 0;
    uint64_t derivedTextureReadBytes = 0;
    uint64_t bc7TextureCount = 0;
    uint64_t rgbaTranscodeFallbackCount = 0;
    uint64_t prebuiltMipTextureCount = 0;
    uint64_t gpuMeshCount = 0;
    uint64_t vertexCount = 0;
    uint64_t indexCount = 0;

    uint64_t encodedSourceBytes = 0;
    uint64_t decodedRgbaBytes = 0;
    uint64_t textureUploadBytes = 0;
    uint64_t textureGpuBytesEstimated = 0;
    uint64_t vertexUploadBytes = 0;
    uint64_t indexUploadBytes = 0;

    uint64_t singleTimeSubmits = 0;
    uint64_t queueWaitIdleCalls = 0;
    uint64_t batchSubmits = 0;
    uint64_t completedBatchSubmits = 0;
    uint64_t fenceWaitCalls = 0;
    uint64_t fencePollCalls = 0;
    uint64_t peakInFlightBatches = 0;
    uint64_t peakStagingBytes = 0;
    uint64_t uploadPumpCalls = 0;
    uint64_t maxUploadBytesPerPump = 0;
};

struct SceneLoadStats {
    uint64_t    taskId = 0;
    uint64_t    generation = 0;
    std::string sceneName;
    std::string finalState;
    uint32_t    maxTextureSize = 0;
    bool        success = false;

    double totalMs = 0.0;
    double deviceIdleMs = 0.0;
    double teardownMs = 0.0;
    double sceneFactoryMs = 0.0;
    double gltfParseMs = 0.0;
    double textureFileReadMs = 0.0;
    double textureDecodeMs = 0.0;
    double materialSetupMs = 0.0;
    double meshCpuMs = 0.0;
    double hierarchyMs = 0.0;
    double workerQueueWaitMs = 0.0;
    double cpuPrepareMs = 0.0;
    double gpuBuildMs = 0.0;
    double timeToFirstUploadMs = 0.0;

    uint64_t deviceWaitIdleCalls = 0;
    uint64_t materialCount = 0;
    uint64_t objectCount = 0;
    uint64_t gltfLightDefinitionCount = 0;
    uint64_t lightInstanceCount = 0;
    uint64_t directionalLightCount = 0;
    uint64_t pointLightCount = 0;
    uint64_t spotLightCount = 0;
    uint64_t preparedCpuBytes = 0;

    AllocatorMemorySnapshot allocatorBefore;
    AllocatorMemorySnapshot allocatorAfter;
    ResourceLoadStats       resources;
};

inline int64_t memoryDelta(uint64_t after, uint64_t before) {
    return after >= before ? static_cast<int64_t>(after - before)
                           : -static_cast<int64_t>(before - after);
}

class ScopedLoadTimer {
  public:
    explicit ScopedLoadTimer(double *accumulator)
        : accumulator_(accumulator), start_(Clock::now()) {}

    ~ScopedLoadTimer() {
        if (accumulator_) {
            *accumulator_ +=
                std::chrono::duration<double, std::milli>(Clock::now() - start_)
                    .count();
        }
    }

    ScopedLoadTimer(const ScopedLoadTimer &) = delete;
    ScopedLoadTimer &operator=(const ScopedLoadTimer &) = delete;

  private:
    using Clock = std::chrono::steady_clock;

    double           *accumulator_ = nullptr;
    Clock::time_point start_;
};

} // namespace vkr
