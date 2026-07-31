#include "SceneGpuBuilder.h"

#include "PreparedSceneData.h"
#include "Scene.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/IncrementalUploadQueue.h"
#include "core/Log.h"
#include "core/PipelineConfigBuilder.h"
#include "render/FallbackTextures.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/Texture.h"

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

SceneGpuBuilder::SceneGpuBuilder(
    Device &device, DescriptorAllocator &descriptorAllocator,
    std::shared_ptr<SceneLoadTask> task,
    std::unique_ptr<PreparedSceneData> prepared)
    : device_(&device), descriptorAllocator_(&descriptorAllocator),
      task_(std::move(task)), prepared_(std::move(prepared)) {
    if (!task_ || !prepared_)
        throw std::invalid_argument("SceneGpuBuilder requires task and data");
    uploadQueue_ = std::make_unique<IncrementalUploadQueue>(
        device, &task_->stats.resources, 2,
        IncrementalUploadQueue::kDefaultSlotCapacity, task_->id,
        task_->sceneName);
    scene_ = std::make_unique<Scene>();
    textures_.reserve(prepared_->textures.size());
    meshes_.reserve(prepared_->meshes.size());
    materials_.reserve(prepared_->materials.size());
    task_->progress.uploadTextureTotal = prepared_->textures.size();
    task_->progress.uploadMeshTotal = prepared_->meshes.size();
    task_->state = SceneLoadState::Uploading;
    buildStart_ = std::chrono::steady_clock::now();
    task_->stats.timeToFirstUploadMs =
        std::chrono::duration<double, std::milli>(buildStart_ -
                                                  task_->requestedAt)
            .count();
}

SceneGpuBuilder::~SceneGpuBuilder() = default;

void SceneGpuBuilder::pump(const Budget &budget) {
    if (finished())
        return;
    uint64_t recordedBytes = 0;
    UploadPumpStatsScope pumpStats(task_->stats.resources, recordedBytes);
    try {
        uploadQueue_->poll();
        if (task_->cancellation->load() && phase_ != Phase::Cancelling) {
            phase_ = Phase::Cancelling;
            task_->state = SceneLoadState::Cancelling;
        }

        if (phase_ == Phase::Cancelling) {
            submitRecorded();
            uploadQueue_->poll();
            if (uploadQueue_->idle()) {
                phase_ = failurePending_ ? Phase::Failed : Phase::Cancelled;
                task_->state = failurePending_ ? SceneLoadState::Failed
                                               : SceneLoadState::Cancelled;
                task_->stats.gpuBuildMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - buildStart_)
                        .count();
            }
            return;
        }

        const auto start = std::chrono::steady_clock::now();

        if (phase_ == Phase::Fallbacks) {
            if (!materialTemplate_) {
                materialTemplate_ = std::make_shared<MaterialTemplate>(
                    *device_, standardPipelineConfig(*device_));
            }
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
                info.debugName =
                    "Scene/" + task_->sceneName + "/FallbackTexture/" +
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
            info.maxExtent = 0;
            info.generateMipmaps = true;
            info.format = source.format;
            info.minFilter = source.minFilter;
            info.magFilter = source.magFilter;
            info.mipmapMode = source.mipmapMode;
            info.wrapU = source.wrapU;
            info.wrapV = source.wrapV;
            info.debugName =
                "Scene/" + task_->sceneName + "/Texture/" +
                std::to_string(textureIndex_) +
                (source.debugName.empty() ? std::string{}
                                          : "/" + source.debugName);
            std::vector<TextureMipLevelInfo> mipLevels;
            if (source.image->kind ==
                PreparedTextureDataKind::PrebuiltMipChain) {
                if (source.image->mipLevels.empty())
                    throw std::runtime_error(
                        "Prepared texture has an empty mip chain");
                mipLevels.reserve(source.image->mipLevels.size());
                for (const PreparedMipLevel &mip : source.image->mipLevels) {
                    mipLevels.push_back({mip.offset, mip.size, mip.width,
                                         mip.height});
                }
                info.dataSize = source.image->pixels.size();
                info.generateMipmaps = false;
                info.mipLevels = mipLevels.data();
                info.mipLevelCount =
                    static_cast<uint32_t>(mipLevels.size());
            }
            textures_.push_back(
                std::make_shared<Texture>(*device_, *recorder, info));
            source.image.reset();
            ++textureIndex_;
            task_->progress.uploadedTextures = textureIndex_;
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
            const uint64_t vertexBytes =
                source.vertices.size() * sizeof(Vertex);
            const uint64_t indexBytes =
                source.indices.size() * sizeof(uint32_t);
            const uint64_t reservation =
                vertexBytes + indexBytes +
                (uploadQueue_->copyAlignment() - 1);
            UploadRecorder *recorder = uploadQueue_->acquire(reservation);
            if (!recorder)
                break;
            meshes_.push_back(std::make_shared<Mesh>(
                *device_, *recorder, source.vertices.data(), vertexBytes,
                source.indices.data(),
                static_cast<uint32_t>(source.indices.size()),
                "Scene/" + task_->sceneName + "/Mesh/" +
                    std::to_string(meshIndex_) +
                    (source.debugName.empty() ? std::string{}
                                              : "/" + source.debugName)));
            source.vertices.clear();
            source.vertices.shrink_to_fit();
            source.indices.clear();
            source.indices.shrink_to_fit();
            ++meshIndex_;
            task_->progress.uploadedMeshes = meshIndex_;
            recordedBytes += vertexBytes + indexBytes;
        }
        if (phase_ == Phase::Meshes &&
            meshIndex_ == prepared_->meshes.size()) {
            phase_ = Phase::WaitingForGpu;
            task_->state = SceneLoadState::WaitingForGpu;
        }

        if (phase_ == Phase::WaitingForGpu) {
            submitRecorded();
            uploadQueue_->poll();
            if (!uploadQueue_->idle())
                return;
            phase_ = Phase::Materials;
            task_->state = SceneLoadState::Uploading;
        }

        if (phase_ == Phase::Materials && !fallbackMaterial_) {
            MaterialParams params{};
            params.debugName = "Fallback Material";
            fallbackMaterial_ = std::make_shared<MaterialInstance>(
                *device_, *descriptorAllocator_, materialTemplate_,
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
                const int32_t textureIndex =
                    source.textureIndices[slotIndex];
                const MaterialTextureSlot slot =
                    static_cast<MaterialTextureSlot>(slotIndex);
                textureSet[slotIndex] =
                    textureIndex >= 0 &&
                            textureIndex < static_cast<int32_t>(textures_.size())
                        ? textures_[static_cast<size_t>(textureIndex)]
                        : fallbackTextures_->textureFor(slot);
            }
            materials_.push_back(std::make_shared<MaterialInstance>(
                *device_, *descriptorAllocator_, materialTemplate_,
                std::move(textureSet), std::move(source.params)));
            ++materialIndex_;
        }
        if (phase_ == Phase::Materials &&
            materialIndex_ == prepared_->materials.size()) {
            phase_ = Phase::Objects;
        }

        while (phase_ == Phase::Objects &&
               objectIndex_ < prepared_->objects.size() &&
               !budgetExpired(start, recordedBytes, budget)) {
            const PreparedObject &source = prepared_->objects[objectIndex_];
            if (source.meshIndex >= meshes_.size())
                throw std::runtime_error("Prepared object mesh is invalid");
            std::shared_ptr<MaterialInstance> material =
                source.materialIndex >= 0 &&
                        source.materialIndex <
                            static_cast<int32_t>(materials_.size())
                    ? materials_[static_cast<size_t>(source.materialIndex)]
                    : fallbackMaterial_;
            scene_->addObject(
                {meshes_[source.meshIndex], std::move(material),
                 source.transform});
            ++objectIndex_;
        }
        if (phase_ == Phase::Objects &&
            objectIndex_ == prepared_->objects.size()) {
            scene_->addMaterialTemplate(materialTemplate_);
            for (auto &texture : textures_)
                scene_->addTexture(texture);
            for (auto &material : materials_)
                scene_->addMaterial(material);
            scene_->addMaterial(fallbackMaterial_);
            for (auto &mesh : meshes_)
                scene_->addMesh(mesh);
            for (SceneLight &light : prepared_->lights)
                scene_->addLight(std::move(light));
            prepared_->lights.clear();
            scene_->initialCamera = prepared_->initialCamera;
            task_->stats.materialCount = materials_.size() + 1;
            task_->stats.objectCount = scene_->objects().size();
            task_->stats.lightInstanceCount = scene_->lights().size();
            task_->stats.directionalLightCount = 0;
            task_->stats.pointLightCount = 0;
            task_->stats.spotLightCount = 0;
            for (const SceneLight &light : scene_->lights()) {
                switch (light.type) {
                case LightType::Directional:
                    ++task_->stats.directionalLightCount;
                    break;
                case LightType::Point:
                    ++task_->stats.pointLightCount;
                    break;
                case LightType::Spot:
                    ++task_->stats.spotLightCount;
                    break;
                }
            }
            task_->state = SceneLoadState::ReadyToPublish;
            phase_ = Phase::Ready;
            task_->stats.gpuBuildMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - buildStart_)
                    .count();
        }
    } catch (const std::exception &error) {
        fail(error);
    }
}

void SceneGpuBuilder::cancel() {
    task_->cancellation->store(true);
    if (!finished()) {
        phase_ = Phase::Cancelling;
        task_->state = SceneLoadState::Cancelling;
    }
}

bool SceneGpuBuilder::ready() const { return phase_ == Phase::Ready; }

bool SceneGpuBuilder::finished() const {
    return phase_ == Phase::Ready || phase_ == Phase::Cancelled ||
           phase_ == Phase::Failed;
}

bool SceneGpuBuilder::cancelled() const {
    return phase_ == Phase::Cancelled;
}

std::unique_ptr<Scene> SceneGpuBuilder::takeScene() {
    if (!ready())
        return {};
    return std::move(scene_);
}

uint64_t SceneGpuBuilder::pendingTextureCount() const {
    return prepared_ && textureIndex_ < prepared_->textures.size()
               ? static_cast<uint64_t>(prepared_->textures.size() -
                                       textureIndex_)
               : 0;
}

uint64_t SceneGpuBuilder::pendingMeshCount() const {
    return prepared_ && meshIndex_ < prepared_->meshes.size()
               ? static_cast<uint64_t>(prepared_->meshes.size() - meshIndex_)
               : 0;
}

uint64_t SceneGpuBuilder::pendingUploadCount() const {
    uint64_t pending = pendingTextureCount() + pendingMeshCount();
    if (pending == 0 && uploadQueue_ && !uploadQueue_->idle())
        pending = 1;
    return pending;
}

uint32_t SceneGpuBuilder::inFlightUploadBatches() const {
    return uploadQueue_ ? uploadQueue_->inFlightCount() : 0;
}

bool SceneGpuBuilder::budgetExpired(
    const std::chrono::steady_clock::time_point &start, uint64_t bytes,
    const Budget &budget) const {
    if (bytes >= budget.maxUploadBytes)
        return true;
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
               .count() >= budget.maxRecordMs;
}

void SceneGpuBuilder::submitRecorded() { uploadQueue_->submitActive(); }

void SceneGpuBuilder::fail(const std::exception &error) {
    {
        std::lock_guard<std::mutex> lock(task_->mutex);
        task_->error = error.what();
    }
    failurePending_ = true;
    task_->state = SceneLoadState::Cancelling;
    phase_ = Phase::Cancelling;
    VKR_LOG_ERROR("Scene", "GPU build for '{}' failed: {}",
                  task_->sceneName, error.what());
}

} // namespace vkr
