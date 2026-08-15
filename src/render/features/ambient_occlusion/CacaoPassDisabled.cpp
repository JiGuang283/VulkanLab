#include "render/features/ambient_occlusion/CacaoPass.h"

#include "core/Device.h"
#include "render/graph/RenderGraph.h"
#include "render/graph/RenderResourcePool.h"

namespace vkr {

struct CacaoPass::Impl {};

CacaoPass::CacaoPass(Device &device, const RenderResourcePool &,
                     RendererResourceHandles resourceHandles,
                     CacaoResolution resolution)
    : device_(&device), resourceHandles_(resourceHandles),
      requestedResolution_(resolution) {
    status_.compiled = false;
    status_.supported = false;
    status_.resolution = resolution;
    status_.unavailableReason = "CACAO was not compiled into this build";
}

CacaoPass::~CacaoPass() = default;

void CacaoPass::setup(RenderGraphBuilder &builder,
                      const RenderGraphBuildContext &) const {
    builder.addNode(std::string(name()), RgPassType::External,
                    RgQueueClass::Graphics);
    builder.setActive(false);
}

void CacaoPass::releaseViewportResources() {}

void CacaoPass::onViewportResize(const RenderResourcePool &) {}

void CacaoPass::recordNode(RenderGraphPassContext &, uint32_t,
                           const VisibilityFrame &) {
    status_.active = false;
}

bool CacaoPass::reconfigure(const RenderResourcePool &,
                            CacaoResolution resolution,
                            std::string &error) {
    requestedResolution_ = resolution;
    status_.resolution = resolution;
    error = status_.unavailableReason;
    return false;
}

} // namespace vkr
