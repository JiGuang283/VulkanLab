#include "render/FrameGpuData.h"
#include "render/GpuMaterialData.h"
#include "render/RendererShaderPaths.h"
#include "render/ShaderVariant.h"
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

std::filesystem::path shaderFile(std::string_view runtimePath) {
    constexpr std::string_view prefix = "shader/";
    requireShader(runtimePath.substr(0, prefix.size()) == prefix,
                  "runtime shader path is outside shader/: " +
                      std::string(runtimePath));
    return std::filesystem::path(VKL_TEST_SHADER_ROOT) /
           std::filesystem::path(runtimePath.substr(prefix.size()));
}

SpvReflectShaderStageFlagBits expectedStage(std::string_view path) {
    if (path.find(".vert.spv") != std::string_view::npos)
        return SPV_REFLECT_SHADER_STAGE_VERTEX_BIT;
    if (path.find(".frag.spv") != std::string_view::npos)
        return SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT;
    throw std::runtime_error("shader contract: unknown stage for " +
                             std::string(path));
}

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
        {"cameraPosWS", offsetof(vkr::GlobalFrameUbo, cameraPosWS)},
        {"ambientColorIntensity",
         offsetof(vkr::GlobalFrameUbo, ambientColorIntensity)},
        {"lightCounts", offsetof(vkr::GlobalFrameUbo, lightCounts)},
        {"directionalLights",
         offsetof(vkr::GlobalFrameUbo, directionalLights)},
        {"punctualLights", offsetof(vkr::GlobalFrameUbo, punctualLights)},
        {"directionalShadowViewProj",
         offsetof(vkr::GlobalFrameUbo, directionalShadowViewProj)},
        {"shadowParams", offsetof(vkr::GlobalFrameUbo, shadowParams)},
    };
    checkBlockLayout(binding.block, sizeof(vkr::GlobalFrameUbo), members,
                     "GlobalFrameUbo", path);
}

void checkPushConstant(const SpvReflectShaderModule &module,
                       std::string_view path) {
    const auto blocks = enumerateVariables<SpvReflectBlockVariable>(
        module, spvReflectEnumeratePushConstantBlocks, "push constants", path);
    if (blocks.empty()) {
        requireShader(path.find("material_debug/shadow.frag.spv") !=
                              std::string_view::npos ||
                          path.find("postprocess/fullscreen.vert.spv") !=
                              std::string_view::npos,
                      "missing push constant in " + std::string(path));
        return;
    }
    requireShader(blocks.size() == 1,
                  "multiple push constant blocks in " + std::string(path));

    if (path.find("postprocess/tonemap.frag.spv") != std::string_view::npos) {
        static const std::vector<MemberLayout> toneMapMembers = {
            {"exposureEv", offsetof(vkr::ToneMapPushConstants, exposureEv)},
            {"toneMapper", offsetof(vkr::ToneMapPushConstants, toneMapper)},
            {"encodeGamma", offsetof(vkr::ToneMapPushConstants, encodeGamma)},
            {"applyExposure",
             offsetof(vkr::ToneMapPushConstants, applyExposure)},
        };
        checkBlockLayout(*blocks[0], sizeof(vkr::ToneMapPushConstants),
                         toneMapMembers, "ToneMapPushConstants", path);
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

void checkDescriptors(const ReflectedModule &reflected) {
    const auto &module = reflected.module();
    const auto bindings = enumerateVariables<SpvReflectDescriptorBinding>(
        module, spvReflectEnumerateDescriptorBindings, "descriptor bindings",
        reflected.path());
    const bool toneMap = reflected.path().find("postprocess/tonemap.frag.spv") !=
                         std::string::npos;
    const VkShaderStageFlags stage =
        static_cast<VkShaderStageFlags>(module.shader_stage);

    for (const SpvReflectDescriptorBinding *binding : bindings) {
        requireShader(binding->count == 1,
                      "descriptor array is not supported in " +
                          reflected.path());
        if (toneMap) {
            requireShader(
                binding->set == 0 && binding->binding == 0 &&
                    binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                    stage == VK_SHADER_STAGE_FRAGMENT_BIT,
                "ToneMap descriptor contract mismatch in " + reflected.path());
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
        } else if (binding->set == 1 && binding->binding < 5) {
            requireShader(
                binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                    stage == VK_SHADER_STAGE_FRAGMENT_BIT,
                "material descriptor contract mismatch in " + reflected.path());
        } else if (binding->set == 2 && binding->binding == 0) {
            requireShader(
                binding->descriptor_type ==
                        SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                    stage == VK_SHADER_STAGE_FRAGMENT_BIT,
                "shadow descriptor contract mismatch in " + reflected.path());
        } else {
            throw std::runtime_error(
                "shader contract: unexpected set=" +
                std::to_string(binding->set) + " binding=" +
                std::to_string(binding->binding) + " in " + reflected.path());
        }
    }
}

void checkVertexInputs(const ReflectedModule &reflected) {
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
        reflected.path().find("postprocess/fullscreen.vert.spv") !=
        std::string::npos;
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

void checkFragmentOutputs(const ReflectedModule &reflected) {
    if (reflected.module().shader_stage != SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT)
        return;
    const InterfaceMap outputs = userInterface(reflected, false);
    const bool depthOnly =
        reflected.path().find("shadow/depth_mask.frag.spv") != std::string::npos;
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
}

void testShaderContracts() {
    using Program = std::pair<std::string, std::string>;
    std::vector<Program> programs;
    for (const auto &variant : vkr::kShaderVariants)
        programs.emplace_back(variant.vertSpvPath, variant.fragSpvPath);
    programs.emplace_back(std::string(vkr::kShadowVertexShaderPath), "");
    programs.emplace_back(std::string(vkr::kShadowVertexShaderPath),
                          std::string(vkr::kShadowMaskFragmentShaderPath));
    programs.emplace_back(std::string(vkr::kFullscreenVertexShaderPath),
                          std::string(vkr::kToneMapFragmentShaderPath));

    std::map<std::string, std::unique_ptr<ReflectedModule>> modules;
    for (const auto &[vertexPath, fragmentPath] : programs) {
        for (const std::string *path : {&vertexPath, &fragmentPath}) {
            if (path->empty() || modules.find(*path) != modules.end())
                continue;
            auto reflected =
                std::make_unique<ReflectedModule>(*path, shaderFile(*path));
            requireShader(reflected->module().shader_stage == expectedStage(*path),
                          "stage mismatch in " + *path);
            checkDescriptors(*reflected);
            checkPushConstant(reflected->module(), reflected->path());
            checkVertexInputs(*reflected);
            checkFragmentOutputs(*reflected);
            modules.emplace(*path, std::move(reflected));
        }
    }

    for (const auto &[vertexPath, fragmentPath] : programs) {
        const auto &vertex = *modules.at(vertexPath);
        const ReflectedModule *fragment =
            fragmentPath.empty() ? nullptr : modules.at(fragmentPath).get();
        checkProgramInterface(vertex, fragment);
    }
}

} // namespace

void runShaderContractTests() { testShaderContracts(); }
