#include "BuildInfo.h"

#include <RuntimeFeatures.h>
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
                                 build::kCacao}};
    return info;
}

} // namespace vkr
