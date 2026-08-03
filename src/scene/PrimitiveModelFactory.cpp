#include "PrimitiveModelFactory.h"

#include "PrimitiveMeshGenerator.h"
#include "PreparedModelData.h"
#include "SceneLoadTask.h"

#include <stdexcept>

namespace vkr {
namespace {

void applyMaterialPreset(PrimitiveMaterialPreset preset,
                         MaterialParams &params) {
    params.metallicFactor = 0.0f;
    params.roughnessFactor = 0.7f;

    switch (preset) {
    case PrimitiveMaterialPreset::Default:
        params.baseColorFactor = glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
        params.roughnessFactor = 0.5f;
        break;
    case PrimitiveMaterialPreset::CalibrationWhite:
        params.baseColorFactor =
            glm::vec4(0.725f, 0.71f, 0.68f, 1.0f);
        break;
    case PrimitiveMaterialPreset::CalibrationRed:
        params.baseColorFactor =
            glm::vec4(0.63f, 0.065f, 0.05f, 1.0f);
        break;
    case PrimitiveMaterialPreset::CalibrationGreen:
        params.baseColorFactor =
            glm::vec4(0.14f, 0.45f, 0.091f, 1.0f);
        break;
    case PrimitiveMaterialPreset::CalibrationDark:
        params.baseColorFactor =
            glm::vec4(0.035f, 0.035f, 0.035f, 1.0f);
        params.roughnessFactor = 0.9f;
        break;
    case PrimitiveMaterialPreset::CalibrationEmissive:
        params.baseColorFactor =
            glm::vec4(0.03f, 0.025f, 0.02f, 1.0f);
        params.emissiveFactor = glm::vec3(1.0f, 0.72f, 0.38f);
        params.emissiveStrength = 20.0f;
        params.roughnessFactor = 0.6f;
        break;
    }
}

} // namespace

ModelPrepareFactory
primitiveModelPrepareFactory(PrimitiveModelDefinition definition) {
    return [definition](const SceneLoadContext &,
                        const CancellationToken &cancel,
                        SceneLoadProgress &progress) {
        if (cancel.cancelled())
            throw std::runtime_error("Primitive model preparation cancelled");

        progress.totalTextures = 0;
        progress.totalMeshes = 1;

        PreparedModelData prepared;
        prepared.sourcePath = "engine://" + std::string(definition.id);
        prepared.meshes.push_back(generatePrimitiveMesh(definition.type));

        PreparedMaterial material;
        material.params.debugName = std::string(definition.displayName) +
                                    " Material";
        applyMaterialPreset(definition.materialPreset, material.params);
        prepared.materials.push_back(std::move(material));
        prepared.primitives.push_back({0, 0, glm::mat4(1.0f)});

        progress.completedMeshes = 1;
        progress.processedBytes =
            prepared.meshes.front().vertices.size() * sizeof(Vertex) +
            prepared.meshes.front().indices.size() * sizeof(uint32_t);
        return prepared;
    };
}

} // namespace vkr
