#include "EnvironmentGpuBuilder.h"

#include "assets/EnvironmentLoadManager.h"
#include "assets/PreparedEnvironment.h"
#include "core/Device.h"
#include "core/IncrementalUploadQueue.h"
#include "render/Texture.h"
#include "diagnostics/Profiling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vkr {

EnvironmentGpuBuilder::EnvironmentGpuBuilder(
    Device &device, std::shared_ptr<EnvironmentLoadTask> task,
    std::unique_ptr<PreparedEnvironmentData> prepared)
    : device_(&device), task_(std::move(task)),
      prepared_(std::move(prepared)),
      resources_(std::make_shared<EnvironmentGpuResources>()) {
    if (!task_ || !prepared_)
        throw std::invalid_argument(
            "EnvironmentGpuBuilder requires task and prepared data");
    uploadQueue_ = std::make_unique<IncrementalUploadQueue>(
        device, nullptr, 2, IncrementalUploadQueue::kDefaultSlotCapacity,
        task_->id, "Environment/" + task_->environmentId);
    resources_->environmentId = prepared_->environmentId;
    resources_->displayName = prepared_->displayName;
    resources_->profileId = prepared_->profileId;
    task_->state = EnvironmentLoadState::Uploading;
}

EnvironmentGpuBuilder::~EnvironmentGpuBuilder() = default;

void EnvironmentGpuBuilder::pump(const Budget &budget) {
    VKL_PROFILE_ZONE("EnvironmentGpuBuilder::pump");
    VKL_PROFILE_TEXT(task_->environmentId);
    if (finished())
        return;
    try {
        uploadQueue_->poll();
        if (task_->cancellation->load() &&
            phase_ != Phase::Cancelling) {
            phase_ = Phase::Cancelling;
            task_->state = EnvironmentLoadState::Cancelling;
        }
        if (phase_ == Phase::Cancelling) {
            submitRecorded();
            uploadQueue_->poll();
            if (uploadQueue_->idle()) {
                phase_ =
                    failurePending_ ? Phase::Failed : Phase::Cancelled;
                task_->state =
                    failurePending_ ? EnvironmentLoadState::Failed
                                    : EnvironmentLoadState::Cancelled;
            }
            return;
        }

        const auto start = std::chrono::steady_clock::now();
        uint64_t recordedBytes = 0;
        std::array<PreparedEnvironmentImage *, 4> images{
            &prepared_->radiance, &prepared_->irradiance,
            &prepared_->prefilteredSpecular, &prepared_->brdfLut};
        while (phase_ == Phase::Images && imageIndex_ < images.size()) {
            PreparedEnvironmentImage &source = *images[imageIndex_];
            if (source.bytes.empty() || source.subresources.empty())
                throw std::runtime_error(
                    "Prepared environment image has no payload");
            UploadRecorder *recorder =
                uploadQueue_->acquire(source.bytes.size());
            if (!recorder)
                break;

            std::vector<TextureSubresourceInfo> subresources;
            subresources.reserve(source.subresources.size());
            for (const PreparedEnvironmentSubresource &subresource :
                 source.subresources) {
                subresources.push_back(
                    {subresource.offset, subresource.size,
                     subresource.width, subresource.height,
                     subresource.mipLevel, subresource.arrayLayer});
            }
            const bool cube = source.arrayLayers == 6;
            TextureCreateInfo info{};
            info.pixels = source.bytes.data();
            info.dataSize = source.bytes.size();
            info.width = source.width;
            info.height = source.height;
            info.generateMipmaps = false;
            info.format = source.format;
            info.wrapU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.wrapV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.wrapW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.subresources = subresources.data();
            info.subresourceCount =
                static_cast<uint32_t>(subresources.size());
            info.arrayLayers = source.arrayLayers;
            info.imageFlags =
                cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
            info.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE
                                 : VK_IMAGE_VIEW_TYPE_2D;
            info.debugName =
                "Environment/" + task_->environmentId + "/" +
                environmentMapKindName(source.kind);
            std::shared_ptr<Texture> texture =
                std::make_shared<Texture>(*device_, *recorder, info);
            switch (source.kind) {
            case EnvironmentMapKind::Radiance:
                resources_->radiance = std::move(texture);
                break;
            case EnvironmentMapKind::Irradiance:
                resources_->irradiance = std::move(texture);
                break;
            case EnvironmentMapKind::PrefilteredSpecular:
                resources_->maxSpecularLod =
                    static_cast<float>(source.mipLevels - 1);
                resources_->prefilteredSpecular = std::move(texture);
                break;
            case EnvironmentMapKind::BrdfLut:
                resources_->brdfLut = std::move(texture);
                break;
            }
            recordedBytes += source.bytes.size();
            source.bytes.clear();
            source.bytes.shrink_to_fit();
            ++imageIndex_;
            task_->uploadedImages = imageIndex_;
            const double elapsed =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start)
                    .count();
            if (recordedBytes >= budget.maxUploadBytes ||
                elapsed >= budget.maxRecordMs) {
                break;
            }
        }
        if (phase_ == Phase::Images && imageIndex_ == images.size()) {
            phase_ = Phase::WaitingForGpu;
            task_->state = EnvironmentLoadState::WaitingForGpu;
        }
        if (phase_ == Phase::WaitingForGpu) {
            submitRecorded();
            uploadQueue_->poll();
            if (!uploadQueue_->idle())
                return;
            if (!resources_->radiance || !resources_->irradiance ||
                !resources_->prefilteredSpecular ||
                !resources_->brdfLut) {
                throw std::runtime_error(
                    "Environment GPU resources are incomplete");
            }
            phase_ = Phase::Ready;
            task_->state = EnvironmentLoadState::ReadyToPublish;
        }
    } catch (const std::exception &error) {
        fail(error);
    }
}

void EnvironmentGpuBuilder::cancel() {
    task_->cancellation->store(true);
    if (!finished()) {
        phase_ = Phase::Cancelling;
        task_->state = EnvironmentLoadState::Cancelling;
    }
}

bool EnvironmentGpuBuilder::ready() const {
    return phase_ == Phase::Ready;
}

bool EnvironmentGpuBuilder::finished() const {
    return phase_ == Phase::Ready || phase_ == Phase::Cancelled ||
           phase_ == Phase::Failed;
}

std::shared_ptr<EnvironmentGpuResources>
EnvironmentGpuBuilder::takeResources() {
    if (!ready())
        return {};
    return std::move(resources_);
}

void EnvironmentGpuBuilder::submitRecorded() {
    uploadQueue_->submitActive();
}

void EnvironmentGpuBuilder::fail(const std::exception &error) {
    {
        std::lock_guard<std::mutex> lock(task_->mutex);
        task_->error = error.what();
    }
    failurePending_ = true;
    task_->state = EnvironmentLoadState::Cancelling;
    phase_ = Phase::Cancelling;
}

} // namespace vkr
