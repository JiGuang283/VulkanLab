#include "render/graph/RenderResourcePool.h"

#include <stdexcept>
#include <vector>

namespace {

template <typename Fn>
void requireContractFailure(Fn &&fn, const char *message) {
    try {
        fn();
    } catch (const std::runtime_error &) {
        return;
    }
    throw std::runtime_error(message);
}

std::vector<vkr::RenderImageDesc> testDescriptions() {
    vkr::RenderImageDesc shadow{};
    shadow.name = "Shadow";
    shadow.format = VK_FORMAT_D32_SFLOAT;
    shadow.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT;
    shadow.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;

    vkr::RenderImageDesc hdr{};
    hdr.name = "HDR";
    hdr.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    hdr.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    return {shadow, hdr};
}

void testValidExplicitPassContract() {
    const std::vector<vkr::RenderPassResourceUsage> passes = {
        {"Shadow",
         {{{0}, vkr::RenderImageAccess::DepthAttachmentWrite,
           VK_IMAGE_LAYOUT_UNDEFINED,
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}}},
        {"Skybox",
         {{{1}, vkr::RenderImageAccess::ColorAttachmentWrite,
           VK_IMAGE_LAYOUT_UNDEFINED,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}},
        {"Forward",
         {{{0}, vkr::RenderImageAccess::SampledRead,
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
          {{1}, vkr::RenderImageAccess::ColorAttachmentReadWrite,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}},
        {"ToneMap",
         {{{1}, vkr::RenderImageAccess::SampledRead,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}},
    };
    vkr::validateRenderResourceContracts(testDescriptions(), passes);
}

void testAttachmentReadBeforeWriteFails() {
    const std::vector<vkr::RenderPassResourceUsage> passes = {
        {"Forward",
         {{{1}, vkr::RenderImageAccess::ColorAttachmentReadWrite,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}},
    };
    requireContractFailure(
        [&] {
            vkr::validateRenderResourceContracts(testDescriptions(), passes);
        },
        "attachment read-before-write render contract was accepted");
}

void testReadBeforeWriteFails() {
    const std::vector<vkr::RenderPassResourceUsage> passes = {
        {"ToneMap",
         {{{1}, vkr::RenderImageAccess::SampledRead,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}},
    };
    requireContractFailure(
        [&] {
            vkr::validateRenderResourceContracts(testDescriptions(), passes);
        },
        "read-before-write render contract was accepted");
}

void testLayoutMismatchFails() {
    const std::vector<vkr::RenderPassResourceUsage> passes = {
        {"Forward",
         {{{1}, vkr::RenderImageAccess::ColorAttachmentWrite,
           VK_IMAGE_LAYOUT_UNDEFINED,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}},
        {"ToneMap",
         {{{1}, vkr::RenderImageAccess::SampledRead,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}},
    };
    requireContractFailure(
        [&] {
            vkr::validateRenderResourceContracts(testDescriptions(), passes);
        },
        "incompatible render resource layout was accepted");
}

void testMissingUsageFlagFails() {
    auto descriptions = testDescriptions();
    descriptions[1].usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const std::vector<vkr::RenderPassResourceUsage> passes = {
        {"ExternalRead",
         {{{1}, vkr::RenderImageAccess::SampledRead,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}},
    };
    descriptions[1].externallyInitialized = true;
    descriptions[1].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    requireContractFailure(
        [&] {
            vkr::validateRenderResourceContracts(descriptions, passes);
        },
        "missing sampled usage flag was accepted");
}

} // namespace

void runRenderResourcePoolTests() {
    testValidExplicitPassContract();
    testReadBeforeWriteFails();
    testAttachmentReadBeforeWriteFails();
    testLayoutMismatchFails();
    testMissingUsageFlagFails();
}
