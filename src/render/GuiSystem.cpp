#include "GuiSystem.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/VulkanCheck.h"
#include "editor/EditorTheme.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <stdexcept>
#include <cstring>
#include <string>

namespace vkr {

static void imguiCheckVkResult(VkResult err) {
    if (err != VK_SUCCESS)
        throw std::runtime_error("ImGui Vulkan backend returned error");
}

GuiSystem::GuiSystem(VkInstance instance, Device &device,
                     VkFormat colorFormat, GLFWwindow *window,
                     uint32_t minImageCount, uint32_t imageCount)
    : device_(&device) {
    // --- descriptor pool just for ImGui ---
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 100;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 100;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(device_->logicalDevice(), &poolInfo,
                                    nullptr, &descriptorPool_));
    device_->debugUtils().setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_POOL,
                                        descriptorPool_,
                                        "ImGui/DescriptorPool");

    // --- ImGui context ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    editor::applyEditorTheme(window);

    // --- GLFW backend (install_callbacks=true chains existing callbacks) ---
    ImGui_ImplGlfw_InitForVulkan(window, true);

    // --- Vulkan backend (post-2025/09/26 API: RenderPass in PipelineInfoMain)
    // ---
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = instance;
    initInfo.PhysicalDevice = device.physicalDevice();
    initInfo.Device = device.logicalDevice();
    initInfo.QueueFamily = device.queueFamilies().graphicsFamily.value();
    initInfo.Queue = device.graphicsQueue();
    initInfo.DescriptorPool = descriptorPool_;
    initInfo.MinImageCount = minImageCount;
    initInfo.ImageCount = imageCount;
    initInfo.CheckVkResultFn = imguiCheckVkResult;

    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo
        .colorAttachmentCount = 1;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo
        .pColorAttachmentFormats = &colorFormat;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&initInfo))
        throw std::runtime_error("ImGui_ImplVulkan_Init failed");
    // Modern backend uploads the font atlas on first use; no manual call
    // needed.
}

GuiSystem::~GuiSystem() {
    if (device_)
        vkDeviceWaitIdle(device_->logicalDevice());
    clearViewportTextures();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (descriptorPool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device_->logicalDevice(), descriptorPool_,
                                nullptr);
}

void GuiSystem::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void GuiSystem::render(VkCommandBuffer cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void GuiSystem::discardFrame() {
    ImGui::EndFrame();
}

void GuiSystem::onSwapChainRecreated(uint32_t minImageCount) {
    ImGui_ImplVulkan_SetMinImageCount(minImageCount);
}

bool GuiSystem::wantCaptureMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

bool GuiSystem::wantCaptureKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

void GuiSystem::setViewportTextures(
    VkSampler sampler,
    const std::array<VkImageView, MAX_FRAMES_IN_FLIGHT> &imageViews) {
    clearViewportTextures();
    for (uint32_t frame = 0; frame < viewportTextureSets_.size(); ++frame) {
        viewportTextureSets_[frame] = ImGui_ImplVulkan_AddTexture(
            sampler, imageViews[frame],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_DESCRIPTOR_SET, viewportTextureSets_[frame],
            "ImGui/ViewportTexture/Frame" + std::to_string(frame));
    }
}

void GuiSystem::clearViewportTextures() {
    for (VkDescriptorSet &set : viewportTextureSets_) {
        if (set != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(set);
            set = VK_NULL_HANDLE;
        }
    }
}

uint64_t GuiSystem::viewportTextureId(uint32_t frameIndex) const {
    if (frameIndex >= viewportTextureSets_.size())
        return 0;
    uint64_t id = 0;
    const VkDescriptorSet set = viewportTextureSets_[frameIndex];
    static_assert(sizeof(set) <= sizeof(id));
    std::memcpy(&id, &set, sizeof(set));
    return id;
}

} // namespace vkr
