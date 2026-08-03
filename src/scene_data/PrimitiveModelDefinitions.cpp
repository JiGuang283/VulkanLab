#include "PrimitiveModelDefinitions.h"

#include <algorithm>

namespace vkr {
namespace {

constexpr std::array<PrimitiveModelDefinition, kPrimitiveModelDefinitionCount>
    kDefinitions{{
    {PrimitiveType::Plane, "vkl-primitive-plane", "Plane"},
    {PrimitiveType::Cube, "vkl-primitive-cube", "Cube"},
    {PrimitiveType::Sphere, "vkl-primitive-sphere", "Sphere"},
    {PrimitiveType::Cylinder, "vkl-primitive-cylinder", "Cylinder"},
    {PrimitiveType::Cone, "vkl-primitive-cone", "Cone"},
    {PrimitiveType::Capsule, "vkl-primitive-capsule", "Capsule"},
    {PrimitiveType::Cube, "vkl-primitive-cube-white",
     "Calibration Cube - White",
     PrimitiveMaterialPreset::CalibrationWhite},
    {PrimitiveType::Cube, "vkl-primitive-cube-red",
     "Calibration Cube - Red", PrimitiveMaterialPreset::CalibrationRed},
    {PrimitiveType::Cube, "vkl-primitive-cube-green",
     "Calibration Cube - Green", PrimitiveMaterialPreset::CalibrationGreen},
    {PrimitiveType::Cube, "vkl-primitive-cube-dark",
     "Calibration Cube - Dark", PrimitiveMaterialPreset::CalibrationDark},
    {PrimitiveType::Cube, "vkl-primitive-cube-emissive",
     "Calibration Cube - Emissive",
     PrimitiveMaterialPreset::CalibrationEmissive},
}};

} // namespace

const std::array<PrimitiveModelDefinition, kPrimitiveModelDefinitionCount> &
primitiveModelDefinitions() {
    return kDefinitions;
}

const PrimitiveModelDefinition *findPrimitiveModel(std::string_view id) {
    const auto found = std::find_if(
        kDefinitions.begin(), kDefinitions.end(),
        [id](const PrimitiveModelDefinition &definition) {
            return definition.id == id;
        });
    return found == kDefinitions.end() ? nullptr : &*found;
}

const PrimitiveModelDefinition *findPrimitiveModel(PrimitiveType type) {
    const auto found = std::find_if(
        kDefinitions.begin(), kDefinitions.end(),
        [type](const PrimitiveModelDefinition &definition) {
            return definition.type == type;
        });
    return found == kDefinitions.end() ? nullptr : &*found;
}

bool isPrimitiveModelId(std::string_view id) {
    return findPrimitiveModel(id) != nullptr;
}

} // namespace vkr
