#include "AssetRepository.h"

#include "PreparedModelData.h"
#include "SceneLoadTask.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/Log.h"
#include "core/PipelineConfigBuilder.h"
#include "diagnostics/Profiling.h"
#include "render/MaterialTemplate.h"
#include "render/MaterialSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace vkr {
namespace detail {

struct ModelAssetRecord {
    ModelAssetRequest request;
    uint64_t taskId = 0;
    uint64_t generation = 0;
    std::atomic<ModelAssetState> state{ModelAssetState::Queued};
    std::atomic<uint64_t> consumers{0};
    std::shared_ptr<std::atomic_bool> cancellation =
        std::make_shared<std::atomic_bool>(false);
    SceneLoadProgress progress;
    SceneLoadStats stats;
    std::chrono::steady_clock::time_point requestedAt =
        std::chrono::steady_clock::now();
    mutable std::mutex mutex;
    std::unique_ptr<PreparedModelData> prepared;
    std::shared_ptr<const ModelAsset> asset;
    std::string error;
    bool superseded = false;
    std::optional<uint64_t> retireAfterSerial;
};

struct ModelAssetLease {
    explicit ModelAssetLease(std::shared_ptr<ModelAssetRecord> value)
        : record(std::move(value)) {
        ++record->consumers;
    }

    ~ModelAssetLease() {
        if (record)
            --record->consumers;
    }

    std::shared_ptr<ModelAssetRecord> record;
};

} // namespace detail

namespace {

using Record = detail::ModelAssetRecord;

bool loadingState(ModelAssetState state) {
    return state == ModelAssetState::Queued ||
           state == ModelAssetState::PreparingCpu ||
           state == ModelAssetState::ReadyForUpload ||
           state == ModelAssetState::Uploading ||
           state == ModelAssetState::WaitingForGpu;
}

bool terminalStatsAvailable(ModelAssetState state) {
    return state == ModelAssetState::Ready ||
           state == ModelAssetState::Retiring ||
           state == ModelAssetState::Failed ||
           state == ModelAssetState::Cancelled;
}

PipelineConfig standardPipelineConfig(Device &device) {
    return PipelineConfigBuilder{}
        .defaultVertexLayout()
        .msaa(device.msaaSamples())
        .pushConstant(
            {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
             128})
        .build();
}

} // namespace

const char *modelAssetStateName(ModelAssetState state) {
    switch (state) {
    case ModelAssetState::Unloaded:
        return "Unloaded";
    case ModelAssetState::Queued:
        return "Queued";
    case ModelAssetState::PreparingCpu:
        return "PreparingCpu";
    case ModelAssetState::ReadyForUpload:
        return "ReadyForUpload";
    case ModelAssetState::Uploading:
        return "Uploading";
    case ModelAssetState::WaitingForGpu:
        return "WaitingForGpu";
    case ModelAssetState::Ready:
        return "Ready";
    case ModelAssetState::Failed:
        return "Failed";
    case ModelAssetState::Cancelled:
        return "Cancelled";
    case ModelAssetState::Retiring:
        return "Retiring";
    }
    return "Unknown";
}

size_t ModelAssetKeyHash::operator()(const ModelAssetKey &key) const noexcept {
    const size_t left = std::hash<std::string>{}(key.modelId.value());
    const size_t right = std::hash<std::string>{}(key.profileId);
    return left ^ (right + 0x9e3779b9u + (left << 6u) + (left >> 2u));
}

ModelAssetKey ModelAssetHandle::key() const {
    return lease_ && lease_->record ? lease_->record->request.key
                                    : ModelAssetKey{};
}

uint64_t ModelAssetHandle::generation() const {
    return lease_ && lease_->record ? lease_->record->generation : 0;
}

ModelAssetState ModelAssetHandle::state() const {
    return lease_ && lease_->record ? lease_->record->state.load()
                                    : ModelAssetState::Unloaded;
}

std::shared_ptr<const ModelAsset> ModelAssetHandle::asset() const {
    if (!lease_ || !lease_->record)
        return {};
    std::lock_guard<std::mutex> lock(lease_->record->mutex);
    return lease_->record->asset;
}

ModelAssetHandleSnapshot ModelAssetHandle::snapshot() const {
    ModelAssetHandleSnapshot result{};
    if (!lease_ || !lease_->record)
        return result;
    const auto &record = *lease_->record;
    result.key = record.request.key;
    result.generation = record.generation;
    result.state = record.state.load();
    result.texturesCompleted = record.progress.completedTextures.load();
    result.texturesTotal = record.progress.totalTextures.load();
    result.meshesCompleted = record.progress.completedMeshes.load();
    result.meshesTotal = record.progress.totalMeshes.load();
    result.texturesUploaded = record.progress.uploadedTextures.load();
    result.textureUploadTotal = record.progress.uploadTextureTotal.load();
    result.meshesUploaded = record.progress.uploadedMeshes.load();
    result.meshUploadTotal = record.progress.uploadMeshTotal.load();
    result.processedBytes = record.progress.processedBytes.load();
    std::lock_guard<std::mutex> lock(record.mutex);
    result.error = record.error;
    if (terminalStatsAvailable(result.state))
        result.terminalStats = record.stats;
    return result;
}

class AssetRepository::Impl {
  public:
    Impl(Device &device, DescriptorAllocator &descriptorAllocator,
         MaterialSystem &materialSystem)
        : device_(&device), descriptorAllocator_(&descriptorAllocator),
          materialTemplate_(std::make_shared<MaterialTemplate>(
              standardPipelineConfig(device),
              materialSystem.descriptorSetLayout())),
          materialSystem_(&materialSystem),
          worker_([this] { workerLoop(); }) {}

    ~Impl() { shutdown(); }

    ModelAssetHandle requestModel(const ModelAssetRequest &request,
                                  bool *repositoryHit,
                                  bool *coalesced) {
        if (repositoryHit)
            *repositoryHit = false;
        if (coalesced)
            *coalesced = false;
        if (request.key.modelId.empty() || request.key.profileId.empty() ||
            !request.prepareFactory) {
            throw std::invalid_argument("Model asset request is incomplete");
        }

        auto found = active_.find(request.key);
        if (found != active_.end() &&
            request.policy == ModelAssetRequestPolicy::UseCached) {
            const auto &record = found->second;
            const ModelAssetState state = record->state.load();
            if (state == ModelAssetState::Ready ||
                state == ModelAssetState::Retiring) {
                record->retireAfterSerial.reset();
                record->state = ModelAssetState::Ready;
                ++readyHits_;
                if (repositoryHit)
                    *repositoryHit = true;
                VKR_LOG_DEBUG("ModelAsset", "Ready hit for '{}:{}' gen {}",
                              request.key.modelId.value(),
                              request.key.profileId, record->generation);
                return makeHandle(record);
            }
            if (loadingState(state)) {
                ++coalescedRequests_;
                if (coalesced)
                    *coalesced = true;
                VKR_LOG_DEBUG("ModelAsset",
                              "Coalesced request for '{}:{}' gen {}",
                              request.key.modelId.value(),
                              request.key.profileId, record->generation);
                return makeHandle(record);
            }
        }

        if (found != active_.end())
            supersede(found->second);

        auto record = std::make_shared<Record>();
        record->request = request;
        record->request.loadContext.loadStats = nullptr;
        record->request.loadContext.modelId = request.key.modelId.value();
        record->request.loadContext.sceneId = request.key.modelId.value();
        record->request.loadContext.profileId = request.key.profileId;
        record->taskId = nextTaskId_++;
        record->generation = nextGeneration_++;
        record->stats.taskId = record->taskId;
        record->stats.generation = record->generation;
        record->stats.modelGeneration = record->generation;
        record->stats.sceneName = request.displayName;
        record->stats.maxTextureSize = request.loadContext.maxTextureSize;
        record->stats.allocatorBefore = device_->allocatorMemorySnapshot();
        ModelAssetHandle handle = makeHandle(record);
        active_[request.key] = record;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            cpuQueue_.push_back(record);
        }
        queueCondition_.notify_one();
        VKR_LOG_INFO("ModelAsset", "Queued '{}:{}' generation {}",
                     request.key.modelId.value(), request.key.profileId,
                     record->generation);
        return handle;
    }

    void pump(const ModelGpuBuilder::Budget &budget) {
        VKL_PROFILE_ZONE("AssetRepository::pump");
        if (gpuBuilder_) {
            gpuBuilder_->pump(budget);
            if (gpuBuilder_->ready()) {
                auto asset = gpuBuilder_->takeAsset();
                {
                    std::lock_guard<std::mutex> lock(gpuRecord_->mutex);
                    gpuRecord_->asset = std::move(asset);
                }
                gpuRecord_->stats.success = true;
                gpuRecord_->stats.finalState = "Ready";
                gpuRecord_->stats.totalMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() -
                        gpuRecord_->requestedAt)
                        .count();
                gpuRecord_->stats.allocatorAfter =
                    device_->allocatorMemorySnapshot();
                gpuRecord_->state = ModelAssetState::Ready;
                VKR_LOG_INFO("ModelAsset", "Ready '{}:{}' generation {}",
                             gpuRecord_->request.key.modelId.value(),
                             gpuRecord_->request.key.profileId,
                             gpuRecord_->generation);
                gpuBuilder_.reset();
                gpuRecord_.reset();
            } else if (gpuBuilder_->finished()) {
                const bool cancelled = gpuBuilder_->cancelled();
                {
                    std::lock_guard<std::mutex> lock(gpuRecord_->mutex);
                    gpuRecord_->error = gpuBuilder_->error();
                }
                gpuRecord_->stats.success = false;
                gpuRecord_->stats.finalState =
                    cancelled ? "Cancelled" : "Failed";
                gpuRecord_->stats.totalMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() -
                        gpuRecord_->requestedAt)
                        .count();
                gpuRecord_->stats.allocatorAfter =
                    device_->allocatorMemorySnapshot();
                gpuRecord_->state = cancelled ? ModelAssetState::Cancelled
                                              : ModelAssetState::Failed;
                gpuBuilder_.reset();
                gpuRecord_.reset();
            } else if (gpuBuilder_->pendingUploadCount() == 0 &&
                       gpuBuilder_->inFlightUploadBatches() > 0) {
                gpuRecord_->state = ModelAssetState::WaitingForGpu;
            }
        }

        if (gpuBuilder_)
            return;

        for (;;) {
            std::shared_ptr<Record> record;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (gpuQueue_.empty())
                    return;
                record = std::move(gpuQueue_.front());
                gpuQueue_.pop_front();
            }
            if (!record ||
                record->state.load() != ModelAssetState::ReadyForUpload)
                continue;
            if (record->consumers.load() == 0 ||
                record->cancellation->load()) {
                record->cancellation->store(true);
                record->state = ModelAssetState::Cancelled;
                continue;
            }

            std::unique_ptr<PreparedModelData> prepared;
            {
                std::lock_guard<std::mutex> lock(record->mutex);
                prepared = std::move(record->prepared);
            }
            if (!prepared) {
                record->state = ModelAssetState::Failed;
                std::lock_guard<std::mutex> lock(record->mutex);
                record->error = "Prepared model data is unavailable";
                continue;
            }

            ModelGpuBuilder::Context context{};
            context.modelId = record->request.key.modelId.value();
            context.profileId = record->request.key.profileId;
            context.displayName = record->request.displayName;
            context.taskId = record->taskId;
            context.generation = record->generation;
            context.requestedAt = record->requestedAt;
            context.progress = &record->progress;
            context.stats = &record->stats;
            context.cancellation = record->cancellation;
            context.materialTemplate = materialTemplate_;
            gpuRecord_ = record;
            gpuBuilder_ = std::make_unique<ModelGpuBuilder>(
                *device_, *materialSystem_, std::move(context),
                std::move(prepared));
            record->state = ModelAssetState::Uploading;
            ++gpuBuildStarts_;
            return;
        }
    }

    void invalidate(const ModelAssetId &modelId,
                    const std::optional<std::string> &profileId) {
        for (auto it = active_.begin(); it != active_.end();) {
            if (it->first.modelId == modelId &&
                (!profileId || it->first.profileId == *profileId)) {
                supersede(it->second);
                it = active_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void releaseUnused(uint64_t lastSubmittedSerial,
                       uint64_t completedSerial) {
        for (auto it = active_.begin(); it != active_.end();) {
            if (collectRecord(it->second, lastSubmittedSerial,
                              completedSerial)) {
                it = active_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = inactive_.begin(); it != inactive_.end();) {
            if (collectRecord(*it, lastSubmittedSerial, completedSerial))
                it = inactive_.erase(it);
            else
                ++it;
        }
    }

    AssetRepositorySnapshot snapshot() const {
        AssetRepositorySnapshot result{};
        result.cpuPrepareStarts = cpuPrepareStarts_.load();
        result.gpuBuildStarts = gpuBuildStarts_;
        result.readyHits = readyHits_;
        result.coalescedRequests = coalescedRequests_;
        const auto append = [&](const std::shared_ptr<Record> &record) {
            if (!record)
                return;
            ModelAssetRecordSnapshot item{};
            item.key = record->request.key;
            item.generation = record->generation;
            item.state = record->state.load();
            item.consumerCount = record->consumers.load();
            {
                std::lock_guard<std::mutex> lock(record->mutex);
                item.error = record->error;
                if (record->asset) {
                    item.textureCount = record->asset->textures.size();
                    item.meshCount = record->asset->meshes.size();
                    item.materialCount = record->asset->materials.size();
                    item.primitiveCount = record->asset->primitives.size();
                }
            }
            ++result.recordCount;
            if (item.state == ModelAssetState::Ready)
                ++result.readyCount;
            else if (item.state == ModelAssetState::Retiring)
                ++result.retiringCount;
            else if (item.state == ModelAssetState::Failed)
                ++result.failedCount;
            else if (loadingState(item.state))
                ++result.loadingCount;
            result.records.push_back(std::move(item));
        };
        for (const auto &pair : active_)
            append(pair.second);
        for (const auto &record : inactive_)
            append(record);
        std::sort(result.records.begin(), result.records.end(),
                  [](const auto &left, const auto &right) {
                      if (left.key.modelId.value() !=
                          right.key.modelId.value()) {
                          return left.key.modelId.value() <
                                 right.key.modelId.value();
                      }
                      if (left.key.profileId != right.key.profileId)
                          return left.key.profileId < right.key.profileId;
                      return left.generation < right.generation;
                  });
        return result;
    }

    uint64_t pendingUploadCount() const {
        return gpuBuilder_ ? gpuBuilder_->pendingUploadCount() : 0;
    }
    uint64_t pendingTextureCount() const {
        return gpuBuilder_ ? gpuBuilder_->pendingTextureCount() : 0;
    }
    uint64_t pendingMeshCount() const {
        return gpuBuilder_ ? gpuBuilder_->pendingMeshCount() : 0;
    }
    uint32_t inFlightUploadBatches() const {
        return gpuBuilder_ ? gpuBuilder_->inFlightUploadBatches() : 0;
    }
    uint64_t stagingBytesInUse() const {
        return gpuBuilder_ ? gpuBuilder_->stagingBytesInUse() : 0;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (stopping_)
                return;
            stopping_ = true;
            for (auto &pair : active_)
                pair.second->cancellation->store(true);
            for (auto &record : inactive_)
                record->cancellation->store(true);
        }
        queueCondition_.notify_all();
        if (worker_.joinable())
            worker_.join();
        if (gpuBuilder_)
            gpuBuilder_->cancel();
        gpuBuilder_.reset();
        gpuRecord_.reset();
        active_.clear();
        inactive_.clear();
        materialTemplate_.reset();
    }

  private:
    ModelAssetHandle makeHandle(const std::shared_ptr<Record> &record) {
        return ModelAssetHandle(
            std::make_shared<detail::ModelAssetLease>(record));
    }

    void supersede(const std::shared_ptr<Record> &record) {
        if (!record)
            return;
        record->superseded = true;
        if (record->consumers.load() == 0 &&
            loadingState(record->state.load())) {
            record->cancellation->store(true);
        }
        inactive_.push_back(record);
    }

    bool collectRecord(const std::shared_ptr<Record> &record,
                       uint64_t lastSubmittedSerial,
                       uint64_t completedSerial) {
        if (!record)
            return true;
        const uint64_t consumers = record->consumers.load();
        ModelAssetState state = record->state.load();
        if (consumers > 0) {
            if (state == ModelAssetState::Retiring) {
                record->retireAfterSerial.reset();
                record->state = ModelAssetState::Ready;
            }
            return false;
        }
        if (loadingState(state)) {
            record->cancellation->store(true);
            return false;
        }
        if (state == ModelAssetState::Failed ||
            state == ModelAssetState::Cancelled) {
            return true;
        }
        if (state == ModelAssetState::Ready) {
            record->retireAfterSerial = lastSubmittedSerial;
            record->state = ModelAssetState::Retiring;
            state = ModelAssetState::Retiring;
            VKR_LOG_DEBUG("ModelAsset", "Retiring '{}:{}' gen {} after {}",
                          record->request.key.modelId.value(),
                          record->request.key.profileId, record->generation,
                          lastSubmittedSerial);
        }
        if (state == ModelAssetState::Retiring &&
            record->retireAfterSerial &&
            completedSerial >= *record->retireAfterSerial) {
            std::lock_guard<std::mutex> lock(record->mutex);
            record->asset.reset();
            VKR_LOG_DEBUG("ModelAsset", "Released '{}:{}' gen {}",
                          record->request.key.modelId.value(),
                          record->request.key.profileId, record->generation);
            return true;
        }
        return false;
    }

    void workerLoop() {
        profileSetThreadName("ModelPrepare");
        for (;;) {
            std::shared_ptr<Record> record;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCondition_.wait(lock, [this] {
                    return stopping_ || !cpuQueue_.empty();
                });
                if (stopping_)
                    return;
                record = std::move(cpuQueue_.front());
                cpuQueue_.pop_front();
            }
            if (!record || record->cancellation->load() ||
                record->consumers.load() == 0) {
                if (record)
                    record->state = ModelAssetState::Cancelled;
                continue;
            }

            VKL_PROFILE_ZONE("Model Prepare Task");
            VKL_PROFILE_TEXT(record->request.displayName);
            const auto start = std::chrono::steady_clock::now();
            record->stats.workerQueueWaitMs =
                std::chrono::duration<double, std::milli>(
                    start - record->requestedAt)
                    .count();
            record->state = ModelAssetState::PreparingCpu;
            ++cpuPrepareStarts_;
            try {
                SceneLoadContext context = record->request.loadContext;
                context.loadStats = &record->stats;
                auto prepared = std::make_unique<PreparedModelData>(
                    record->request.prepareFactory(
                        context, CancellationToken(record->cancellation),
                        record->progress));
                record->stats.cpuPrepareMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
                record->stats.sceneFactoryMs += record->stats.cpuPrepareMs;
                if (record->cancellation->load() ||
                    record->consumers.load() == 0) {
                    record->state = ModelAssetState::Cancelled;
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(record->mutex);
                    record->prepared = std::move(prepared);
                }
                record->state = ModelAssetState::ReadyForUpload;
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    gpuQueue_.push_back(record);
                }
            } catch (const std::exception &error) {
                std::lock_guard<std::mutex> lock(record->mutex);
                if (record->cancellation->load()) {
                    record->state = ModelAssetState::Cancelled;
                } else {
                    record->error = error.what();
                    record->stats.success = false;
                    record->stats.finalState = "Failed";
                    record->stats.totalMs =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            record->requestedAt)
                            .count();
                    record->state = ModelAssetState::Failed;
                    VKR_LOG_ERROR("ModelAsset",
                                  "CPU prepare for '{}:{}' failed: {}",
                                  record->request.key.modelId.value(),
                                  record->request.key.profileId,
                                  record->error);
                }
            }
        }
    }

    Device *device_ = nullptr;
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::shared_ptr<MaterialTemplate> materialTemplate_;
    MaterialSystem *materialSystem_ = nullptr;
    std::unordered_map<ModelAssetKey, std::shared_ptr<Record>,
                       ModelAssetKeyHash>
        active_;
    std::vector<std::shared_ptr<Record>> inactive_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::deque<std::shared_ptr<Record>> cpuQueue_;
    std::deque<std::shared_ptr<Record>> gpuQueue_;
    std::thread worker_;
    bool stopping_ = false;
    uint64_t nextTaskId_ = 1;
    uint64_t nextGeneration_ = 1;
    std::atomic<uint64_t> cpuPrepareStarts_{0};
    uint64_t gpuBuildStarts_ = 0;
    uint64_t readyHits_ = 0;
    uint64_t coalescedRequests_ = 0;
    std::shared_ptr<Record> gpuRecord_;
    std::unique_ptr<ModelGpuBuilder> gpuBuilder_;
};

AssetRepository::AssetRepository(Device &device,
                                 DescriptorAllocator &descriptorAllocator,
                                 MaterialSystem &materialSystem)
    : impl_(std::make_unique<Impl>(device, descriptorAllocator,
                                  materialSystem)) {}

AssetRepository::~AssetRepository() = default;

ModelAssetHandle AssetRepository::requestModel(
    const ModelAssetRequest &request, bool *repositoryHit, bool *coalesced) {
    return impl_->requestModel(request, repositoryHit, coalesced);
}

void AssetRepository::pump(const ModelGpuBuilder::Budget &budget) {
    impl_->pump(budget);
}

void AssetRepository::invalidate(
    const ModelAssetId &modelId, std::optional<std::string> profileId) {
    impl_->invalidate(modelId, profileId);
}

void AssetRepository::releaseUnused(uint64_t lastSubmittedSerial,
                                    uint64_t completedSerial) {
    impl_->releaseUnused(lastSubmittedSerial, completedSerial);
}

AssetRepositorySnapshot AssetRepository::snapshot() const {
    return impl_->snapshot();
}

uint64_t AssetRepository::pendingUploadCount() const {
    return impl_->pendingUploadCount();
}
uint64_t AssetRepository::pendingTextureCount() const {
    return impl_->pendingTextureCount();
}
uint64_t AssetRepository::pendingMeshCount() const {
    return impl_->pendingMeshCount();
}
uint32_t AssetRepository::inFlightUploadBatches() const {
    return impl_->inFlightUploadBatches();
}
uint64_t AssetRepository::stagingBytesInUse() const {
    return impl_->stagingBytesInUse();
}
void AssetRepository::shutdown() { impl_->shutdown(); }

} // namespace vkr
