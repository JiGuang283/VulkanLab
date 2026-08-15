#pragma once

#include "render/GpuMaterialData.h"
#include "core/MaterialBindingMode.h"
#include "render/MaterialTextureSlot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Buffer;
class DescriptorAllocator;
class Device;
class Texture;
struct MaterialParams;

struct TextureSlotHandle {
    uint32_t index = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
};

struct MaterialHandle {
    uint32_t index = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
};

struct MaterialBindingStatus {
    MaterialBindingMode requested = MaterialBindingMode::Auto;
    MaterialBindingMode active = MaterialBindingMode::Legacy;
    bool deviceSupported = false;
    bool shaderManifestSupported = false;
    uint32_t textureCapacity = 0;
    uint32_t materialCapacity = 0;
    std::string fallbackReason;
    uint32_t activeTextures = 0;
    uint32_t activeMaterials = 0;
    uint32_t retiringTextures = 0;
    uint32_t retiringMaterials = 0;
    uint32_t textureHighWaterMark = 0;
    uint32_t materialHighWaterMark = 0;
    uint64_t descriptorWrites = 0;
    uint64_t textureSlotReuses = 0;
    uint64_t materialSlotReuses = 0;
    uint64_t textureCapacityFailures = 0;
    uint64_t materialCapacityFailures = 0;
};

class MaterialSystem {
  public:
    MaterialSystem(Device &device, DescriptorAllocator &descriptorAllocator,
                   MaterialBindingMode requested,
                   bool shaderManifestSupported);
    ~MaterialSystem();

    MaterialSystem(const MaterialSystem &) = delete;
    MaterialSystem &operator=(const MaterialSystem &) = delete;

    MaterialHandle registerMaterial(
        const MaterialParams &params,
        const MaterialTextureSlotArray<std::shared_ptr<Texture>> &textures,
        std::string_view debugName = {});
    void releaseMaterial(MaterialHandle handle);
    void updateSubmissionSerials(uint64_t lastSubmitted,
                                 uint64_t completed);
    void collectGarbage(uint64_t completedSerial);

    void bindGlobal(VkCommandBuffer commandBuffer,
                    VkPipelineLayout layout) const;
    void bindMaterial(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                      MaterialHandle handle) const;

    std::shared_ptr<Texture> fallbackTexture(MaterialTextureSlot slot) const;
    VkDescriptorSetLayout descriptorSetLayout() const { return layout_; }
    MaterialBindingMode activeMode() const { return status_.active; }
    const MaterialBindingStatus &status() const { return status_; }
    const GpuMaterial *gpuMaterial(MaterialHandle handle) const;

  private:
    struct TextureRecord {
        uint32_t generation = 0;
        uint32_t references = 0;
        bool active = false;
        bool retiring = false;
        uint64_t retireAfterSerial = 0;
        std::shared_ptr<Texture> texture;
    };
    struct MaterialRecord {
        uint32_t generation = 0;
        bool active = false;
        bool retiring = false;
        uint64_t retireAfterSerial = 0;
        VkDescriptorSet legacySet = VK_NULL_HANDLE;
        MaterialTextureSlotArray<TextureSlotHandle> textures{};
    };

    void createLayouts();
    void createBindlessSet();
    void createFallbackResources();
    TextureSlotHandle acquireTexture(const std::shared_ptr<Texture> &texture);
    void releaseTexture(TextureSlotHandle handle, uint64_t retireSerial);
    uint32_t allocateTextureIndex();
    uint32_t allocateMaterialIndex();
    VkDescriptorSet createLegacySet(
        uint32_t materialIndex,
        const MaterialTextureSlotArray<std::shared_ptr<Texture>> &textures,
        std::string_view debugName);
    void writeMaterial(uint32_t index, const GpuMaterial &material);
    void writeBindlessTexture(uint32_t index, const Texture &texture);

    Device *device_ = nullptr;
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    MaterialBindingStatus status_{};
    std::unique_ptr<Buffer> materialBuffer_;
    GpuMaterial *mappedMaterials_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bindlessPool_ = VK_NULL_HANDLE;
    VkDescriptorSet bindlessSet_ = VK_NULL_HANDLE;
    std::vector<TextureRecord> textureRecords_;
    std::vector<MaterialRecord> materialRecords_;
    std::vector<uint32_t> freeTextureSlots_;
    std::vector<uint32_t> freeMaterialSlots_;
    std::unordered_map<const Texture *, uint32_t> textureLookup_;
    std::array<std::shared_ptr<Texture>, 4> fallbackTextures_{};
    uint64_t lastSubmittedSerial_ = 0;
    uint64_t completedSerial_ = 0;
};

} // namespace vkr
