#pragma once

#include "render/shader/ShaderTypes.h"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace vkr {

class ShaderRegistry;

class MaterialShaderFamilyRegistry {
  public:
    MaterialShaderFamilyHandle defaultFamily() const;
    MaterialShaderFamilyHandle find(std::string_view id) const;
    const MaterialShaderFamily &get(MaterialShaderFamilyHandle handle) const;
    const std::vector<MaterialShaderFamily> &families() const {
        return families_;
    }

  private:
    friend class ShaderRegistry;
    std::vector<MaterialShaderFamily> families_;
    size_t defaultFamilyIndex_ = 0;
};

class ShaderRegistry {
  public:
    static constexpr uint32_t kSchemaVersion = 3;
    static constexpr uint32_t kLegacySchemaVersion = 2;

    bool supportsBindlessMaterials() const;

    static ShaderRegistry load(const std::filesystem::path &manifestPath);

    const ViewMode &defaultViewMode() const;
    const ViewMode *findViewMode(std::string_view idOrDisplayName) const;
    MaterialShaderFamilyHandle defaultMaterialShaderFamily() const;
    MaterialShaderFamilyHandle
    findMaterialShaderFamily(std::string_view id) const;
    const MaterialShaderFamily &
    materialShaderFamily(MaterialShaderFamilyHandle handle) const;
    const MaterialShaderFamilyRegistry &materialShaders() const {
        return materialShaders_;
    }
    const ShaderProgram &materialProgram(MaterialShaderFamilyHandle family,
                                         MaterialShaderPass pass,
                                         const ViewMode *viewMode = nullptr) const;
    const ShaderProgram *findProgram(std::string_view id) const;
    const ShaderProgram &program(std::string_view id) const;

    const std::vector<ViewMode> &viewModes() const { return viewModes_; }
    const std::vector<MaterialShaderFamily> &materialShaderFamilies() const {
        return materialShaders_.families();
    }
    const std::vector<ShaderProgram> &programs() const { return programs_; }
    const std::filesystem::path &manifestPath() const { return manifestPath_; }
    std::vector<std::filesystem::path> spirvPaths() const;

  private:
    std::filesystem::path manifestPath_;
    std::vector<ShaderProgram> programs_;
    MaterialShaderFamilyRegistry materialShaders_;
    std::vector<ViewMode> viewModes_;
    size_t defaultViewModeIndex_ = 0;
};

const char *shaderProgramContractName(ShaderProgramContract contract);
const char *shaderToneMappingPolicyName(ShaderToneMappingPolicy policy);
const char *materialShaderPassName(MaterialShaderPass pass);

} // namespace vkr
