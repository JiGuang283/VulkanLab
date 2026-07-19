#include "BuildInfo.h"

#include <BuildInfoGenerated.h>

namespace vkr {

const BuildInfo &currentBuildInfo() {
    static const BuildInfo info{generated::kBuildRevision,
                                generated::kBuildDirty,
                                generated::kBuildConfiguration,
                                generated::kBuildCompiler,
                                generated::kBuildVulkanSdk,
                                generated::kBuildGlslc};
    return info;
}

} // namespace vkr
