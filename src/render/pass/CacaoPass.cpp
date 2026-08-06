#include "render/pass/CacaoPass.h"

#include "core/Device.h"
#include "core/GpuBarrier.h"
#include "core/Image.h"
#include "core/Log.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/RenderFrame.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"

#include <ffx_cacao.h>
#include <ffx_cacao_impl.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

namespace vkr {

namespace {

const char *statusName(FFX_CACAO_Status status) {
    switch (status) {
    case FFX_CACAO_STATUS_OK:
        return "ok";
    case FFX_CACAO_STATUS_INVALID_ARGUMENT:
        return "invalid_argument";
    case FFX_CACAO_STATUS_INVALID_POINTER:
        return "invalid_pointer";
    case FFX_CACAO_STATUS_OUT_OF_MEMORY:
        return "out_of_memory";
    case FFX_CACAO_STATUS_FAILED:
        return "failed";
    }
    return "unknown";
}

FFX_CACAO_Quality qualityValue(CacaoQuality quality) {
    switch (quality) {
    case CacaoQuality::Lowest:
        return FFX_CACAO_QUALITY_LOWEST;
    case CacaoQuality::Low:
        return FFX_CACAO_QUALITY_LOW;
    case CacaoQuality::Medium:
        return FFX_CACAO_QUALITY_MEDIUM;
    case CacaoQuality::High:
        return FFX_CACAO_QUALITY_HIGH;
    case CacaoQuality::Highest:
        return FFX_CACAO_QUALITY_HIGHEST;
    }
    return FFX_CACAO_QUALITY_HIGH;
}

FFX_CACAO_Matrix4x4 cacaoProjection(glm::mat4 projection) {
    projection[1][1] *= -1.0f;
    FFX_CACAO_Matrix4x4 result{};
    for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row)
            result.elements[column][row] = projection[column][row];
    }
    return result;
}

FFX_CACAO_Matrix4x4 identityMatrix() {
    FFX_CACAO_Matrix4x4 result{};
    for (uint32_t index = 0; index < 4; ++index)
        result.elements[index][index] = 1.0f;
    return result;
}

} // namespace

struct CacaoPass::Impl {
    struct Context {
        std::vector<std::byte> storage;
        FFX_CACAO_VkContext *handle = nullptr;
        bool contextInitialized = false;
        bool screenInitialized = false;
    };

    Device *device = nullptr;
    std::array<Context, MAX_FRAMES_IN_FLIGHT> contexts{};

    ~Impl() {
        for (Context &context : contexts) {
            if (context.screenInitialized)
                FFX_CACAO_VkDestroyScreenSizeDependentResources(
                    context.handle);
            if (context.contextInitialized)
                FFX_CACAO_VkDestroyContext(context.handle);
        }
    }

    bool initialize(const RenderResourceRegistry &resources,
                    const RendererResourceHandles &handles,
                    CacaoResolution resolution, std::string &error) {
        const size_t contextSize = FFX_CACAO_VkGetContextSize();
        const VkExtent2D extent = resources.extent(handles.cacaoOutput);
        for (uint32_t frame = 0; frame < contexts.size(); ++frame) {
            Context &context = contexts[frame];
            context.storage.resize(contextSize);
            context.handle = reinterpret_cast<FFX_CACAO_VkContext *>(
                context.storage.data());
            FFX_CACAO_VkCreateInfo createInfo{};
            createInfo.physicalDevice = device->physicalDevice();
            createInfo.device = device->logicalDevice();
            createInfo.flags = 0;
            FFX_CACAO_Status result =
                FFX_CACAO_VkInitContext(context.handle, &createInfo);
            if (result != FFX_CACAO_STATUS_OK) {
                error = "FFX_CACAO_VkInitContext failed: " +
                        std::string(statusName(result));
                return false;
            }
            context.contextInitialized = true;

            FFX_CACAO_VkScreenSizeInfo screenInfo{};
            screenInfo.width = extent.width;
            screenInfo.height = extent.height;
            screenInfo.depthView =
                resources.image(handles.cacaoDepth, frame).imageView();
            screenInfo.normalsView =
                resources.image(handles.cacaoViewNormals, frame).imageView();
            screenInfo.output =
                resources.image(handles.cacaoOutput, frame).handle();
            screenInfo.outputView =
                resources.image(handles.cacaoOutput, frame).imageView();
            screenInfo.useDownsampledSsao =
                resolution == CacaoResolution::Half ? FFX_CACAO_TRUE
                                                    : FFX_CACAO_FALSE;
            result = FFX_CACAO_VkInitScreenSizeDependentResources(
                context.handle, &screenInfo);
            if (result != FFX_CACAO_STATUS_OK) {
                error =
                    "FFX_CACAO_VkInitScreenSizeDependentResources failed: " +
                    std::string(statusName(result));
                return false;
            }
            context.screenInitialized = true;
        }
        return true;
    }
};

CacaoPass::CacaoPass(Device &device,
                     const RenderResourceRegistry &resources,
                     RendererResourceHandles resourceHandles,
                     CacaoResolution resolution)
    : device_(&device), resourceHandles_(resourceHandles),
      requestedResolution_(resolution) {
    status_.compiled = device.cacaoSupport().compiled;
    status_.supported = device.cacaoSupport().available;
    status_.fp32 = true;
    status_.resolution = resolution;
    status_.unavailableReason = device.cacaoSupport().reason;
    if (!status_.supported)
        return;
    std::string error;
    if (!reconfigure(resources, resolution, error)) {
        status_.unavailableReason = error;
        VKR_LOG_WARN("CACAO", "Initialization failed: {}", error);
    }
}

CacaoPass::~CacaoPass() = default;

std::vector<RenderImageUsage> CacaoPass::resourceUsages() const {
    if (!resourceHandles_.cacaoOutput.valid())
        return {};
    return {{resourceHandles_.cacaoDepth, RenderImageAccess::SampledRead,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {resourceHandles_.cacaoViewNormals,
             RenderImageAccess::SampledRead,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {resourceHandles_.cacaoOutput, RenderImageAccess::StorageWrite,
             VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
}

void CacaoPass::releaseViewportResources() {
    impl_.reset();
    status_.initialized = false;
    status_.active = false;
    status_.outputExtent = {};
}

void CacaoPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    std::string error;
    if (!reconfigure(resources, requestedResolution_, error)) {
        status_.unavailableReason = error;
        VKR_LOG_WARN("CACAO", "Resize initialization failed: {}", error);
    }
}

bool CacaoPass::reconfigure(const RenderResourceRegistry &resources,
                            CacaoResolution resolution,
                            std::string &error) {
    if (!status_.supported) {
        error = status_.unavailableReason;
        return false;
    }
    auto candidate = std::make_unique<Impl>();
    candidate->device = device_;
    if (!candidate->initialize(resources, resourceHandles_, resolution,
                               error)) {
        return false;
    }
    impl_ = std::move(candidate);
    requestedResolution_ = resolution;
    status_.resolution = resolution;
    status_.initialized = true;
    status_.active = false;
    status_.outputExtent = resources.extent(resourceHandles_.cacaoOutput);
    status_.unavailableReason.clear();
    ++status_.generation;
    VKR_LOG_INFO("CACAO", "Initialized {} resolution at {}x{} (FP32)",
                 cacaoResolutionName(resolution), status_.outputExtent.width,
                 status_.outputExtent.height);
    return true;
}

void CacaoPass::execute(const RenderFrameContext &frame,
                        const RenderResourceRegistry &resources,
                        const VisibilityFrame &) {
    status_.active = false;
    if (!frame.features.cacaoRequired || !impl_ || !frame.view)
        return;

    VKL_PROFILE_ZONE("Record CACAO");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "CACAO");
    FFX_CACAO_Settings settings = FFX_CACAO_DEFAULT_SETTINGS;
    settings.qualityLevel = qualityValue(frame.view->settings.cacao.quality);
    settings.radius = frame.view->settings.cacao.radius;
    settings.shadowMultiplier = frame.view->settings.cacao.intensity;
    settings.shadowPower = frame.view->settings.cacao.power;
    settings.temporalSupersamplingAngleOffset = 0.0f;
    settings.temporalSupersamplingRadiusOffset = 0.0f;
    settings.generateNormals = FFX_CACAO_FALSE;

    Impl::Context &context = impl_->contexts.at(frame.frameIndex);
    FFX_CACAO_Status result =
        FFX_CACAO_VkUpdateSettings(context.handle, &settings);
    if (result == FFX_CACAO_STATUS_OK) {
        const FFX_CACAO_Matrix4x4 projection =
            cacaoProjection(frame.view->globalUbo.proj);
        const FFX_CACAO_Matrix4x4 normalsToView = identityMatrix();
        result = FFX_CACAO_VkDraw(context.handle, frame.cmd, &projection,
                                  &normalsToView);
    }
    if (result != FFX_CACAO_STATUS_OK) {
        status_.unavailableReason =
            "CACAO draw failed: " + std::string(statusName(result));
        VKR_LOG_ERROR("CACAO", "{}", status_.unavailableReason);
        return;
    }

    const Image &output =
        resources.image(resourceHandles_.cacaoOutput, frame.frameIndex);
    cmdImageBarrier(frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT,
                    output.handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
    status_.active = true;
}

} // namespace vkr
