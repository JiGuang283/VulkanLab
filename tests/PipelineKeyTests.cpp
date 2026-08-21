#include "render/pipeline/PipelineKey.h"

#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace {

void requirePipelineKey(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Handle>
Handle fakeHandle(uintptr_t value) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(value);
    } else {
        return static_cast<Handle>(value);
    }
}

vkr::PipelineConfig baseConfig() {
    vkr::PipelineConfig config{};
    config.vertShaderPath = "base.vert.spv";
    config.fragShaderPath = "base.frag.spv";
    config.vertexLayout.bindings = {
        {0, 64, VK_VERTEX_INPUT_RATE_VERTEX}};
    config.vertexLayout.attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}};
    config.descriptorLayouts = {fakeHandle<VkDescriptorSetLayout>(3)};
    config.pushConstants = {
        {VK_SHADER_STAGE_VERTEX_BIT, 0, 64}};
    return config;
}

vkr::PipelineRenderingSignature baseSignature() {
    vkr::PipelineRenderingSignature signature;
    signature.colorAttachmentFormats = {VK_FORMAT_R16G16B16A16_SFLOAT};
    signature.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    return signature;
}

template <typename Mutate>
void requireMutationChangesKey(Mutate mutate, const char *message) {
    const vkr::PipelineRenderingSignature signature = baseSignature();
    const vkr::PipelineKey original{signature, baseConfig()};
    vkr::PipelineConfig changed = baseConfig();
    mutate(changed);
    const vkr::PipelineKey mutated{signature, std::move(changed)};
    requirePipelineKey(!(original == mutated), message);
}

void testEquivalentConfigsShareIdentity() {
    const vkr::PipelineKey first{baseSignature(), baseConfig()};
    const vkr::PipelineKey second{baseSignature(), baseConfig()};
    requirePipelineKey(first == second,
                       "equivalent pipeline configs have different keys");
    requirePipelineKey(vkr::PipelineKeyHash{}(first) ==
                           vkr::PipelineKeyHash{}(second),
                       "equivalent pipeline configs have different hashes");

    vkr::PipelineRenderingSignature otherSignature = baseSignature();
    otherSignature.colorAttachmentFormats = {VK_FORMAT_R8G8B8A8_UNORM};
    const vkr::PipelineKey otherRendering{std::move(otherSignature),
                                          baseConfig()};
    requirePipelineKey(
        !(first == otherRendering),
        "dynamic rendering signature was omitted from pipeline identity");

    vkr::PipelineConfig renamedConfig = baseConfig();
    renamedConfig.debugName = "Pipeline/DiagnosticsOnly";
    const vkr::PipelineKey renamed{baseSignature(),
                                   std::move(renamedConfig)};
    requirePipelineKey(first == renamed,
                       "diagnostic pipeline name changed cache identity");
    requirePipelineKey(vkr::PipelineKeyHash{}(first) ==
                           vkr::PipelineKeyHash{}(renamed),
                       "diagnostic pipeline name changed cache hash");
}

void testAllImmutableStateParticipates() {
    requireMutationChangesKey(
        [](auto &c) { c.vertShaderPath = "other.vert.spv"; },
        "vertex shader was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.fragShaderPath = "other.frag.spv"; },
        "fragment shader was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.vertexLayout.bindings[0].stride = 80; },
        "vertex binding was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) {
            c.vertexLayout.attributes[0].format =
                VK_FORMAT_R32G32B32A32_SFLOAT;
        },
        "vertex attribute was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.polygonMode = VK_POLYGON_MODE_LINE; },
        "polygon mode was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.cullMode = VK_CULL_MODE_NONE; },
        "cull mode was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.frontFace = VK_FRONT_FACE_CLOCKWISE; },
        "front face was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; },
        "topology was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.depthTest = false; },
        "depth test was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.depthWrite = false; },
        "depth write was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.depthCompare = VK_COMPARE_OP_GREATER; },
        "depth compare was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.depthBiasEnable = true; },
        "depth bias was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.colorBlendAttachments[0].blendEnable = true; },
        "blend state was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) {
            c.colorBlendAttachments[0].srcColorBlendFactor =
                VK_BLEND_FACTOR_ONE;
        },
        "blend factors were omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.msaaSamples = VK_SAMPLE_COUNT_4_BIT; },
        "sample count was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.subpass = 1; },
        "subpass was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) {
            c.descriptorLayouts[0] =
                fakeHandle<VkDescriptorSetLayout>(4);
        },
        "descriptor layout was omitted from pipeline identity");
    requireMutationChangesKey(
        [](auto &c) { c.pushConstants[0].size = 128; },
        "push constant range was omitted from pipeline identity");
}

} // namespace

void runPipelineKeyTests() {
    testEquivalentConfigsShareIdentity();
    testAllImmutableStateParticipates();
}
