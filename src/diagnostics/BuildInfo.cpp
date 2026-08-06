#include "BuildInfo.h"

#include <BuildFeatures.h>
#include <BuildInfoGenerated.h>

namespace vkr {

const BuildInfo &currentBuildInfo() {
    static const BuildInfo info{generated::kBuildRevision,
                                generated::kBuildDirty,
                                generated::kBuildConfiguration,
                                generated::kBuildCompiler,
                                generated::kBuildVulkanSdk,
                                generated::kBuildGlslc,
                                {build::kEditorUi,
                                 build::kRuntimeControl,
                                 build::kCapture,
                                 build::kAssetAuthoring,
                                 build::kValidation,
                                 build::kGpuDebugUtils,
                                 build::kGpuProfiling,
                                 build::kTracy,
                                 build::kCacao,
                                 build::kAssetTool,
                                 build::kControlTool,
                                 build::kRenderTest}};
    return info;
}

} // namespace vkr
