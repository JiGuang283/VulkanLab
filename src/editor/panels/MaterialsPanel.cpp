#include "MaterialsPanel.h"

#include "editor/EditorWidgets.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace vkr {
namespace {

bool containsIgnoreCase(const std::string &text, const std::string &query) {
    if (query.empty())
        return true;
    std::string foldedText = text;
    std::string foldedQuery = query;
    const auto fold = [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    };
    std::transform(foldedText.begin(), foldedText.end(), foldedText.begin(),
                   fold);
    std::transform(foldedQuery.begin(), foldedQuery.end(),
                   foldedQuery.begin(), fold);
    return foldedText.find(foldedQuery) != std::string::npos;
}

const char *alphaModeName(AlphaMode mode) {
    switch (mode) {
    case AlphaMode::Opaque:
        return "Opaque";
    case AlphaMode::Mask:
        return "Mask";
    case AlphaMode::Blend:
        return "Blend";
    }
    return "Unknown";
}

const char *slotName(MaterialTextureSlot slot) {
    switch (slot) {
    case MaterialTextureSlot::BaseColor:
        return "BaseColor";
    case MaterialTextureSlot::Normal:
        return "Normal";
    case MaterialTextureSlot::MetallicRoughness:
        return "MetallicRoughness";
    case MaterialTextureSlot::Occlusion:
        return "Occlusion";
    case MaterialTextureSlot::Emissive:
        return "Emissive";
    case MaterialTextureSlot::Count:
        break;
    }
    return "Unknown";
}

bool isTransparent(const MaterialParams &params) {
    return params.alphaMode == AlphaMode::Blend ||
           params.transmissionFactor > 0.0f;
}

} // namespace

void MaterialsPanel::draw(const MaterialsPanelSnapshot &snapshot) {
    if (sceneGeneration_ != snapshot.sceneGeneration) {
        sceneGeneration_ = snapshot.sceneGeneration;
        selectedIndex_ = 0;
    }

    ImGui::TextDisabled("GPU binding: %s",
                        materialBindingModeName(snapshot.bindingMode));
    ImGui::TextDisabled("%u / %u materials", snapshot.activeMaterials,
                        snapshot.materialCapacity);
    ImGui::Separator();
    if (!snapshot.sceneLoaded) {
        editor::emptyState("No scene is loaded.");
        return;
    }

    ImGui::SetNextItemWidth(-52.0f);
    ImGui::InputTextWithHint("##MaterialSearch", "Search materials...",
                             search_.data(), search_.size());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", snapshot.materials.size());

    const std::string query = search_.data();
    std::vector<size_t> filtered;
    filtered.reserve(snapshot.materials.size());
    for (size_t index = 0; index < snapshot.materials.size(); ++index) {
        const std::string &name = snapshot.materials[index].params.debugName;
        if (containsIgnoreCase(name.empty() ? "<unnamed>" : name, query) ||
            containsIgnoreCase(
                std::to_string(snapshot.materials[index].sourceIndex), query))
            filtered.push_back(index);
    }
    if (std::find(filtered.begin(), filtered.end(), selectedIndex_) ==
            filtered.end() &&
        !filtered.empty())
        selectedIndex_ = filtered.front();

    const float listHeight =
        std::clamp(ImGui::GetContentRegionAvail().y * 0.35f, 100.0f, 220.0f);
    ImGui::BeginChild("MaterialList", ImVec2(0.0f, listHeight),
                      ImGuiChildFlags_Borders);
    if (filtered.empty())
        editor::emptyState("No matching materials.");
    for (size_t index : filtered) {
        const std::string &name = snapshot.materials[index].params.debugName;
        const std::string label =
            std::to_string(snapshot.materials[index].sourceIndex) + "  " +
                                  (name.empty() ? "<unnamed>" : name);
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::Selectable(label.c_str(), index == selectedIndex_))
            selectedIndex_ = index;
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (selectedIndex_ >= snapshot.materials.size()) {
        editor::emptyState("No material selected.");
        return;
    }
    const MaterialPanelItem &item = snapshot.materials[selectedIndex_];
    const MaterialParams &params = item.params;
    const auto property = [](const char *label) {
        editor::propertyLabel(label);
    };

    if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen) &&
        editor::beginPropertyGrid("SurfaceProperties", 0.46f)) {
        property("Name");
        ImGui::TextWrapped("%s", params.debugName.empty()
                                     ? "<unnamed>"
                                     : params.debugName.c_str());
        property("Index");
        ImGui::Text("%u", item.sourceIndex);
        property("GPU Material Index");
        ImGui::Text("%u", item.gpuMaterialIndex);
        property("Shader Family");
        ImGui::TextUnformatted(item.shaderFamily.c_str());
        property("Alpha Mode");
        ImGui::TextUnformatted(alphaModeName(params.alphaMode));
        property("Alpha Cutoff");
        ImGui::Text("%.3f", params.alphaCutoff);
        property("Double Sided");
        ImGui::TextUnformatted(params.doubleSided ? "true" : "false");
        editor::endPropertyGrid();
    }

    if (ImGui::CollapsingHeader("PBR", ImGuiTreeNodeFlags_DefaultOpen) &&
        editor::beginPropertyGrid("PbrProperties", 0.46f)) {
        property("Base Color");
        ImGui::Text("%.3f %.3f %.3f %.3f", params.baseColorFactor.r,
                    params.baseColorFactor.g, params.baseColorFactor.b,
                    params.baseColorFactor.a);
        property("Metallic / Roughness");
        ImGui::Text("%.3f / %.3f", params.metallicFactor,
                    params.roughnessFactor);
        property("Normal Scale");
        ImGui::Text("%.3f", params.normalScale);
        property("Occlusion Strength / UV");
        ImGui::Text("%.3f / %u", params.occlusionStrength,
                    params.occlusionTexCoord);
        property("Emissive");
        ImGui::Text("%.3f %.3f %.3f x %.3f", params.emissiveFactor.r,
                    params.emissiveFactor.g, params.emissiveFactor.b,
                    params.emissiveStrength);
        property("Transmission");
        ImGui::Text("%.3f", params.transmissionFactor);
        editor::endPropertyGrid();
    }

    if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen) &&
        editor::beginPropertyGrid("TextureProperties", 0.46f)) {
        for (size_t index = 0; index < kMaterialTextureSlotCount; ++index) {
            property(slotName(static_cast<MaterialTextureSlot>(index)));
            if (!item.texturesBound[index])
                ImGui::TextUnformatted("Missing");
            else if (snapshot.bindingMode == MaterialBindingMode::Bindless)
                ImGui::Text("Bound (slot %u)", item.textureSlots[index]);
            else
                ImGui::Text("Bound (fixed binding %zu)", index + 1);
        }
        editor::endPropertyGrid();
    }

    if (ImGui::CollapsingHeader("Derived Render State",
                                ImGuiTreeNodeFlags_DefaultOpen) &&
        editor::beginPropertyGrid("DerivedProperties", 0.46f)) {
        property("Render Queue");
        ImGui::TextUnformatted(isTransparent(params) ? "Transparent"
                                                     : "Opaque");
        property("Cull");
        ImGui::TextUnformatted(params.doubleSided ? "None" : "Back");
        editor::endPropertyGrid();
    }
}

} // namespace vkr
