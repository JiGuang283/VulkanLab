#pragma once

#include "render/ShaderVariant.h"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace vkr {

class ShaderRegistry {
  public:
    static constexpr uint32_t kSchemaVersion = 1;

    static ShaderRegistry load(const std::filesystem::path &manifestPath);

    const ShaderVariant &defaultVariant() const;
    const ShaderVariant *findVariant(std::string_view idOrDisplayName) const;
    const ShaderProgram *findProgram(std::string_view id) const;
    const ShaderProgram &program(std::string_view id) const;

    const std::vector<ShaderVariant> &variants() const { return variants_; }
    const std::vector<ShaderProgram> &programs() const { return programs_; }
    const std::filesystem::path &manifestPath() const { return manifestPath_; }
    std::vector<std::filesystem::path> spirvPaths() const;

  private:
    std::filesystem::path manifestPath_;
    std::vector<ShaderProgram> programs_;
    std::vector<ShaderVariant> variants_;
    size_t defaultVariantIndex_ = 0;
};

const char *shaderProgramContractName(ShaderProgramContract contract);
const char *shaderToneMappingPolicyName(ShaderToneMappingPolicy policy);

} // namespace vkr
