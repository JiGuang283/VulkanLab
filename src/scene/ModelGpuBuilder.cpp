#include "ModelGpuBuilder.h"

#include "PreparedModelData.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/IncrementalUploadQueue.h"
#include "core/Log.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/SceneLoadStats.h"
#include "render/FallbackTextures.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/Texture.h"
#include "SceneLoadTask.h"
#include "scene/BoundsMath.h"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

namespace vkr {
namespace {

class UploadPumpStatsScope {
  public:
    UploadPumpStatsScope(ResourceLoadStats &stats, uint64_t &bytes)
        : stats_(stats), bytes_(bytes), start_(Clock::now()) {}
    ~UploadPumpStatsScope() {
        const double elapsed = std::chrono::duration<double, std::milli>(
                                   Clock::now() - start_)
                                   .count();
        ++stats_.uploadPumpCalls;
        stats_.maxUploadPumpMs = std::max(stats_.maxUploadPumpMs, elapsed);
        stats_.maxUploadBytesPerPump =
            std::max(stats_.maxUploadBytesPerPump, bytes_);
    }

  private:
    using Clock = std::chrono::steady_clock;
    ResourceLoadStats &stats_;
    uint64_t &bytes_;
    Clock::time_point start_;
};

} // namespace

ModelGpuBuilder::ModelGpuBuilder(
    Device &device, DescriptorAllocator &descriptorAllocator,
    Context context, std::unique_ptr<PreparedModelData> prepared)
    : device_(&device), descriptorAllocator_(&descriptorAllocator),
      context_(std::move(context)), prepared_(std::move(prepared)) {
    if (!prepared_ || !context_.progress || !context_.stats ||
        !context_.cancellation || !context_.materialTemplate ||
        context_.modelId.empty() || context_.profileId.empty()) {
        throw std::invalid_argument("ModelGpuBuilder context is incomplete");
    }
    uploadQueue_ = std::make_unique<IncrementalUploadQueue>(
        device, &context_.stats->resources, 2,
        IncrementalUploadQueue::kDefaultSlotCapacity, context_.taskId,
        context_.modelId + "/" + context_.profileId,
        "ModelUpload model=" + context_.modelId + " profile=" +
            context_.profileId,
        "Model/" + context_.modelId + "/" + context_.profileId +
            "/Upload");
    asset_ = std::make_shared<ModelAsset>();
    asset_->id = ModelAssetId(context_.modelId);
    asset_->profileId = context_.profileId;
    asset_->generation = context_.generation;
    asset_->materialTemplate = context_.materialTemplate;
    textures_.reserve(prepared_->textures.size());
    meshes_.reserve(prepared_->meshes.size());
    materials_.reserve(prepared_->materials.size());
    context_.progress->uploadTextureTotal = prepared_->textures.size();
    context_.progress->uploadMeshTotal = prepared_->meshes.size();
    buildStart_ = std::chrono::steady_clock::now();
    context_.stats->timeToFirstUploadMs =
        std::chrono::duration<double, std::milli>(buildStart_ -
                                                  context_.requestedAt)
            .count();
}

ModelGpuBuilder::~ModelGpuBuilder() = default;

void ModelGpuBuilder::pump(const Budget &budget) {
    VKL_PROFILE_ZONE("ModelGpuBuilder::pump");
    VKL_PROFILE_TEXT(context_.displayName);
    if (finished())
        return;
    uint64_t recordedBytes = 0;
    UploadPumpStatsScope statsScope(context_.stats->resources, recordedBytes);
    try {
        uploadQueue_->poll();
        if (context_.cancellation->load() && phase_ != Phase::Cancelling) {
            phase_ = Phase::Cancelling;
        }

        if (phase_ == Phase::Cancelling) {
            submitRecorded();
            uploadQueue_->poll();
            if (uploadQueue_->idle()) {
                phase_ = failurePending_ ? Phase::Failed : Phase::Cancelled;
                context_.stats->gpuBuildMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - buildStart_)
                        .count();
            }
            return;
        }

        const auto start = std::chrono::steady_clock::now();
        const std::string debugRoot = "Model/" + context_.modelId + "/" +
                                      context_.profileId;

        if (phase_ == Phase::Fallbacks) {
            static constexpr std::array<std::array<uint8_t, 4>, 3>
                fallbackPixels{{{255, 255, 255, 255},
                                {0, 0, 0, 255},
                                {128, 128, 255, 255}}};
            while (fallbackBuildTextures_.size() < fallbackPixels.size()) {
                UploadRecorder *recorder = uploadQueue_->acquire(4);
                if (!recorder)
                    return;
                const size_t index = fallbackBuildTextures_.size();
                TextureCreateInfo info{};
                info.pixels = fallbackPixels[index].data();
                info.width = 1;
                info.height = 1;
                info.generateMipmaps = false;
                info.format = index == 2 ? VK_FORMAT_R8G8B8A8_UNORM
                                         : VK_FORMAT_R8G8B8A8_SRGB;
                info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                info.debugName = debugRoot + "/FallbackTexture/" +
                                 std::to_string(index);
                fallbackBuildTextures_.push_back(
                    std::make_shared<Texture>(*device_, *recorder, info));
                recordedBytes += 4;
            }
            fallbackTextures_ = std::make_shared<FallbackTextures>(
                fallbackBuildTextures_[0], fallbackBuildTextures_[1],
                fallbackBuildTextures_[2]);
            phase_ = Phase::Textures;
        }

        while (phase_ == Phase::Textures &&
               textureIndex_ < prepared_->textures.size()) {
            PreparedTexture &source = prepared_->textures[textureIndex_];
            if (!source.image || source.image->pixels.empty())
                throw std::runtime_error("Prepared texture has no pixels");
            const uint64_t bytes = source.image->pixels.size();
            UploadRecorder *recorder = uploadQueue_->acquire(bytes);
            if (!recorder)
                break;

            TextureCreateInfo info{};
            info.pixels = source.image->pixels.data();
            info.width = source.image->width;
            info.height = source.image->height;
            info.generateMipmaps = true;
            info.format = source.format;
            info.minFilter = source.minFilter;
            info.magFilter = source.magFilter;
            info.mipmapMode = source.mipmapMode;
            info.wrapU = source.wrapU;
            info.wrapV = source.wrapV;
            info.debugName = debugRoot + "/Texture/" +
                             std::to_string(textureIndex_) +
                             (source.debugName.empty()
                                  ? std::string{}
                                  : "/" + source.debugName);
            std::vector<TextureMipLevelInfo> mipLevels;
            if (source.image->kind ==
                PreparedTextureDataKind::PrebuiltMipChain) {
                if (source.image->mipLevels.empty())
                    throw std::runtime_error(
                        "Prepared texture has an empty mip chain");
                mipLevels.reserve(source.image->mipLevels.size());
                for (const PreparedMipLevel &mip : source.image->mipLevels) {
                    mipLevels.push_back(
                        {mip.offset, mip.size, mip.width, mip.height});
                }
                info.dataSize = source.image->pixels.size();
                info.generateMipmaps = false;
                info.mipLevels = mipLevels.data();
                info.mipLevelCount = static_cast<uint32_t>(mipLevels.size());
            }
            textures_.push_back(
                std::make_shared<Texture>(*device_, *recorder, info));
            source.image.reset();
            ++textureIndex_;
            context_.progress->uploadedTextures = textureIndex_;
            recordedBytes += bytes;
            if (budgetExpired(start, recordedBytes, budget))
                break;
        }
        if (phase_ == Phase::Textures &&
            textureIndex_ == prepared_->textures.size()) {
            phase_ = Phase::Meshes;
        }

        while (phase_ == Phase::Meshes &&
               meshIndex_ < prepared_->meshes.size() &&
               !budgetExpired(start, recordedBytes, budget)) {
            PreparedMesh &source = prepared_->meshes[meshIndex_];
            const uint64_t vertexBytes = source.vertices.size() * sizeof(Vertex);
            const uint64_t indexBytes =
                source.indices.size() * sizeof(uint32_t);
            const uint64_t reservation =
                vertexBytes + indexBytes + (uploadQueue_->copyAlignment() - 1);
            UploadRecorder *recorder = uploadQueue_->acquire(reservation);
            if (!recorder)
                break;
            meshes_.push_back(std::make_shared<Mesh>(
                *device_, *recorder, source.vertices.data(), vertexBytes,
                source.indices.data(),
                static_cast<uint32_t>(source.indices.size()),
                debugRoot + "/Mesh/" + std::to_string(meshIndex_) +
                    (source.debugName.empty() ? std::string{}
                                              : "/" + source.debugName)));
            source.vertices.clear();
            source.vertices.shrink_to_fit();
            source.indices.clear();
            source.indices.shrink_to_fit();
            ++meshIndex_;
            context_.progress->uploadedMeshes = meshIndex_;
            recordedBytes += vertexBytes + indexBytes;
        }
        if (phase_ == Phase::Meshes &&
            meshIndex_ == prepared_->meshes.size()) {
            phase_ = Phase::WaitingForGpu;
        }

        if (phase_ == Phase::WaitingForGpu) {
            submitRecorded();
            uploadQueue_->poll();
            if (!uploadQueue_->idle())
                return;
            phase_ = Phase::Materials;
        }

        if (phase_ == Phase::Materials && !fallbackMaterial_) {
            MaterialParams params{};
            params.debugName = "Fallback Material";
            fallbackMaterial_ = std::make_shared<MaterialInstance>(
                *device_, *descriptorAllocator_, context_.materialTemplate,
                MaterialInstance::makeTextureSet(fallbackTextures_->white(),
                                                 *fallbackTextures_),
                params);
        }

        while (phase_ == Phase::Materials &&
               materialIndex_ < prepared_->materials.size() &&
               !budgetExpired(start, recordedBytes, budget)) {
            PreparedMaterial &source = prepared_->materials[materialIndex_];
            MaterialTextureSet textureSet{};
            for (size_t slotIndex = 0;
                 slotIndex < kMaterialTextureSlotCount; ++slotIndex) {
                const int32_t textureIndex = source.textureIndices[slotIndex];
                const MaterialTextureSlot slot =
                    static_cast<MaterialTextureSlot>(slotIndex);
                textureSet[slotIndex] =
                    textureIndex >= 0 &&
                            textureIndex < static_cast<int32_t>(textures_.size())
                        ? textures_[static_cast<size_t>(textureIndex)]
                        : fallbackTextures_->textureFor(slot);
            }
            materials_.push_back(std::make_shared<MaterialInstance>(
                *device_, *descriptorAllocator_, context_.materialTemplate,
                std::move(textureSet), std::move(source.params)));
            ++materialIndex_;
        }
        if (phase_ == Phase::Materials &&
            materialIndex_ == prepared_->materials.size()) {
            phase_ = Phase::Finalize;
        }

        if (phase_ == Phase::Finalize)
            finalizeAsset();
    } catch (const std::exception &error) {
        fail(error);
    }
}

void ModelGpuBuilder::finalizeAsset() {
    asset_->textures.reserve(fallbackBuildTextures_.size() + textures_.size());
    asset_->textures.insert(asset_->textures.end(),
                            fallbackBuildTextures_.begin(),
                            fallbackBuildTextures_.end());
    asset_->textures.insert(asset_->textures.end(), textures_.begin(),
                            textures_.end());
    asset_->meshes = meshes_;
    asset_->materials = materials_;
    asset_->materials.push_back(fallbackMaterial_);
    asset_->primitives.reserve(prepared_->primitives.size());
    for (const PreparedModelPrimitive &source : prepared_->primitives) {
        if (source.meshIndex >= meshes_.size())
            throw std::runtime_error("Prepared primitive mesh is invalid");
        std::shared_ptr<MaterialInstance> material =
            source.materialIndex >= 0 &&
                    source.materialIndex <
                        static_cast<int32_t>(materials_.size())
                ? materials_[static_cast<size_t>(source.materialIndex)]
                : fallbackMaterial_;
        asset_->primitives.push_back(
            {meshes_[source.meshIndex], std::move(material),
             source.localToAsset});
        includeTransformedBounds(asset_->localBounds,
                                 meshes_[source.meshIndex]->localBounds(),
                                 source.localToAsset);
    }
    asset_->lights = std::move(prepared_->lights);
    asset_->previewCamera = prepared_->previewCamera;

    context_.stats->materialCount = asset_->materials.size();
    context_.stats->objectCount = asset_->primitives.size();
    context_.stats->primitiveCount = asset_->primitives.size();
    context_.stats->lightInstanceCount = asset_->lights.size();
    context_.stats->directionalLightCount = 0;
    context_.stats->pointLightCount = 0;
    context_.stats->spotLightCount = 0;
    for (const ModelLightPrototype &light : asset_->lights) {
        switch (light.type) {
        case LightType::Directional:
            ++context_.stats->directionalLightCount;
            break;
        case LightType::Point:
            ++context_.stats->pointLightCount;
            break;
        case LightType::Spot:
            ++context_.stats->spotLightCount;
            break;
        }
    }
    context_.stats->gpuBuildMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - buildStart_)
            .count();
    phase_ = Phase::Ready;
}

void ModelGpuBuilder::cancel() {
    context_.cancellation->store(true);
    if (!finished())
        phase_ = Phase::Cancelling;
}

bool ModelGpuBuilder::ready() const { return phase_ == Phase::Ready; }

bool ModelGpuBuilder::finished() const {
    return phase_ == Phase::Ready || phase_ == Phase::Cancelled ||
           phase_ == Phase::Failed;
}

bool ModelGpuBuilder::cancelled() const {
    return phase_ == Phase::Cancelled;
}

std::shared_ptr<const ModelAsset> ModelGpuBuilder::takeAsset() {
    if (!ready())
        return {};
    return std::move(asset_);
}

uint64_t ModelGpuBuilder::pendingTextureCount() const {
    return prepared_ && textureIndex_ < prepared_->textures.size()
               ? static_cast<uint64_t>(prepared_->textures.size() -
                                       textureIndex_)
               : 0;
}

uint64_t ModelGpuBuilder::pendingMeshCount() const {
    return prepared_ && meshIndex_ < prepared_->meshes.size()
               ? static_cast<uint64_t>(prepared_->meshes.size() - meshIndex_)
               : 0;
}

uint64_t ModelGpuBuilder::pendingUploadCount() const {
    uint64_t pending = pendingTextureCount() + pendingMeshCount();
    if (pending == 0 && uploadQueue_ && !uploadQueue_->idle())
        pending = 1;
    return pending;
}

uint32_t ModelGpuBuilder::inFlightUploadBatches() const {
    return uploadQueue_ ? uploadQueue_->inFlightCount() : 0;
}

uint64_t ModelGpuBuilder::stagingBytesInUse() const {
    return uploadQueue_
               ? static_cast<uint64_t>(uploadQueue_->stagingBytesInUse())
               : 0;
}

bool ModelGpuBuilder::budgetExpired(
    const std::chrono::steady_clock::time_point &start, uint64_t bytes,
    const Budget &budget) const {
    if (bytes >= budget.maxUploadBytes)
        return true;
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
               .count() >= budget.maxRecordMs;
}

void ModelGpuBuilder::submitRecorded() { uploadQueue_->submitActive(); }

void ModelGpuBuilder::fail(const std::exception &error) {
    error_ = error.what();
    failurePending_ = true;
    phase_ = Phase::Cancelling;
    VKR_LOG_ERROR("ModelAsset", "GPU build for '{}:{}' failed: {}",
                  context_.modelId, context_.profileId, error.what());
}

} // namespace vkr
