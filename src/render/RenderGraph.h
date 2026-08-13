#pragma once

#include "RenderGraphTypes.h"
#include "RenderResourceRegistry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class GpuPassProfiler;
struct GpuPassProfile;
class IRenderPass;
class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;
struct FrameRenderFeatures;
struct VisibilityFrame;

struct RenderGraphBuildContext {
    const FrameRenderFeatures &features;
    const RenderResourceRegistry &resources;
};

struct RenderGraphPassContext {
    const RenderFrameContext &frame;
    const RenderResourceRegistry &resources;
};

struct RenderGraphImageUse {
    RgImageHandle before{};
    RgImageHandle after{};
    RenderImageUsage physical{};
    RgImageSubresource subresource{};
    RgResourceLifetime lifetime = RgResourceLifetime::Transient;
};

struct RenderGraphBufferUse {
    RgBufferHandle before{};
    RgBufferHandle after{};
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = VK_WHOLE_SIZE;
    RgBufferAccess access = RgBufferAccess::StorageRead;
    uint32_t frameSlot = UINT32_MAX;
};

enum class RgImportedImageKind { Swapchain };

struct RenderGraphImportedImageUse {
    RgImportedImageKind kind = RgImportedImageKind::Swapchain;
    RenderImageAccess access = RenderImageAccess::ColorAttachmentWrite;
    VkImageLayout requiredLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    RgImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
};

enum class RgAttachmentKind { Color, Depth, Stencil };

struct RenderGraphAttachment {
    RgAttachmentKind kind = RgAttachmentKind::Color;
    bool importedSwapchain = false;
    RenderImageHandle image{};
    RenderImageHandle resolveImage{};
    RenderImageFrame frame = RenderImageFrame::Current;
    RgImageSubresource subresource{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout resolveLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkResolveModeFlagBits resolveMode = VK_RESOLVE_MODE_NONE;
    VkClearValue clearValue{};
};

struct RenderGraphPassDeclaration {
    RenderGraphPassId id = 0;
    RenderGraphPassId groupId = 0;
    uint32_t registrationIndex = 0;
    uint32_t ownerPassIndex = 0;
    uint32_t localNodeIndex = 0;
    std::string groupName;
    std::string name;
    RgPassType type = RgPassType::Graphics;
    RgQueueClass queue = RgQueueClass::Graphics;
    bool active = true;
    bool sideEffect = false;
    std::vector<RenderGraphImageUse> images;
    std::vector<RenderGraphImportedImageUse> importedImages;
    std::vector<RenderGraphBufferUse> buffers;
    std::vector<RenderGraphAttachment> attachments;
    std::vector<RenderGraphPassId> explicitDependencies;
};

class RenderGraphBuilder {
  public:
    RenderGraphBuilder(const RenderResourceRegistry &resources,
                       const FrameRenderFeatures &features);

    void beginPass(uint32_t registrationIndex, const IRenderPass &pass);
    void addNode(std::string name, RgPassType type,
                 RgQueueClass queue, uint32_t localNodeIndex = 0);
    void useImage(RenderImageUsage usage,
                  RgImageSubresource subresource = {});
    void useBuffer(VkBuffer buffer, RgBufferAccess access,
                   VkDeviceSize offset = 0,
                   VkDeviceSize size = VK_WHOLE_SIZE,
                   uint32_t frameSlot = UINT32_MAX);
    void useSwapchainImage(
        RenderImageAccess access, VkImageLayout requiredLayout,
        VkImageLayout finalLayout,
        RgImageSubresource subresource = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
    void addColorAttachment(
        RenderImageHandle image, RenderImageAccess access,
        VkImageLayout finalLayout, VkAttachmentLoadOp loadOp,
        VkAttachmentStoreOp storeOp, VkClearColorValue clearValue = {},
        RgImageSubresource subresource = {},
        RenderImageHandle resolveImage = {},
        VkImageLayout resolveFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        VkResolveModeFlagBits resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT);
    void addDepthAttachment(
        RenderImageHandle image, RenderImageAccess access,
        VkImageLayout requiredLayout, VkImageLayout finalLayout,
        VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp,
        VkClearDepthStencilValue clearValue = {1.0f, 0},
        RgImageSubresource subresource = {});
    void addSwapchainColorAttachment(
        VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp,
        VkClearColorValue clearValue = {});
    void dependsOn(RenderGraphPassId pass);
    void setSideEffect(bool enabled = true);
    void setActive(bool active);

    const std::vector<RenderGraphPassDeclaration> &passes() const {
        return passes_;
    }

  private:
    const RenderResourceRegistry *resources_ = nullptr;
    const FrameRenderFeatures *features_ = nullptr;
    RenderGraphPassDeclaration *currentPass_ = nullptr;
    const IRenderPass *pendingPass_ = nullptr;
    uint32_t pendingPassIndex_ = 0;
    std::vector<RenderGraphPassDeclaration> passes_;
    std::unordered_map<uint64_t, uint32_t> imageVersions_;
    std::unordered_map<uint64_t, uint32_t> bufferVersions_;
    std::unordered_map<uint64_t, uint32_t> bufferResources_;
    uint32_t nextBufferResource_ = 0;
};

struct RenderGraphDependency {
    uint32_t producer = 0;
    uint32_t consumer = 0;
    uint32_t resource = 0;
};

struct CompiledRenderGraph {
    std::vector<RenderGraphPassDeclaration> passes;
    std::vector<uint32_t> executionOrder;
    std::vector<uint32_t> culledPasses;
    std::vector<RenderGraphDependency> dependencies;
    uint64_t topologyHash = 0;
};

class RenderGraphCompiler {
  public:
    static CompiledRenderGraph compile(
        const std::vector<std::unique_ptr<IRenderPass>> &passes,
        const RenderResourceRegistry &resources,
        const struct FrameRenderFeatures &features);
};

class RenderGraphExecutor {
  public:
    static void execute(
        const CompiledRenderGraph &compiled,
        const std::vector<std::unique_ptr<IRenderPass>> &passes,
        const RenderFrameContext &frame,
        const RenderResourceRegistry &resources,
        const VisibilityFrame &visibility, GpuPassProfiler *profiler,
        std::unordered_map<uint64_t, struct RenderGraphImageState> &states,
        std::unordered_map<uint64_t, struct RenderGraphBufferState> &bufferStates,
        struct RenderGraphDiagnostics &diagnostics);
};

struct RenderGraphImageState {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool initialized = false;
};

struct RenderGraphBufferState {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    bool initialized = false;
};

struct RenderGraphDiagnostics {
    struct Resource {
        uint32_t index = 0;
        std::string name;
        std::string lifetime;
        uint32_t versions = 0;
        uint64_t residentBytes = 0;
        VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        std::vector<std::string> producers;
        std::vector<std::string> consumers;
    };

    struct BufferResource {
        uint32_t index = 0;
        uint64_t nativeHandle = 0;
        std::string name;
        std::string lifetime;
        uint32_t versions = 0;
        uint64_t declaredRangeBytes = 0;
        std::vector<std::string> producers;
        std::vector<std::string> consumers;
    };

    uint64_t topologyHash = 0;
    uint32_t activePasses = 0;
    uint32_t culledPasses = 0;
    uint32_t dependencyEdges = 0;
    uint32_t automaticBarriers = 0;
    uint32_t layoutBarriers = 0;
    uint32_t hazardBarriers = 0;
    uint64_t activeImageBytes = 0;
    uint64_t residentImageBytes = 0;
    std::vector<std::string> executionOrder;
    std::vector<std::string> culledNames;
    std::vector<Resource> resources;
    std::vector<BufferResource> buffers;
};

class RenderGraph {
  public:
    RenderGraph() = default;
    ~RenderGraph();

    RenderGraph(const RenderGraph &) = delete;
    RenderGraph &operator=(const RenderGraph &) = delete;

    void addPass(std::unique_ptr<IRenderPass> pass);
    void compile(const RenderResourceRegistry &resources,
                 const struct FrameRenderFeatures &features);
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility,
                 GpuPassProfiler *profiler);

    void releaseViewportResources();
    void onViewportResize(const RenderResourceRegistry &resources);
    void releaseSwapChainResources();
    void onSwapChainResize(const SwapChain &swapChain);

    std::vector<GpuPassProfile> passProfiles() const;
    const CompiledRenderGraph &compiled() const { return compiled_; }
    const RenderGraphDiagnostics &diagnostics() const {
        return diagnostics_;
    }
    std::string toJson() const;
    std::string toDot() const;

  private:
    std::vector<std::unique_ptr<IRenderPass>> passes_;
    CompiledRenderGraph compiled_{};
    bool compiledValid_ = false;
    uint64_t compiledFeatureKey_ = 0;
    std::unordered_map<uint64_t, CompiledRenderGraph> compiledCache_;
    std::unordered_map<uint64_t, RenderGraphImageState> imageStates_;
    std::unordered_map<uint64_t, RenderGraphBufferState> bufferStates_;
    RenderGraphDiagnostics diagnostics_{};
};

} // namespace vkr
