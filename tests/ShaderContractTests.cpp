#include "render/FrameGpuData.h"
#include "render/GpuMaterialData.h"
#include "render/ShaderRegistry.h"
#include "render/Vertex.h"

#include <spirv_reflect.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using MemberLayout = std::pair<std::string_view, size_t>;

void requireShader(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error("shader contract: " + message);
}

std::string variableName(const char *name) {
    return name ? name : "<unnamed>";
}

class ReflectedModule {
  public:
    ReflectedModule(std::string runtimePath, const std::filesystem::path &path)
        : runtimePath_(std::move(runtimePath)) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        requireShader(file.is_open(), "missing " + path.string());
        const auto size = file.tellg();
        requireShader(size > 0 && size % 4 == 0,
                      runtimePath_ + " is not valid SPIR-V data");
        bytes_.resize(static_cast<size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char *>(bytes_.data()), size);
        requireShader(file.good(), "failed to read " + path.string());

        const SpvReflectResult result = spvReflectCreateShaderModule(
            bytes_.size(), bytes_.data(), &module_);
        requireShader(result == SPV_REFLECT_RESULT_SUCCESS,
                      "reflection failed for " + runtimePath_);
        initialized_ = true;
    }

    ~ReflectedModule() {
        if (initialized_)
            spvReflectDestroyShaderModule(&module_);
    }

    ReflectedModule(const ReflectedModule &) = delete;
    ReflectedModule &operator=(const ReflectedModule &) = delete;

    const std::string &path() const { return runtimePath_; }
    const SpvReflectShaderModule &module() const { return module_; }

  private:
    std::string runtimePath_;
    std::vector<uint8_t> bytes_;
    SpvReflectShaderModule module_{};
    bool initialized_ = false;
};

template <typename Variable, typename Enumerate>
std::vector<Variable *> enumerateVariables(const SpvReflectShaderModule &module,
                                           Enumerate enumerate,
                                           std::string_view label,
                                           std::string_view path) {
    uint32_t count = 0;
    requireShader(enumerate(&module, &count, nullptr) ==
                      SPV_REFLECT_RESULT_SUCCESS,
                  "failed to count " + std::string(label) + " in " +
                      std::string(path));
    std::vector<Variable *> values(count);
    requireShader(enumerate(&module, &count, values.data()) ==
                      SPV_REFLECT_RESULT_SUCCESS,
                  "failed to enumerate " + std::string(label) + " in " +
                      std::string(path));
    values.resize(count);
    return values;
}

bool isUserInterfaceVariable(const SpvReflectInterfaceVariable &variable) {
    return variable.built_in == -1 && variable.location != UINT32_MAX &&
           (variable.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) == 0;
}

void checkBlockLayout(const SpvReflectBlockVariable &block, uint32_t size,
                      const std::vector<MemberLayout> &members,
                      std::string_view label, std::string_view path) {
    requireShader(block.offset == 0 && block.size == size,
                  std::string(label) + " size/offset mismatch in " +
                      std::string(path));
    requireShader(block.member_count == members.size(),
                  std::string(label) + " member count mismatch in " +
                      std::string(path));
    for (const auto &[expectedName, expectedOffset] : members) {
        const auto *member = std::find_if(
            block.members, block.members + block.member_count,
            [expectedName](const SpvReflectBlockVariable &candidate) {
                return candidate.name && candidate.name == expectedName;
            });
        requireShader(member != block.members + block.member_count,
                      std::string(label) + " is missing member " +
                          std::string(expectedName) + " in " +
                          std::string(path));
        requireShader(member->offset == expectedOffset,
                      std::string(label) + "." + std::string(expectedName) +
                          " offset mismatch in " + std::string(path));
    }
}

void checkGlobalUbo(const SpvReflectDescriptorBinding &binding,
                    std::string_view path) {
    static const std::vector<MemberLayout> members = {
        {"view", offsetof(vkr::GlobalFrameUbo, view)},
        {"proj", offsetof(vkr::GlobalFrameUbo, proj)},
        {"inverseViewProjection",
         offsetof(vkr::GlobalFrameUbo, inverseViewProjection)},
        {"cameraPosWS", offsetof(vkr::GlobalFrameUbo, cameraPosWS)},
        {"ambientColorIntensity",
         offsetof(vkr::GlobalFrameUbo, ambientColorIntensity)},
        {"lightCounts", offsetof(vkr::GlobalFrameUbo, lightCounts)},
        {"directionalShadowViewProj",
         offsetof(vkr::GlobalFrameUbo, directionalShadowViewProj)},
        {"shadowParams", offsetof(vkr::GlobalFrameUbo, shadowParams)},
        {"environmentParams",
         offsetof(vkr::GlobalFrameUbo, environmentParams)},
    };
    checkBlockLayout(binding.block, sizeof(vkr::GlobalFrameUbo), members,
                     "GlobalFrameUbo", path);
}

void checkAtmosphereUbo(const SpvReflectDescriptorBinding &binding,
                        std::string_view path) {
    static const std::vector<MemberLayout> members = {
        {"planetCenterBottomRadius",
         offsetof(vkr::AtmosphereGpuParams, planetCenterBottomRadius)},
        {"topRadiusDensityHeights",
         offsetof(vkr::AtmosphereGpuParams, topRadiusDensityHeights)},
        {"rayleighScatteringOzoneHalfWidth",
         offsetof(vkr::AtmosphereGpuParams,
                  rayleighScatteringOzoneHalfWidth)},
        {"mieScatteringExtinction",
         offsetof(vkr::AtmosphereGpuParams, mieScatteringExtinction)},
        {"ozoneAbsorptionAerialStart",
         offsetof(vkr::AtmosphereGpuParams, ozoneAbsorptionAerialStart)},
        {"groundAlbedoDistanceScale",
         offsetof(vkr::AtmosphereGpuParams, groundAlbedoDistanceScale)},
        {"sunDirectionAngularRadius",
         offsetof(vkr::AtmosphereGpuParams, sunDirectionAngularRadius)},
        {"sunColorIntensity",
         offsetof(vkr::AtmosphereGpuParams, sunColorIntensity)},
        {"cameraDistanceParams",
         offsetof(vkr::AtmosphereGpuParams, cameraDistanceParams)},
        {"viewportParams", offsetof(vkr::AtmosphereGpuParams, viewportParams)},
        {"runtimeParams", offsetof(vkr::AtmosphereGpuParams, runtimeParams)},
        {"reserved", offsetof(vkr::AtmosphereGpuParams, reserved)},
    };
    checkBlockLayout(binding.block, sizeof(vkr::AtmosphereGpuParams), members,
                     "AtmosphereGpuParams", path);
}

void checkSurfaceFrameUbo(const SpvReflectDescriptorBinding &binding,
                          std::string_view path) {
    static const std::vector<MemberLayout> members = {
        {"previousViewProjection",
         offsetof(vkr::SurfaceFrameUbo, previousViewProjection)},
        {"viewportSizeInvSize",
         offsetof(vkr::SurfaceFrameUbo, viewportSizeInvSize)},
        {"params", offsetof(vkr::SurfaceFrameUbo, params)},
    };
    checkBlockLayout(binding.block, sizeof(vkr::SurfaceFrameUbo), members,
                     "SurfaceFrameUbo", path);
}

void checkScreenSpaceUbo(const SpvReflectDescriptorBinding &binding,
                         std::string_view path) {
    static const std::vector<MemberLayout> members = {
        {"viewportSizeInvSize",
         offsetof(vkr::ScreenSpaceLightingUbo, viewportSizeInvSize)},
        {"modes", offsetof(vkr::ScreenSpaceLightingUbo, modes)},
    };
    checkBlockLayout(binding.block, sizeof(vkr::ScreenSpaceLightingUbo),
                     members, "ScreenSpaceLightingUbo", path);
}

void checkRenderItemHistoryBuffer(const SpvReflectDescriptorBinding &binding,
                                  std::string_view path) {
    requireShader(binding.block.member_count == 1,
                  "RenderItemHistoryBuffer member count mismatch in " +
                      std::string(path));
    const SpvReflectBlockVariable &items = binding.block.members[0];
    requireShader(variableName(items.name) == "items" &&
                      items.array.dims_count == 1 &&
                      items.array.dims[0] == SPV_REFLECT_ARRAY_DIM_RUNTIME &&
                      items.array.stride == sizeof(vkr::GpuRenderItemHistory),
                  "GpuRenderItemHistory runtime-array mismatch in " +
                      std::string(path));
    static const std::vector<MemberLayout> members = {
        {"previousWorld",
         offsetof(vkr::GpuRenderItemHistory, previousWorld)},
        {"params", offsetof(vkr::GpuRenderItemHistory, params)},
    };
    requireShader(items.member_count == members.size(),
                  "GpuRenderItemHistory member count mismatch in " +
                      std::string(path));
    for (const auto &[expectedName, expectedOffset] : members) {
        const auto *member = std::find_if(
            items.members, items.members + items.member_count,
            [expectedName](const SpvReflectBlockVariable &candidate) {
                return candidate.name && candidate.name == expectedName;
            });
        requireShader(member != items.members + items.member_count &&
                          member->offset == expectedOffset,
                      "GpuRenderItemHistory." + std::string(expectedName) +
                          " layout mismatch in " + std::string(path));
    }
}

void checkSceneLightBuffer(const SpvReflectDescriptorBinding &binding,
                           std::string_view path) {
    requireShader(binding.block.member_count == 1,
                  "SceneLightBuffer member count mismatch in " +
                      std::string(path));
    const SpvReflectBlockVariable &lights = binding.block.members[0];
    requireShader(variableName(lights.name) == "lights",
                  "SceneLightBuffer is missing lights[] in " +
                      std::string(path));
    requireShader(lights.array.dims_count == 1 &&
                      lights.array.dims[0] == SPV_REFLECT_ARRAY_DIM_RUNTIME &&
                      lights.array.stride == sizeof(vkr::GpuLight),
                  "GpuLight runtime-array stride mismatch in " +
                      std::string(path));
    requireShader(lights.member_count == 4,
                  "GpuLight member count mismatch in " + std::string(path));
    static const std::vector<MemberLayout> members = {
        {"positionRange", offsetof(vkr::GpuLight, positionRange)},
        {"directionInnerCos", offsetof(vkr::GpuLight, directionInnerCos)},
        {"colorIntensity", offsetof(vkr::GpuLight, colorIntensity)},
        {"params", offsetof(vkr::GpuLight, params)},
    };
    for (const auto &[expectedName, expectedOffset] : members) {
        const auto *member = std::find_if(
            lights.members, lights.members + lights.member_count,
            [expectedName](const SpvReflectBlockVariable &candidate) {
                return candidate.name && candidate.name == expectedName;
            });
        requireShader(member != lights.members + lights.member_count &&
                          member->offset == expectedOffset,
                      "GpuLight." + std::string(expectedName) +
                          " layout mismatch in " + std::string(path));
    }
}

void checkPushConstant(const SpvReflectShaderModule &module,
                       vkr::ShaderProgramContract contract,
                       bool expectsAtmosphere,
                       std::string_view path) {
    const auto blocks = enumerateVariables<SpvReflectBlockVariable>(
        module, spvReflectEnumeratePushConstantBlocks, "push constants", path);
    if (blocks.empty()) {
        const bool allowed =
            contract == vkr::ShaderProgramContract::Fullscreen ||
            (contract == vkr::ShaderProgramContract::Compute &&
             expectsAtmosphere) ||
            (contract == vkr::ShaderProgramContract::MainForward &&
             module.shader_stage == SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT);
        requireShader(allowed,
                      "missing push constant in " + std::string(path));
        return;
    }
    requireShader(blocks.size() == 1,
                  "multiple push constant blocks in " + std::string(path));

    if (contract == vkr::ShaderProgramContract::Fullscreen) {
        static const std::vector<MemberLayout> toneMapMembers = {
            {"exposureEv", offsetof(vkr::ToneMapPushConstants, exposureEv)},
            {"bloomIntensity",
             offsetof(vkr::ToneMapPushConstants, bloomIntensity)},
            {"toneMapper", offsetof(vkr::ToneMapPushConstants, toneMapper)},
            {"encodeGamma", offsetof(vkr::ToneMapPushConstants, encodeGamma)},
            {"applyExposure",
             offsetof(vkr::ToneMapPushConstants, applyExposure)},
            {"applyBloom", offsetof(vkr::ToneMapPushConstants, applyBloom)},
            {"surfaceDebugMode",
             offsetof(vkr::ToneMapPushConstants, surfaceDebugMode)},
            {"motionDebugScale",
             offsetof(vkr::ToneMapPushConstants, motionDebugScale)},
            {"screenDebugMode",
             offsetof(vkr::ToneMapPushConstants, screenDebugMode)},
            {"screenDebugMip",
             offsetof(vkr::ToneMapPushConstants, screenDebugMip)},
            {"cameraNear", offsetof(vkr::ToneMapPushConstants, cameraNear)},
            {"cameraFar", offsetof(vkr::ToneMapPushConstants, cameraFar)},
        };
        checkBlockLayout(*blocks[0], sizeof(vkr::ToneMapPushConstants),
                         toneMapMembers, "ToneMapPushConstants", path);
        return;
    }
    if (contract == vkr::ShaderProgramContract::Compute) {
        if (path.find("postprocess/bloom_") != std::string_view::npos) {
            static const std::vector<MemberLayout> members = {
                {"threshold", offsetof(vkr::BloomPushConstants, threshold)},
                {"softKnee", offsetof(vkr::BloomPushConstants, softKnee)},
                {"filterRadius",
                 offsetof(vkr::BloomPushConstants, filterRadius)},
                {"applyThreshold",
                 offsetof(vkr::BloomPushConstants, applyThreshold)},
            };
            checkBlockLayout(*blocks[0], sizeof(vkr::BloomPushConstants),
                             members, "BloomPushConstants", path);
        } else if (path.find("visibility/occlusion_cull") !=
                   std::string_view::npos) {
            static const std::vector<MemberLayout> members = {
                {"viewProjection", 0}, {"params", 64}, {"counts", 80}};
            checkBlockLayout(*blocks[0], 96, members,
                             "OcclusionPushConstants", path);
        } else if (path.find("screenspace/ssao_") !=
                   std::string_view::npos) {
            static const std::vector<MemberLayout> members = {
                {"parameters", 0}, {"dimensions", 16}};
            checkBlockLayout(*blocks[0], 32, members, "SsaoPushConstants",
                             path);
        } else {
            static const std::vector<MemberLayout> members = {{"extents", 0}};
            checkBlockLayout(*blocks[0], 16, members,
                             "PyramidPushConstants", path);
        }
        return;
    }

    static const std::vector<MemberLayout> materialMembers = {
        {"model", offsetof(vkr::GpuPushBlock, model)},
        {"baseColorFactor", offsetof(vkr::GpuPushBlock, baseColorFactor)},
        {"emissiveMetallic", offsetof(vkr::GpuPushBlock, emissiveMetallic)},
        {"roughnessAlpha", offsetof(vkr::GpuPushBlock, roughnessAlpha)},
        {"reserved", offsetof(vkr::GpuPushBlock, reserved)},
    };
    checkBlockLayout(*blocks[0], sizeof(vkr::GpuPushBlock), materialMembers,
                     "GpuPushBlock", path);
}

void checkDescriptors(const ReflectedModule &reflected,
                      vkr::ShaderProgramContract contract,
                      bool expectsSceneLights,
                      bool expectsAtmosphere,
                      bool expectsScreenSpace) {
    const auto &module = reflected.module();
    const auto bindings = enumerateVariables<SpvReflectDescriptorBinding>(
        module, spvReflectEnumerateDescriptorBindings, "descriptor bindings",
        reflected.path());
    if (contract == vkr::ShaderProgramContract::Compute) {
        const bool ssao = reflected.path().find("screenspace/ssao_") !=
                          std::string::npos;
        const bool occlusion =
            reflected.path().find("visibility/occlusion_cull") !=
            std::string::npos;
        const uint32_t expectedCount = expectsAtmosphere
                                           ? 6u
                                           : ssao ? 5u
                                                  : occlusion ? 4u : 2u;
        requireShader(bindings.size() == expectedCount,
                      "compute descriptor count mismatch in " +
                          reflected.path());
        for (const SpvReflectDescriptorBinding *binding : bindings) {
            if (expectsAtmosphere) {
                const bool atmosphereUbo =
                    binding->set == 0 && binding->binding == 0 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                const bool atmosphereTexture =
                    binding->set == 0 && binding->binding >= 1 &&
                    binding->binding <= 4 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                const bool destination =
                    binding->set == 1 && binding->binding == 0 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                requireShader(binding->count == 1 &&
                                  (atmosphereUbo || atmosphereTexture ||
                                   destination),
                              "Atmosphere compute descriptor contract "
                              "mismatch in " + reflected.path());
                if (atmosphereUbo)
                    checkAtmosphereUbo(*binding, reflected.path());
                continue;
            }
            if (ssao) {
                const bool globalUbo =
                    binding->set == 0 && binding->binding == 0 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                const bool source =
                    binding->set == 1 && binding->binding < 3 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                const bool destination =
                    binding->set == 1 && binding->binding == 3 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                requireShader(binding->count == 1 &&
                                  (globalUbo || source || destination),
                              "SSAO compute descriptor contract mismatch in " +
                                  reflected.path());
                if (globalUbo)
                    checkGlobalUbo(*binding, reflected.path());
                continue;
            }
            if (occlusion) {
                const bool source =
                    binding->set == 0 && binding->binding == 0 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                const bool buffer =
                    binding->set == 0 && binding->binding >= 1 &&
                    binding->binding <= 3 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                requireShader(binding->count == 1 && (source || buffer),
                              "Occlusion compute descriptor contract mismatch "
                              "in " + reflected.path());
                continue;
            }
            const bool validSource =
                binding->set == 0 && binding->binding == 0 &&
                binding->descriptor_type ==
                    SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            const bool validDestination =
                binding->set == 0 && binding->binding == 1 &&
                binding->descriptor_type ==
                    SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            requireShader(
                binding->count == 1 && (validSource || validDestination),
                "Image pyramid compute descriptor contract mismatch in " +
                    reflected.path());
        }
        return;
    }
    const bool toneMap =
        contract == vkr::ShaderProgramContract::Fullscreen &&
        module.shader_stage == SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT &&
        reflected.path().find("postprocess/tonemap.frag") !=
            std::string::npos;
    const bool present =
        contract == vkr::ShaderProgramContract::Fullscreen &&
        module.shader_stage == SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT &&
        reflected.path().find("postprocess/present.frag") !=
            std::string::npos;
    const VkShaderStageFlags stage =
        static_cast<VkShaderStageFlags>(module.shader_stage);
    bool foundSceneLights = false;
    uint32_t atmosphereBindingCount = 0;
    uint32_t screenSpaceBindingCount = 0;

    for (const SpvReflectDescriptorBinding *binding : bindings) {
        requireShader(binding->count == 1,
                      "descriptor array is not supported in " +
                          reflected.path());
        if (toneMap) {
            requireShader(
                binding->set == 0 && binding->binding < 8 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                    stage == VK_SHADER_STAGE_FRAGMENT_BIT,
                "ToneMap descriptor contract mismatch in " + reflected.path());
            continue;
        }
        if (present) {
            requireShader(
                binding->set == 0 && binding->binding == 0 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                    stage == VK_SHADER_STAGE_FRAGMENT_BIT,
                "Present descriptor contract mismatch in " +
                    reflected.path());
            continue;
        }

        if (binding->set == 0 && binding->binding == 0) {
            requireShader(binding->descriptor_type ==
                              SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                              (stage & ~(VK_SHADER_STAGE_VERTEX_BIT |
                                         VK_SHADER_STAGE_FRAGMENT_BIT)) == 0,
                          "global descriptor contract mismatch in " +
                              reflected.path());
            checkGlobalUbo(*binding, reflected.path());
        } else if (binding->set == 0 && binding->binding == 1) {
            requireShader(
                binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                    stage == VK_SHADER_STAGE_FRAGMENT_BIT,
                "scene light descriptor contract mismatch in " +
                    reflected.path());
            checkSceneLightBuffer(*binding, reflected.path());
            foundSceneLights = true;
        } else if (binding->set == 1 && binding->binding < 5) {
            requireShader(
                binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                    stage == VK_SHADER_STAGE_FRAGMENT_BIT,
                "material descriptor contract mismatch in " + reflected.path());
        } else if (contract == vkr::ShaderProgramContract::SurfacePrepass &&
                   binding->set == 2 && binding->binding == 0) {
            requireShader(
                binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                    stage == VK_SHADER_STAGE_VERTEX_BIT,
                "surface frame descriptor contract mismatch in " +
                    reflected.path());
            checkSurfaceFrameUbo(*binding, reflected.path());
        } else if (contract == vkr::ShaderProgramContract::SurfacePrepass &&
                   binding->set == 2 && binding->binding == 1) {
            requireShader(
                binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                    stage == VK_SHADER_STAGE_VERTEX_BIT,
                "surface history descriptor contract mismatch in " +
                    reflected.path());
            checkRenderItemHistoryBuffer(*binding, reflected.path());
        } else if (binding->set == 2 && binding->binding < 5) {
            requireShader(
                binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                    stage == VK_SHADER_STAGE_FRAGMENT_BIT,
                "lighting descriptor contract mismatch in " +
                    reflected.path());
        } else if (binding->set == 3 && binding->binding < 5) {
            const bool atmosphereUbo =
                binding->binding == 0 &&
                binding->descriptor_type ==
                    SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            const bool atmosphereTexture =
                binding->binding > 0 &&
                binding->descriptor_type ==
                    SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            requireShader(
                (atmosphereUbo || atmosphereTexture) &&
                    stage == VK_SHADER_STAGE_FRAGMENT_BIT,
                "atmosphere descriptor contract mismatch in " +
                    reflected.path());
            if (atmosphereUbo)
                checkAtmosphereUbo(*binding, reflected.path());
            ++atmosphereBindingCount;
        } else if (binding->set == 4 && binding->binding < 2) {
            const bool screenUbo =
                binding->binding == 0 &&
                binding->descriptor_type ==
                    SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            const bool screenAo =
                binding->binding == 1 &&
                binding->descriptor_type ==
                    SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            requireShader((screenUbo || screenAo) &&
                              stage == VK_SHADER_STAGE_FRAGMENT_BIT,
                          "screen-space descriptor contract mismatch in " +
                              reflected.path());
            if (screenUbo)
                checkScreenSpaceUbo(*binding, reflected.path());
            ++screenSpaceBindingCount;
        } else {
            throw std::runtime_error(
                "shader contract: unexpected set=" +
                std::to_string(binding->set) + " binding=" +
                std::to_string(binding->binding) + " in " + reflected.path());
        }
    }
    requireShader(foundSceneLights == expectsSceneLights,
                  expectsSceneLights
                      ? "missing scene light SSBO in " + reflected.path()
                      : "unexpected scene light SSBO in " + reflected.path());
    requireShader((atmosphereBindingCount == 5) == expectsAtmosphere,
                  expectsAtmosphere
                      ? "missing atmosphere descriptor set in " +
                            reflected.path()
                      : "unexpected atmosphere descriptor set in " +
                            reflected.path());
    requireShader((screenSpaceBindingCount == 2) == expectsScreenSpace,
                  expectsScreenSpace
                      ? "missing screen-space descriptor set in " +
                            reflected.path()
                      : "unexpected screen-space descriptor set in " +
                            reflected.path());
}

void checkVertexInputs(const ReflectedModule &reflected,
                       vkr::ShaderProgramContract contract) {
    if (contract == vkr::ShaderProgramContract::Compute)
        return;
    if (reflected.module().shader_stage != SPV_REFLECT_SHADER_STAGE_VERTEX_BIT)
        return;
    const auto inputs = enumerateVariables<SpvReflectInterfaceVariable>(
        reflected.module(), spvReflectEnumerateInputVariables, "vertex inputs",
        reflected.path());
    const auto attributes = vkr::Vertex::getAttributeDescriptions();
    uint32_t userInputCount = 0;
    for (const SpvReflectInterfaceVariable *input : inputs) {
        if (!isUserInterfaceVariable(*input))
            continue;
        ++userInputCount;
        const auto attribute = std::find_if(
            attributes.begin(), attributes.end(), [input](const auto &candidate) {
                return candidate.location == input->location;
            });
        requireShader(attribute != attributes.end(),
                      "vertex location " + std::to_string(input->location) +
                          " is absent from Vertex in " + reflected.path());
        requireShader(attribute->format == static_cast<VkFormat>(input->format),
                      "vertex format mismatch at location " +
                          std::to_string(input->location) + " in " +
                          reflected.path());
    }
    const bool fullscreen =
        contract == vkr::ShaderProgramContract::Fullscreen;
    requireShader(fullscreen ? userInputCount == 0 : userInputCount > 0,
                  "fullscreen/geometry vertex input convention mismatch in " +
                      reflected.path());
}

using InterfaceKey = std::pair<uint32_t, uint32_t>;
using InterfaceMap = std::map<InterfaceKey, SpvReflectFormat>;

InterfaceMap userInterface(const ReflectedModule &reflected, bool inputs) {
    const auto variables = inputs
                               ? enumerateVariables<SpvReflectInterfaceVariable>(
                                     reflected.module(),
                                     spvReflectEnumerateInputVariables, "inputs",
                                     reflected.path())
                               : enumerateVariables<SpvReflectInterfaceVariable>(
                                     reflected.module(),
                                     spvReflectEnumerateOutputVariables, "outputs",
                                     reflected.path());
    InterfaceMap result;
    for (const SpvReflectInterfaceVariable *variable : variables) {
        if (!isUserInterfaceVariable(*variable))
            continue;
        const uint32_t component =
            variable->component == UINT32_MAX ? 0 : variable->component;
        result[{variable->location, component}] = variable->format;
    }
    return result;
}

void checkProgramInterface(const ReflectedModule &vertex,
                           const ReflectedModule *fragment) {
    if (!fragment)
        return;
    const InterfaceMap outputs = userInterface(vertex, false);
    const InterfaceMap inputs = userInterface(*fragment, true);
    for (const auto &[key, format] : inputs) {
        const auto output = outputs.find(key);
        requireShader(output != outputs.end(),
                      "fragment input location " + std::to_string(key.first) +
                          " is not produced by " + vertex.path() + " for " +
                          fragment->path());
        requireShader(output->second == format,
                      "varying format mismatch at location " +
                          std::to_string(key.first) + " between " + vertex.path() +
                          " and " + fragment->path());
    }
}

void checkFragmentOutputs(const ReflectedModule &reflected,
                          vkr::ShaderProgramContract contract) {
    if (contract == vkr::ShaderProgramContract::Compute)
        return;
    if (reflected.module().shader_stage != SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT)
        return;
    const InterfaceMap outputs = userInterface(reflected, false);
    const bool depthOnly =
        contract == vkr::ShaderProgramContract::ShadowDepth;
    if (depthOnly) {
        requireShader(outputs.empty(),
                      "depth-only fragment shader writes a color output");
        return;
    }
    const auto color = outputs.find({0, 0});
    requireShader(color != outputs.end() &&
                      color->second == SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT,
                  "fragment color output contract mismatch in " +
                      reflected.path());
    if (contract == vkr::ShaderProgramContract::SurfacePrepass) {
        const auto motion = outputs.find({1, 0});
        requireShader(outputs.size() == 2 && motion != outputs.end() &&
                          motion->second ==
                              SPV_REFLECT_FORMAT_R32G32_SFLOAT,
                      "surface prepass MRT output contract mismatch in " +
                          reflected.path());
    }
}

void testShaderContracts() {
    const std::filesystem::path shaderRoot =
        std::filesystem::absolute(VKL_TEST_SHADER_ROOT).lexically_normal();
    const vkr::ShaderRegistry registry =
        vkr::ShaderRegistry::load(shaderRoot / "manifest.json");
    std::map<std::string, std::unique_ptr<ReflectedModule>> modules;
    std::map<std::string, vkr::ShaderProgramContract> moduleContracts;
    std::map<std::string, bool> moduleSceneLightContracts;
    std::map<std::string, bool> moduleAtmosphereContracts;
    std::map<std::string, bool> moduleScreenSpaceContracts;
    const auto reflect =
        [&](const std::string &absolutePath, const std::string &sourcePath,
            SpvReflectShaderStageFlagBits expectedStage,
            vkr::ShaderProgramContract contract,
            bool expectsSceneLights,
            bool expectsAtmosphere,
            bool expectsScreenSpace) -> ReflectedModule & {
        const auto existing = modules.find(absolutePath);
        if (existing != modules.end()) {
            requireShader(moduleContracts.at(absolutePath) == contract,
                          "a shader module is shared across incompatible "
                          "contracts: " +
                              sourcePath);
            requireShader(
                moduleSceneLightContracts.at(absolutePath) ==
                    expectsSceneLights,
                "a shader module is shared across incompatible scene-light "
                "contracts: " + sourcePath);
            requireShader(
                moduleAtmosphereContracts.at(absolutePath) ==
                    expectsAtmosphere,
                "a shader module is shared across incompatible atmosphere "
                              "contracts: " + sourcePath);
            requireShader(
                moduleScreenSpaceContracts.at(absolutePath) ==
                    expectsScreenSpace,
                "a shader module is shared across incompatible screen-space "
                "contracts: " + sourcePath);
            return *existing->second;
        }
        const std::string runtimePath = "shader/" + sourcePath + ".spv";
        auto reflected = std::make_unique<ReflectedModule>(
            runtimePath, std::filesystem::path(absolutePath));
        requireShader(reflected->module().shader_stage == expectedStage,
                      "stage mismatch in " + runtimePath);
        checkDescriptors(*reflected, contract, expectsSceneLights,
                         expectsAtmosphere, expectsScreenSpace);
        checkPushConstant(reflected->module(), contract, expectsAtmosphere,
                          reflected->path());
        checkVertexInputs(*reflected, contract);
        checkFragmentOutputs(*reflected, contract);
        ReflectedModule &result = *reflected;
        modules.emplace(absolutePath, std::move(reflected));
        moduleContracts.emplace(absolutePath, contract);
        moduleSceneLightContracts.emplace(absolutePath, expectsSceneLights);
        moduleAtmosphereContracts.emplace(absolutePath, expectsAtmosphere);
        moduleScreenSpaceContracts.emplace(absolutePath,
                                           expectsScreenSpace);
        return result;
    };

    for (const vkr::ShaderProgram &program : registry.programs()) {
        ReflectedModule *vertex = nullptr;
        ReflectedModule *fragment = nullptr;
        if (!program.vertSpvPath.empty()) {
            vertex = &reflect(program.vertSpvPath, program.vertexSourcePath,
                              SPV_REFLECT_SHADER_STAGE_VERTEX_BIT,
                              program.contract, false, false, false);
        }
        if (!program.fragSpvPath.empty()) {
            fragment = &reflect(program.fragSpvPath,
                                program.fragmentSourcePath,
                                SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT,
                                program.contract, program.usesSceneLights,
                                program.usesAtmosphere,
                                program.usesScreenSpace);
        }
        if (!program.computeSpvPath.empty()) {
            (void)reflect(program.computeSpvPath, program.computeSourcePath,
                          SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT,
                          program.contract, false,
                          program.usesAtmosphere, false);
        }
        if (vertex)
            checkProgramInterface(*vertex, fragment);
    }
}

} // namespace

void runShaderContractTests() { testShaderContracts(); }
