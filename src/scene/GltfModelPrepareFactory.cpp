#include "GltfModelPrepareFactory.h"

#include "PreparedModelData.h"
#include "SceneLoadTask.h"
#include "render/GltfPreparer.h"

#include <utility>

namespace vkr {

ModelPrepareFactory gltfModelPrepareFactory(
    std::string modelPath, std::optional<CameraPose> cameraOverride) {
    return [modelPath = std::move(modelPath), cameraOverride](
               const SceneLoadContext &loadContext,
               const CancellationToken &cancellation,
               SceneLoadProgress &progress) -> PreparedModelData {
        GltfPreparer::Options options{};
        options.maxTextureSize = loadContext.maxTextureSize;
        options.derivedTextureCachePath =
            loadContext.derivedTextureCachePath;
        options.projectId = loadContext.projectId;
        options.sceneId = loadContext.modelId.empty()
                              ? loadContext.sceneId
                              : loadContext.modelId;
        options.profileId = loadContext.profileId;
        options.textureTranscodeTarget =
            loadContext.textureTranscodeTarget;
        options.requireDerivedTextures =
            loadContext.requireDerivedTextures;
        options.loadStats = loadContext.loadStats;
        options.cameraOverride = cameraOverride;
        return GltfPreparer::prepare(modelPath, options, cancellation,
                                     &progress);
    };
}

} // namespace vkr
