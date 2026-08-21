#pragma once

#include "core/MaterialBindingMode.h"
#include "render/material/MaterialInstance.h"
#include "render/material/MaterialTextureSlot.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vkr {

struct MaterialPanelItem {
    uint32_t sourceIndex = 0;
    MaterialParams params;
    uint32_t gpuMaterialIndex = 0;
    std::string shaderFamily;
    std::array<bool, kMaterialTextureSlotCount> texturesBound{};
    std::array<uint32_t, kMaterialTextureSlotCount> textureSlots{};
};

struct MaterialsPanelSnapshot {
    uint64_t sceneGeneration = 0;
    MaterialBindingMode bindingMode = MaterialBindingMode::Legacy;
    uint32_t activeMaterials = 0;
    uint32_t materialCapacity = 0;
    bool sceneLoaded = false;
    std::vector<MaterialPanelItem> materials;
};

class MaterialsPanel {
  public:
    void draw(const MaterialsPanelSnapshot &snapshot);

  private:
    std::array<char, 128> search_{};
    size_t selectedIndex_ = 0;
    uint64_t sceneGeneration_ = UINT64_MAX;
};

} // namespace vkr
