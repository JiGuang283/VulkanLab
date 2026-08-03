#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace vkr {

enum class PrimitiveType {
    Plane,
    Cube,
    Sphere,
    Cylinder,
    Cone,
    Capsule,
};

enum class PrimitiveMaterialPreset {
    Default,
    CalibrationWhite,
    CalibrationRed,
    CalibrationGreen,
    CalibrationDark,
    CalibrationEmissive,
};

struct PrimitiveModelDefinition {
    PrimitiveType type;
    std::string_view id;
    std::string_view displayName;
    PrimitiveMaterialPreset materialPreset = PrimitiveMaterialPreset::Default;
};

inline constexpr std::string_view kPrimitiveModelProfileId =
    "engine-primitive-v1";
inline constexpr size_t kPrimitiveModelDefinitionCount = 11;

const std::array<PrimitiveModelDefinition, kPrimitiveModelDefinitionCount> &
primitiveModelDefinitions();
const PrimitiveModelDefinition *findPrimitiveModel(std::string_view id);
const PrimitiveModelDefinition *findPrimitiveModel(PrimitiveType type);
bool isPrimitiveModelId(std::string_view id);

} // namespace vkr
