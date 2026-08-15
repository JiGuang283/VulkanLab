#include "ModelSourceResolver.h"

#include "GltfModelPrepareFactory.h"
#include "PrimitiveModelFactory.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "scene_data/PrimitiveModelDefinitions.h"

#include <filesystem>

namespace vkr {

std::optional<ResolvedModelSource>
resolveModelSource(const SceneCatalog &catalog,
                   const ProjectContext &projectContext,
                   const ModelAssetId &modelId) {
    if (const PrimitiveModelDefinition *primitive =
            findPrimitiveModel(modelId.value())) {
        ResolvedModelSource result;
        result.kind = ModelSourceKind::Primitive;
        result.id = modelId;
        result.displayName = std::string(primitive->displayName);
        result.profileId = std::string(kPrimitiveModelProfileId);
        result.prepareFactory = primitiveModelPrepareFactory(*primitive);
        result.instanceable = true;
        result.available = true;
        return result;
    }

    const CatalogModel *model = catalog.findModel(modelId.value());
    if (!model)
        return std::nullopt;

    ResolvedModelSource result;
    result.id = modelId;
    result.displayName = model->displayName;
    result.profileId = model->importProfile;
    result.kind = ModelSourceKind::Gltf;
    result.instanceable = true;
    result.textureLimit = catalog.profile(model->importProfile).textureLimit;
    result.sourcePath = projectContext.resolveProjectPath(model->source);
    result.available = std::filesystem::is_regular_file(result.sourcePath);
    if (result.available) {
        result.prepareFactory = gltfModelPrepareFactory(
            result.sourcePath.string(), model->previewCamera);
    } else {
        result.unavailableReason = "Source file is missing: " +
                                   result.sourcePath.string();
    }
    return result;
}

} // namespace vkr
