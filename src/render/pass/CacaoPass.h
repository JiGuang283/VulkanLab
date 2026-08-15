#pragma once

#include "render/RenderSettings.h"
#include "render/pass/IRenderPass.h"

#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class RenderResourceRegistry;

struct CacaoRuntimeStatus {
    bool compiled = false;
    bool supported = false;
    bool initialized = false;
    bool active = false;
    bool fp32 = true;
    bool internalMemoryTracked = false;
    CacaoResolution resolution = CacaoResolution::Half;
    VkExtent2D outputExtent{};
    uint64_t generation = 0;
    std::string unavailableReason;
};

class CacaoPass final : public IRenderPass {
  public:
    CacaoPass(Device &device, const RenderResourceRegistry &resources,
              RendererResourceHandles resourceHandles,
              CacaoResolution resolution);
    ~CacaoPass() override;

    CacaoPass(const CacaoPass &) = delete;
    CacaoPass &operator=(const CacaoPass &) = delete;

    std::string_view name() const override { return "CACAO"; }
    // FidelityFX CACAO records its own internal image barriers. The frame
    // graph treats the SDK invocation as one black-box external node while
    // still tracking its imported inputs and output.
    RgPassType passType() const override { return RgPassType::External; }
    RgPassCondition condition() const override { return RgPassCondition::Cacao; }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    bool managesDeclaredTransitionsInternally() const override { return true; }
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourceRegistry &resources) override;
    void recordNode(RenderGraphPassContext &context, uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;

    bool reconfigure(const RenderResourceRegistry &resources,
                     CacaoResolution resolution, std::string &error);
    const CacaoRuntimeStatus &status() const { return status_; }

  private:
    struct Impl;

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    CacaoResolution requestedResolution_ = CacaoResolution::Half;
    std::unique_ptr<Impl> impl_;
    CacaoRuntimeStatus status_{};
};

} // namespace vkr
