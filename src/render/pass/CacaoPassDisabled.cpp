#include "render/pass/CacaoPass.h"

#include "core/Device.h"
#include "render/RenderResourceRegistry.h"

namespace vkr {

struct CacaoPass::Impl {};

CacaoPass::CacaoPass(Device &device, const RenderResourceRegistry &,
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

std::vector<RenderImageUsage> CacaoPass::resourceUsages() const { return {}; }

void CacaoPass::releaseViewportResources() {}

void CacaoPass::onViewportResize(const RenderResourceRegistry &) {}

void CacaoPass::execute(const RenderFrameContext &,
                        const RenderResourceRegistry &,
                        const VisibilityFrame &) {
    status_.active = false;
}

bool CacaoPass::reconfigure(const RenderResourceRegistry &,
                            CacaoResolution resolution,
                            std::string &error) {
    requestedResolution_ = resolution;
    status_.resolution = resolution;
    error = status_.unavailableReason;
    return false;
}

} // namespace vkr
