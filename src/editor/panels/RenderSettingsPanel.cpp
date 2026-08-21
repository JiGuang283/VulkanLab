#include "RenderSettingsPanel.h"

#include "editor/EditorWidgets.h"
#include "render/features/shadows_visibility/DirectionalShadow.h"
#include "render/features/shadows_visibility/PunctualShadow.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/gtc/constants.hpp>

namespace vkr {
namespace {

const char *textureLimitLabel(uint32_t limit) {
    switch (limit) {
    case 0:
        return "Full";
    case 512:
        return "512";
    case 1024:
        return "1024";
    case 2048:
        return "2048";
    default:
        return "Custom";
    }
}

const char *renderPathRequestLabel(RenderPathRequest request) {
    switch (request) {
    case RenderPathRequest::Auto:
        return "Auto";
    case RenderPathRequest::Forward:
        return "Forward";
    case RenderPathRequest::Deferred:
        return "Deferred";
    }
    return "Unknown";
}

const char *renderPathModeLabel(RenderPathMode mode) {
    switch (mode) {
    case RenderPathMode::Forward:
        return "Forward";
    case RenderPathMode::Deferred:
        return "Deferred";
    }
    return "Unknown";
}

const char *lightTypeName(LightType type) {
    switch (type) {
    case LightType::Directional:
        return "Directional";
    case LightType::Point:
        return "Point";
    case LightType::Spot:
        return "Spot";
    }
    return "Unknown";
}

const char *lightIntensityUnit(LightType type) {
    return type == LightType::Directional ? "lux" : "candela";
}

glm::vec3 normalizedSunDirectionOrDefault(const glm::vec3 &direction) {
    const float lengthSquared = glm::dot(direction, direction);
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8f)
        return glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f));
    return direction / std::sqrt(lengthSquared);
}

void sunAnglesFromDirection(const glm::vec3 &direction, float &azimuth,
                            float &elevation) {
    const glm::vec3 normalized = normalizedSunDirectionOrDefault(direction);
    azimuth = std::atan2(normalized.y, normalized.x);
    elevation = std::asin(std::clamp(normalized.z, -1.0f, 1.0f));
}

glm::vec3 sunDirectionFromAngles(float azimuth, float elevation) {
    const float horizontalLength = std::cos(elevation);
    return {horizontalLength * std::cos(azimuth),
            horizontalLength * std::sin(azimuth), std::sin(elevation)};
}

void applyPatch(const RenderSettingsPanelActions &actions,
                const RenderSettingsPatch &patch) {
    if (actions.applySettings)
        actions.applySettings(patch);
}

} // namespace

void RenderSettingsPanel::draw(
    const RenderSettingsPanelSnapshot &snapshot,
    const RenderSettingsPanelActions &actions) const {
    bool advanced = snapshot.advanced;
    if (ImGui::Checkbox("Advanced", &advanced) && actions.setAdvanced)
        actions.setAdvanced(advanced);
    ImGui::Separator();
    ImGui::PushItemWidth(-145.0f);
    if (ImGui::BeginTabBar("RenderSettingsPages")) {
        if (ImGui::BeginTabItem("Output")) {
            ImGui::PushID("Output");
            drawOutput(snapshot, actions);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lighting")) {
            ImGui::PushID("Lighting");
            drawLighting(snapshot, actions);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Effects")) {
            ImGui::PushID("Effects");
            drawEffects(snapshot, actions);
            if (advanced) {
                ImGui::SeparatorText("Surface Data");
                drawSurfaceData(snapshot, actions);
            }
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Visibility")) {
            ImGui::PushID("Visibility");
            drawVisibility(snapshot, actions);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Camera")) {
            ImGui::PushID("Camera");
            drawCamera(snapshot, actions);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
}

void RenderSettingsPanel::drawOutput(
    const RenderSettingsPanelSnapshot &snapshot,
    const RenderSettingsPanelActions &actions) const {
    const RenderPathSelection &renderPath = snapshot.features.renderPath;
    if (ImGui::BeginCombo(
            "Render Path",
            renderPathRequestLabel(snapshot.features.requested.renderPath))) {
        constexpr std::array requests = {
            RenderPathRequest::Auto,
            RenderPathRequest::Forward,
            RenderPathRequest::Deferred,
        };
        for (const RenderPathRequest request : requests) {
            const bool supported =
                request != RenderPathRequest::Deferred ||
                (renderPath.capabilities.deferred &&
                 renderPath.viewModeCompatible);
            const bool selected =
                request == snapshot.features.requested.renderPath;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(renderPathRequestLabel(request), selected) &&
                !selected) {
                RenderSettingsPatch patch;
                patch.renderPath = request;
                applyPatch(actions, patch);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
            if (!supported && ImGui::IsItemHovered(
                                  ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "%s",
                    renderPath.capabilities.deferred
                        ? "The active View Mode is Forward-only."
                        : "Deferred rendering is unavailable on this device.");
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Active: %s", renderPathModeLabel(renderPath.active));
    if (!renderPath.fallbackReason.empty()) {
        editor::statusIndicator("Render path fallback",
                                editor::StatusTone::Warning,
                                renderPath.fallbackReason.c_str());
    }

    if (snapshot.viewModes && !snapshot.viewModes->empty()) {
        if (ImGui::BeginCombo("View Mode",
                              snapshot.currentViewModeDisplayName.c_str())) {
            const auto drawCategory = [&](const char *category,
                                          const char *displayName) {
                bool hasModes = false;
                for (const ViewMode &viewMode : *snapshot.viewModes)
                    hasModes |= viewMode.category == category;
                if (!hasModes)
                    return;
                ImGui::SeparatorText(displayName);
                for (const ViewMode &viewMode : *snapshot.viewModes) {
                    if (viewMode.category != category)
                        continue;
                    const bool selected =
                        viewMode.id == snapshot.currentViewModeId;
                    const bool compatible =
                        snapshot.features.requested.renderPath !=
                            RenderPathRequest::Deferred ||
                        viewMode.supportsDeferred;
                    ImGui::BeginDisabled(!compatible);
                    if (ImGui::Selectable(viewMode.displayName.c_str(),
                                          selected) &&
                        actions.setViewMode)
                        actions.setViewMode(viewMode.id);
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                    if (!compatible && ImGui::IsItemHovered(
                                           ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(
                            "This View Mode requires the Forward path.");
                    ImGui::EndDisabled();
                }
            };
            drawCategory("legacy", "Legacy");
            drawCategory("pbr", "PBR");
            drawCategory("debug", "Debug");
            drawCategory("postprocess", "Post Process");
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                              snapshot.currentViewModeDisplayName.c_str());
    }

    constexpr uint32_t textureLimits[] = {0, 2048, 1024, 512};
    ImGui::BeginDisabled(snapshot.textureLimitLocked);
    if (ImGui::BeginCombo("Texture Limit",
                          textureLimitLabel(snapshot.textureLimit))) {
        for (uint32_t limit : textureLimits) {
            const bool selected = snapshot.textureLimit == limit;
            if (ImGui::Selectable(textureLimitLabel(limit), selected) &&
                !selected && actions.setTextureLimit)
                actions.setTextureLimit(limit);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !snapshot.textureLimitHelp.empty())
        ImGui::SetTooltip("%s", snapshot.textureLimitHelp.c_str());
    ImGui::EndDisabled();

    ImGui::Separator();
    float exposureEv = snapshot.features.requested.exposureEv;
    if (ImGui::DragFloat("Exposure EV", &exposureEv, 0.05f, -10.0f,
                         10.0f)) {
        RenderSettingsPatch patch;
        patch.exposureEv = exposureEv;
        applyPatch(actions, patch);
    }
    constexpr const char *toneMapperLabels[] = {"PassThrough", "Reinhard",
                                                "ACES"};
    int toneMapper =
        static_cast<int>(snapshot.features.requested.toneMapper);
    if (ImGui::Combo("PBR Tone Mapper", &toneMapper, toneMapperLabels,
                     static_cast<int>(std::size(toneMapperLabels)))) {
        RenderSettingsPatch patch;
        patch.toneMapper = static_cast<ToneMapper>(toneMapper);
        applyPatch(actions, patch);
    }
    ImGui::TextDisabled("Legacy/debug use PassThrough");
}

void RenderSettingsPanel::drawCamera(
    const RenderSettingsPanelSnapshot &snapshot,
    const RenderSettingsPanelActions &actions) const {
    const RenderCameraPanelSnapshot &camera = snapshot.camera;
    ImGui::Text("Position: (%.2f, %.2f, %.2f)", camera.position.x,
                camera.position.y, camera.position.z);
    float moveSpeed = camera.moveSpeed;
    constexpr ImGuiSliderFlags flags =
        ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp;
    if (ImGui::SliderFloat("Move Speed", &moveSpeed, 0.1f, 100.0f,
                           "%.2f", flags) &&
        actions.setCameraMoveSpeed)
        actions.setCameraMoveSpeed(moveSpeed);
    float nearPlane = camera.nearPlane;
    float farPlane = camera.farPlane;
    bool clipChanged = ImGui::DragFloat("Near Plane", &nearPlane, 0.001f,
                                        0.001f, 100.0f, "%.4f");
    clipChanged |= ImGui::DragFloat("Far Plane", &farPlane, 0.1f, 1.0f,
                                    100000.0f, "%.2f");
    if (clipChanged && actions.setCameraClipPlanes)
        actions.setCameraClipPlanes(nearPlane, farPlane);
    ImGui::Separator();
    if (camera.sceneBounds.valid) {
        ImGui::Text("Bounds Center: (%.2f, %.2f, %.2f)",
                    camera.sceneBounds.center.x,
                    camera.sceneBounds.center.y,
                    camera.sceneBounds.center.z);
        ImGui::Text("Bounds Radius: %.2f", camera.sceneBounds.radius);
    } else {
        ImGui::TextDisabled("Bounds unavailable");
    }
}

void RenderSettingsPanel::drawEffects(
    const RenderSettingsPanelSnapshot &snapshot,
    const RenderSettingsPanelActions &actions) const {
    const RenderSettingsSnapshot featureSnapshot =
        snapshot.features;
    const RenderFeatureSupport &featureSupport = featureSnapshot.support;
    const RenderFeatureRuntimeState &featureRuntime = featureSnapshot.runtime;
    const bool available = featureSupport.bloom.supported;
    const bool compatible = snapshot.viewModeSupportsBloom;
    ImGui::BeginDisabled(!available);
    bool enabled = snapshot.features.requested.bloomEnabled;
    if (ImGui::Checkbox("Bloom", &enabled)) {
        RenderSettingsPatch patch;
        patch.bloomEnabled = enabled;
        applyPatch(actions, patch);
    }
    ImGui::BeginDisabled(!snapshot.features.requested.bloomEnabled);
    float intensity = snapshot.features.requested.bloomIntensity;
    if (ImGui::DragFloat("Intensity", &intensity, 0.01f, 0.0f, 5.0f)) {
        RenderSettingsPatch patch;
        patch.bloomIntensity = intensity;
        applyPatch(actions, patch);
    }
    if (ImGui::TreeNodeEx("Bloom Tuning")) {
        float threshold = snapshot.features.requested.bloomThreshold;
        if (ImGui::DragFloat("Threshold", &threshold, 0.05f, 0.0f,
                             20.0f)) {
            RenderSettingsPatch patch;
            patch.bloomThreshold = threshold;
            applyPatch(actions, patch);
        }
        float softKnee = snapshot.features.requested.bloomSoftKnee;
        if (ImGui::DragFloat("Soft Knee", &softKnee, 0.01f, 0.0f,
                             1.0f)) {
            RenderSettingsPatch patch;
            patch.bloomSoftKnee = softKnee;
            applyPatch(actions, patch);
        }
        ImGui::TextDisabled("Up to 6 half-resolution levels");
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    const bool active = featureRuntime.bloomActive;
    editor::statusIndicator(
        active ? "Bloom active" : "Bloom inactive",
        active ? editor::StatusTone::Success : editor::StatusTone::Neutral);
    if (!available) {
        editor::statusIndicator(
            "Bloom unavailable", editor::StatusTone::Warning,
            featureSupport.bloom.unavailableReason.c_str());
    } else if (snapshot.features.requested.bloomEnabled && !compatible) {
        ImGui::TextDisabled("Selected View Mode does not support Bloom.");
    }

    ImGui::SeparatorText("Ambient Occlusion");
    const ScreenSpaceEffectsStatus screenStatus =
        snapshot.screenSpace;
    constexpr const char *aoModeLabels[] = {"Off", "SSAO", "CACAO", "GTAO"};
    int aoMode = static_cast<int>(snapshot.features.requested.ambientOcclusionMode);
    if (ImGui::BeginCombo("Mode##AmbientOcclusion", aoModeLabels[aoMode])) {
        for (int index = 0; index < static_cast<int>(std::size(aoModeLabels));
             ++index) {
            const auto mode = static_cast<AmbientOcclusionMode>(index);
            const bool supported =
                mode == AmbientOcclusionMode::Off ||
                (mode == AmbientOcclusionMode::Ssao &&
                 featureSupport.ssao.supported) ||
                (mode == AmbientOcclusionMode::Cacao &&
                 featureSupport.cacao.supported) ||
                (mode == AmbientOcclusionMode::Gtao &&
                 featureSupport.gtao.supported);
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(aoModeLabels[index], index == aoMode)) {
                RenderSettingsPatch patch;
                patch.ambientOcclusionMode = mode;
                applyPatch(actions, patch);
            }
            if (index == aoMode)
                ImGui::SetItemDefaultFocus();
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    const bool ssaoRequested =
        snapshot.features.requested.ambientOcclusionMode == AmbientOcclusionMode::Ssao;
    const bool ssaoDebugRequested =
        snapshot.features.requested.screenSpaceDebugView ==
            ScreenSpaceDebugView::SsaoRaw ||
        snapshot.features.requested.screenSpaceDebugView ==
            ScreenSpaceDebugView::SsaoFiltered;
    ImGui::BeginDisabled(!ssaoRequested && !ssaoDebugRequested);
    constexpr const char *qualityLabels[] = {"Low (8)", "Medium (16)",
                                              "High (32)"};
    int quality = static_cast<int>(snapshot.features.requested.ssaoQuality);
    if (ImGui::Combo("Quality##SSAO", &quality, qualityLabels,
                     static_cast<int>(std::size(qualityLabels)))) {
        RenderSettingsPatch patch;
        patch.ssaoQuality = static_cast<SsaoQuality>(quality);
        applyPatch(actions, patch);
    }
    float radius = snapshot.features.requested.ssaoRadius;
    if (ImGui::DragFloat("Radius##SSAO", &radius, 0.01f, 0.05f, 10.0f,
                         "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssaoRadius = radius;
        applyPatch(actions, patch);
    }
    float bias = snapshot.features.requested.ssaoBias;
    if (ImGui::DragFloat("Bias##SSAO", &bias, 0.001f, 0.0f, 0.2f,
                         "%.3f")) {
        RenderSettingsPatch patch;
        patch.ssaoBias = bias;
        applyPatch(actions, patch);
    }
    float aoIntensity = snapshot.features.requested.ssaoIntensity;
    if (ImGui::DragFloat("Intensity##SSAO", &aoIntensity, 0.02f, 0.0f,
                         4.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssaoIntensity = aoIntensity;
        applyPatch(actions, patch);
    }
    float aoPower = snapshot.features.requested.ssaoPower;
    if (ImGui::DragFloat("Power##SSAO", &aoPower, 0.02f, 0.25f, 4.0f,
                         "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssaoPower = aoPower;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();

    const bool ssaoActive = featureRuntime.activeAmbientOcclusion ==
                            AmbientOcclusionMode::Ssao;
    editor::statusIndicator(
        ssaoActive ? "SSAO active" : "SSAO inactive",
        ssaoActive ? editor::StatusTone::Success
                   : editor::StatusTone::Neutral,
        !featureSupport.ssao.supported
            ? featureSupport.ssao.unavailableReason.c_str()
            : (ssaoRequested && !snapshot.viewModeSupportsScreenSpace
                   ? "Selected View Mode does not consume screen-space AO."
                   : nullptr));

    const bool cacaoRequested =
        snapshot.features.requested.ambientOcclusionMode == AmbientOcclusionMode::Cacao;
    const bool cacaoDebugRequested =
        snapshot.features.requested.screenSpaceDebugView ==
        ScreenSpaceDebugView::CacaoOutput;
    ImGui::BeginDisabled(!featureSupport.cacao.supported ||
                         (!cacaoRequested && !cacaoDebugRequested));
    constexpr const char *cacaoQualityLabels[] = {
        "Lowest", "Low", "Medium", "High", "Highest"};
    int cacaoQuality = static_cast<int>(snapshot.features.requested.cacao.quality);
    if (ImGui::Combo("Quality##CACAO", &cacaoQuality, cacaoQualityLabels,
                     static_cast<int>(std::size(cacaoQualityLabels)))) {
        RenderSettingsPatch patch;
        patch.cacaoQuality = static_cast<CacaoQuality>(cacaoQuality);
        applyPatch(actions, patch);
    }
    constexpr const char *cacaoResolutionLabels[] = {"Native", "Half"};
    int cacaoResolution = static_cast<int>(snapshot.features.requested.cacao.resolution);
    if (ImGui::Combo("Resolution##CACAO", &cacaoResolution,
                     cacaoResolutionLabels,
                     static_cast<int>(std::size(cacaoResolutionLabels)))) {
        RenderSettingsPatch patch;
        patch.cacaoResolution =
            static_cast<CacaoResolution>(cacaoResolution);
        applyPatch(actions, patch);
    }
    float cacaoRadius = snapshot.features.requested.cacao.radius;
    if (ImGui::DragFloat("Radius##CACAO", &cacaoRadius, 0.01f, 0.05f,
                         10.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.cacaoRadius = cacaoRadius;
        applyPatch(actions, patch);
    }
    float cacaoIntensity = snapshot.features.requested.cacao.intensity;
    if (ImGui::DragFloat("Intensity##CACAO", &cacaoIntensity, 0.02f, 0.0f,
                         4.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.cacaoIntensity = cacaoIntensity;
        applyPatch(actions, patch);
    }
    float cacaoPower = snapshot.features.requested.cacao.power;
    if (ImGui::DragFloat("Power##CACAO", &cacaoPower, 0.02f, 0.25f, 4.0f,
                         "%.2f")) {
        RenderSettingsPatch patch;
        patch.cacaoPower = cacaoPower;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();

    const bool cacaoActive = featureRuntime.activeAmbientOcclusion ==
                             AmbientOcclusionMode::Cacao;
    editor::statusIndicator(
        cacaoActive ? "CACAO active" : "CACAO inactive",
        cacaoActive ? editor::StatusTone::Success
                    : editor::StatusTone::Neutral,
        !featureSupport.cacao.supported
            ? featureSupport.cacao.unavailableReason.c_str()
            : (cacaoRequested && !snapshot.viewModeSupportsScreenSpace
                   ? "Selected View Mode does not consume screen-space AO."
                   : nullptr));
    if (screenStatus.cacaoInitialized) {
        ImGui::TextDisabled("CACAO: %s, generation %llu, %ux%u",
                            screenStatus.cacaoFp32 ? "FP32" : "FP16",
                            static_cast<unsigned long long>(
                                screenStatus.cacaoGeneration),
                            screenStatus.cacaoOutputExtent.width,
                            screenStatus.cacaoOutputExtent.height);
    }

    const bool gtaoRequested =
        snapshot.features.requested.ambientOcclusionMode == AmbientOcclusionMode::Gtao;
    const bool gtaoDebugRequested =
        snapshot.features.requested.screenSpaceDebugView == ScreenSpaceDebugView::GtaoRaw ||
        snapshot.features.requested.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoTemporal ||
        snapshot.features.requested.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoFiltered ||
        snapshot.features.requested.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoRejection ||
        snapshot.features.requested.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoHistoryWeight;
    ImGui::BeginDisabled(!featureSupport.gtao.supported ||
                         (!gtaoRequested && !gtaoDebugRequested));
    constexpr const char *gtaoQualityLabels[] = {
        "Low (2x2)", "Medium (3x4)", "High (4x6)"};
    int gtaoQuality = static_cast<int>(snapshot.features.requested.gtao.quality);
    if (ImGui::Combo("Quality##GTAO", &gtaoQuality, gtaoQualityLabels,
                     static_cast<int>(std::size(gtaoQualityLabels)))) {
        RenderSettingsPatch patch;
        patch.gtaoQuality = static_cast<GtaoQuality>(gtaoQuality);
        applyPatch(actions, patch);
    }
    float gtaoRadius = snapshot.features.requested.gtao.radius;
    if (ImGui::DragFloat("Radius##GTAO", &gtaoRadius, 0.01f, 0.05f, 10.0f,
                         "%.2f")) {
        RenderSettingsPatch patch;
        patch.gtaoRadius = gtaoRadius;
        applyPatch(actions, patch);
    }
    float gtaoFalloff = snapshot.features.requested.gtao.falloff;
    if (ImGui::SliderFloat("Falloff##GTAO", &gtaoFalloff, 0.0f, 0.99f,
                           "%.2f")) {
        RenderSettingsPatch patch;
        patch.gtaoFalloff = gtaoFalloff;
        applyPatch(actions, patch);
    }
    float gtaoIntensity = snapshot.features.requested.gtao.intensity;
    if (ImGui::DragFloat("Intensity##GTAO", &gtaoIntensity, 0.02f, 0.0f,
                         4.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.gtaoIntensity = gtaoIntensity;
        applyPatch(actions, patch);
    }
    float gtaoPower = snapshot.features.requested.gtao.power;
    if (ImGui::DragFloat("Power##GTAO", &gtaoPower, 0.02f, 0.25f, 4.0f,
                         "%.2f")) {
        RenderSettingsPatch patch;
        patch.gtaoPower = gtaoPower;
        applyPatch(actions, patch);
    }
    float gtaoTemporalWeight = snapshot.features.requested.gtao.temporalWeight;
    if (ImGui::SliderFloat("History Weight##GTAO", &gtaoTemporalWeight,
                           0.0f, 0.99f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.gtaoTemporalWeight = gtaoTemporalWeight;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();

    editor::statusIndicator(
        featureRuntime.activeAmbientOcclusion == AmbientOcclusionMode::Gtao
            ? "GTAO active"
            : "GTAO inactive",
        featureRuntime.activeAmbientOcclusion == AmbientOcclusionMode::Gtao
            ? editor::StatusTone::Success
            : editor::StatusTone::Neutral,
        !featureSupport.gtao.supported
            ? featureSupport.gtao.unavailableReason.c_str()
            : (gtaoRequested && !snapshot.viewModeSupportsScreenSpace
                   ? "Selected View Mode does not consume screen-space AO."
                   : nullptr));
    if (featureSupport.gtao.supported &&
        (gtaoRequested || gtaoDebugRequested)) {
        ImGui::TextDisabled("History: %s, generation %llu, %ux%u",
                            screenStatus.gtaoHistoryValid ? "valid" : "reset",
                            static_cast<unsigned long long>(
                                screenStatus.gtaoHistoryGeneration),
                            screenStatus.gtaoExtent.width,
                            screenStatus.gtaoExtent.height);
        if (!screenStatus.gtaoLastResetReason.empty()) {
            ImGui::TextDisabled("Reset: %s",
                                screenStatus.gtaoLastResetReason.c_str());
        }
    }

    ImGui::SeparatorText("Temporal Anti-Aliasing");
    constexpr const char *taaModeLabels[] = {"Off", "TAA"};
    int taaMode =
        static_cast<int>(snapshot.features.requested.temporalAntiAliasingMode);
    ImGui::BeginDisabled(!featureSupport.taa.supported);
    if (ImGui::Combo("Mode##TAA", &taaMode, taaModeLabels,
                     static_cast<int>(std::size(taaModeLabels)))) {
        RenderSettingsPatch patch;
        patch.temporalAntiAliasingMode =
            static_cast<TemporalAntiAliasingMode>(taaMode);
        applyPatch(actions, patch);
    }
    const bool taaRequested =
        snapshot.features.requested.temporalAntiAliasingMode ==
        TemporalAntiAliasingMode::Taa;
    const bool taaDebugRequested =
        snapshot.features.requested.screenSpaceDebugView ==
            ScreenSpaceDebugView::TaaHistory ||
        snapshot.features.requested.screenSpaceDebugView ==
            ScreenSpaceDebugView::TaaRejection ||
        snapshot.features.requested.screenSpaceDebugView ==
            ScreenSpaceDebugView::TaaHistoryWeight;
    ImGui::BeginDisabled(!taaRequested && !taaDebugRequested);
    float historyWeight = snapshot.features.requested.taaHistoryWeight;
    if (ImGui::SliderFloat("History Weight##TAA", &historyWeight, 0.0f,
                           0.99f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.taaHistoryWeight = historyWeight;
        applyPatch(actions, patch);
    }
    float sharpness = snapshot.features.requested.taaSharpness;
    if (ImGui::SliderFloat("Sharpness##TAA", &sharpness, 0.0f, 1.0f,
                           "%.2f")) {
        RenderSettingsPatch patch;
        patch.taaSharpness = sharpness;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    editor::statusIndicator(
        featureRuntime.taaActive ? "TAA active" : "TAA inactive",
        featureRuntime.taaActive ? editor::StatusTone::Success
                                 : editor::StatusTone::Neutral,
        !featureSupport.taa.supported
            ? featureSupport.taa.unavailableReason.c_str()
            : nullptr);
    if (featureSupport.taa.supported) {
        ImGui::TextDisabled(
            "History: %s, generation %llu, jitter (%.2f, %.2f)",
            screenStatus.taaHistoryValid ? "valid" : "reset",
            static_cast<unsigned long long>(
                screenStatus.taaHistoryGeneration),
            screenStatus.taaJitterPixels.x,
            screenStatus.taaJitterPixels.y);
        if (!screenStatus.taaLastResetReason.empty()) {
            ImGui::TextDisabled("Reset: %s",
                                screenStatus.taaLastResetReason.c_str());
        }
    }

    ImGui::SeparatorText("Reflections");
    constexpr const char *reflectionLabels[] = {"IBL Only", "SSR"};
    int reflectionMode = static_cast<int>(snapshot.features.requested.reflectionMode);
    ImGui::BeginDisabled(!featureSupport.ssr.supported);
    if (ImGui::Combo("Mode##Reflections", &reflectionMode,
                     reflectionLabels,
                     static_cast<int>(std::size(reflectionLabels)))) {
        RenderSettingsPatch patch;
        patch.reflectionMode = static_cast<ReflectionMode>(reflectionMode);
        applyPatch(actions, patch);
    }
    const bool ssrRequested =
        snapshot.features.requested.reflectionMode == ReflectionMode::Ssr;
    ImGui::BeginDisabled(!ssrRequested);
    constexpr const char *ssrQualityLabels[] = {"Low", "Medium", "High"};
    int ssrQuality = static_cast<int>(snapshot.features.requested.ssrQuality);
    if (ImGui::Combo("Quality##SSR", &ssrQuality, ssrQualityLabels,
                     static_cast<int>(std::size(ssrQualityLabels)))) {
        RenderSettingsPatch patch;
        patch.ssrQuality = static_cast<SsrQuality>(ssrQuality);
        applyPatch(actions, patch);
    }
    float ssrDistance = snapshot.features.requested.ssrMaxDistance;
    if (ImGui::DragFloat("Max Distance##SSR", &ssrDistance, 0.25f,
                         0.1f, 1000.0f, "%.1f")) {
        RenderSettingsPatch patch; patch.ssrMaxDistance = ssrDistance;
        applyPatch(actions, patch);
    }
    float ssrThickness = snapshot.features.requested.ssrThickness;
    if (ImGui::DragFloat("Thickness##SSR", &ssrThickness, 0.005f,
                         0.001f, 10.0f, "%.3f")) {
        RenderSettingsPatch patch; patch.ssrThickness = ssrThickness;
        applyPatch(actions, patch);
    }
    float ssrRoughness = snapshot.features.requested.ssrMaxRoughness;
    if (ImGui::SliderFloat("Max Roughness##SSR", &ssrRoughness,
                           0.0f, 1.0f, "%.2f")) {
        RenderSettingsPatch patch; patch.ssrMaxRoughness = ssrRoughness;
        applyPatch(actions, patch);
    }
    float ssrIntensity = snapshot.features.requested.ssrIntensity;
    if (ImGui::SliderFloat("Intensity##SSR", &ssrIntensity,
                           0.0f, 4.0f, "%.2f")) {
        RenderSettingsPatch patch; patch.ssrIntensity = ssrIntensity;
        applyPatch(actions, patch);
    }
    float ssrHistory = snapshot.features.requested.ssrHistoryWeight;
    if (ImGui::SliderFloat("History Weight##SSR", &ssrHistory,
                           0.0f, 0.99f, "%.2f")) {
        RenderSettingsPatch patch; patch.ssrHistoryWeight = ssrHistory;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    editor::statusIndicator(
        featureRuntime.ssrActive ? "SSR active" : "SSR inactive",
        featureRuntime.ssrActive ? editor::StatusTone::Success
                                 : editor::StatusTone::Neutral,
        !featureSupport.ssr.supported
            ? featureSupport.ssr.unavailableReason.c_str()
            : (ssrRequested && !snapshot.viewModeSupportsScreenSpace
                   ? "Selected View Mode does not consume screen-space reflections."
                   : nullptr));
    if (featureSupport.ssr.supported && ssrRequested) {
        ImGui::TextDisabled("History: %s, generation %llu, %ux%u",
                            screenStatus.ssrHistoryValid ? "valid" : "reset",
                            static_cast<unsigned long long>(
                                screenStatus.ssrHistoryGeneration),
                            screenStatus.ssrExtent.width,
                            screenStatus.ssrExtent.height);
        if (!screenStatus.ssrLastResetReason.empty())
            ImGui::TextDisabled("Reset: %s",
                                screenStatus.ssrLastResetReason.c_str());
    }

    ImGui::SeparatorText("Global Illumination");
    constexpr const char *giModeLabels[] = {
        "Ambient / IBL", "SSGI", "DDGI", "SSGI + DDGI"};
    int giMode = static_cast<int>(snapshot.features.requested.globalIlluminationMode);
    const DdgiRuntimeStatus ddgiStatus = snapshot.ddgi;
    if (ImGui::BeginCombo("Mode##GI", giModeLabels[giMode])) {
        for (int index = 0;
             index < static_cast<int>(std::size(giModeLabels)); ++index) {
            const auto mode = static_cast<GlobalIlluminationMode>(index);
            const bool needsSsgi = mode == GlobalIlluminationMode::Ssgi ||
                                   mode == GlobalIlluminationMode::SsgiDdgi;
            const bool needsDdgi = mode == GlobalIlluminationMode::Ddgi ||
                                   mode == GlobalIlluminationMode::SsgiDdgi;
            const bool supported =
                (!needsSsgi || featureSupport.ssgi.supported) &&
                (!needsDdgi || (featureSupport.ddgi.supported &&
                                ddgiStatus.componentPresent));
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(giModeLabels[index], giMode == index)) {
                RenderSettingsPatch patch;
                patch.globalIlluminationMode = mode;
                applyPatch(actions, patch);
            }
            ImGui::EndDisabled();
            if (!supported && needsDdgi &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    !featureSupport.ddgi.supported
                        ? featureSupport.ddgi.unavailableReason.c_str()
                        : "Add a DDGI Probe Volume to the native scene first.");
            }
            if (giMode == index)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    const bool ssgiRequested =
        snapshot.features.requested.globalIlluminationMode == GlobalIlluminationMode::Ssgi ||
        snapshot.features.requested.globalIlluminationMode ==
            GlobalIlluminationMode::SsgiDdgi;
    ImGui::BeginDisabled(!ssgiRequested);
    constexpr const char *ssgiQualityLabels[] = {"Low", "Medium", "High"};
    int ssgiQuality = static_cast<int>(snapshot.features.requested.ssgiQuality);
    if (ImGui::Combo("Quality##SSGI", &ssgiQuality, ssgiQualityLabels,
                     static_cast<int>(std::size(ssgiQualityLabels)))) {
        RenderSettingsPatch patch;
        patch.ssgiQuality = static_cast<SsgiQuality>(ssgiQuality);
        applyPatch(actions, patch);
    }
    float ssgiDistance = snapshot.features.requested.ssgiMaxDistance;
    if (ImGui::DragFloat("Max Distance##SSGI", &ssgiDistance, 0.1f,
                         0.05f, 1000.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssgiMaxDistance = ssgiDistance;
        applyPatch(actions, patch);
    }
    float ssgiThickness = snapshot.features.requested.ssgiThickness;
    if (ImGui::DragFloat("Thickness##SSGI", &ssgiThickness, 0.005f,
                         0.001f, 10.0f, "%.3f")) {
        RenderSettingsPatch patch;
        patch.ssgiThickness = ssgiThickness;
        applyPatch(actions, patch);
    }
    float ssgiIntensity = snapshot.features.requested.ssgiIntensity;
    if (ImGui::SliderFloat("Intensity##SSGI", &ssgiIntensity,
                           0.0f, 4.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssgiIntensity = ssgiIntensity;
        applyPatch(actions, patch);
    }
    float ssgiClamp = snapshot.features.requested.ssgiRadianceClamp;
    if (ImGui::DragFloat("Radiance Clamp##SSGI", &ssgiClamp, 0.1f,
                         0.1f, 100.0f, "%.1f")) {
        RenderSettingsPatch patch;
        patch.ssgiRadianceClamp = ssgiClamp;
        applyPatch(actions, patch);
    }
    float ssgiHistory = snapshot.features.requested.ssgiHistoryWeight;
    if (ImGui::SliderFloat("History Weight##SSGI", &ssgiHistory,
                           0.0f, 0.99f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssgiHistoryWeight = ssgiHistory;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();
    editor::statusIndicator(
        featureRuntime.ssgiActive ? "SSGI active" : "SSGI inactive",
        featureRuntime.ssgiActive ? editor::StatusTone::Success
                                  : editor::StatusTone::Neutral,
        !featureSupport.ssgi.supported
            ? featureSupport.ssgi.unavailableReason.c_str()
            : (ssgiRequested && !snapshot.viewModeSupportsScreenSpace
                   ? "Selected View Mode does not consume screen-space GI."
                   : nullptr));
    if (featureSupport.ssgi.supported && ssgiRequested) {
        ImGui::TextDisabled("History: %s, generation %llu, %ux%u",
                            screenStatus.ssgiHistoryValid ? "valid" : "reset",
                            static_cast<unsigned long long>(
                                screenStatus.ssgiHistoryGeneration),
                            screenStatus.ssgiExtent.width,
                            screenStatus.ssgiExtent.height);
        if (!screenStatus.ssgiLastResetReason.empty())
            ImGui::TextDisabled("Reset: %s",
                                screenStatus.ssgiLastResetReason.c_str());
    }

    const bool ddgiRequested =
        snapshot.features.requested.globalIlluminationMode == GlobalIlluminationMode::Ddgi ||
        snapshot.features.requested.globalIlluminationMode ==
            GlobalIlluminationMode::SsgiDdgi;
    ImGui::BeginDisabled(!ddgiRequested || !featureSupport.ddgi.supported);
    float ddgiClamp = snapshot.features.requested.ddgi.radianceClamp;
    if (ImGui::DragFloat("Radiance Clamp##DDGI", &ddgiClamp, 0.1f,
                         0.1f, 100.0f, "%.1f")) {
        RenderSettingsPatch patch;
        patch.ddgiRadianceClamp = ddgiClamp;
        applyPatch(actions, patch);
    }
    constexpr const char *ddgiDebugLabels[] = {
        "None", "Irradiance", "Distance", "Classification"};
    int ddgiDebug = static_cast<int>(snapshot.features.requested.ddgi.debugView);
    if (ImGui::Combo("Debug##DDGI", &ddgiDebug, ddgiDebugLabels,
                     static_cast<int>(std::size(ddgiDebugLabels)))) {
        RenderSettingsPatch patch;
        patch.ddgiDebugView = static_cast<DdgiDebugView>(ddgiDebug);
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();
    editor::statusIndicator(
        featureRuntime.ddgiActive ? "DDGI active" : "DDGI inactive",
        featureRuntime.ddgiActive ? editor::StatusTone::Success
                                  : editor::StatusTone::Neutral,
        !featureSupport.ddgi.supported
            ? featureSupport.ddgi.unavailableReason.c_str()
            : (ddgiRequested && !ddgiStatus.componentPresent
                   ? "The active native scene has no DDGI Probe Volume."
                   : (ddgiRequested &&
                              !snapshot.viewModeSupportsDdgi
                          ? "Selected View Mode does not consume DDGI."
                          : nullptr)));
    if (ddgiRequested && !ddgiStatus.componentPresent &&
        actions.createDdgiProbeVolume) {
        if (ImGui::Button("Create Fitted Probe Volume"))
            actions.createDdgiProbeVolume();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Creates one root DDGI volume fitted to the current scene "
                "bounds. The change participates in Undo/Redo and must be "
                "saved.");
        }
    }
    if (ddgiStatus.componentPresent) {
        ImGui::TextDisabled(
            "Probes: %u, update %u x %u rays, cursor %u",
            ddgiStatus.probeCount, ddgiStatus.probesUpdatedPerFrame,
            ddgiStatus.raysPerProbe, ddgiStatus.updateCursor);
        ImGui::TextDisabled(
            "TLAS instances: %u, generation %llu, memory %.2f MiB",
            ddgiStatus.tracedInstanceCount,
            static_cast<unsigned long long>(ddgiStatus.generation),
            static_cast<double>(ddgiStatus.allocatedBytes) /
                (1024.0 * 1024.0));
    }

    ImGui::SeparatorText("Screen-Space Debug");
    constexpr const char *screenDebugLabels[] = {
        "None", "Nearest Depth", "Scene Color", "SSAO Raw",
        "SSAO Filtered", "CACAO Output", "GTAO Raw", "GTAO Temporal",
        "GTAO Filtered", "GTAO Rejection", "GTAO History Weight",
        "TAA History", "TAA Rejection", "TAA History Weight",
        "SSR Raw", "SSR Temporal", "SSR Filtered", "SSR Confidence",
        "SSR Rejection", "SSGI Raw", "SSGI Temporal", "SSGI Filtered",
        "SSGI Confidence", "SSGI Variance", "SSGI Rejection"};
    int screenDebug =
        static_cast<int>(snapshot.features.requested.screenSpaceDebugView);
    const char *preview = screenDebugLabels[screenDebug];
    if (ImGui::BeginCombo("Debug View##ScreenSpace", preview)) {
        for (int index = 0; index < static_cast<int>(std::size(screenDebugLabels));
             ++index) {
            const auto view = static_cast<ScreenSpaceDebugView>(index);
            const bool supported =
                view == ScreenSpaceDebugView::None ||
                (view == ScreenSpaceDebugView::NearestDepth &&
                 featureSupport.depthPyramid.supported) ||
                (view == ScreenSpaceDebugView::SceneColor &&
                 featureSupport.colorPyramid.supported) ||
                ((view == ScreenSpaceDebugView::SsaoRaw ||
                  view == ScreenSpaceDebugView::SsaoFiltered) &&
                 featureSupport.ssao.supported) ||
                (view == ScreenSpaceDebugView::CacaoOutput &&
                 featureSupport.cacao.supported) ||
                ((view == ScreenSpaceDebugView::GtaoRaw ||
                  view == ScreenSpaceDebugView::GtaoTemporal ||
                  view == ScreenSpaceDebugView::GtaoFiltered ||
                  view == ScreenSpaceDebugView::GtaoRejection ||
                  view == ScreenSpaceDebugView::GtaoHistoryWeight) &&
                 featureSupport.gtao.supported) ||
                ((view == ScreenSpaceDebugView::TaaHistory ||
                  view == ScreenSpaceDebugView::TaaRejection ||
                  view == ScreenSpaceDebugView::TaaHistoryWeight) &&
                 featureSupport.taa.supported) ||
                ((view == ScreenSpaceDebugView::SsrRaw ||
                  view == ScreenSpaceDebugView::SsrTemporal ||
                  view == ScreenSpaceDebugView::SsrFiltered ||
                  view == ScreenSpaceDebugView::SsrConfidence ||
                  view == ScreenSpaceDebugView::SsrRejection) &&
                 featureSupport.ssr.supported) ||
                ((view == ScreenSpaceDebugView::SsgiRaw ||
                  view == ScreenSpaceDebugView::SsgiTemporal ||
                  view == ScreenSpaceDebugView::SsgiFiltered ||
                  view == ScreenSpaceDebugView::SsgiConfidence ||
                  view == ScreenSpaceDebugView::SsgiVariance ||
                  view == ScreenSpaceDebugView::SsgiRejection) &&
                 featureSupport.ssgi.supported);
            ImGui::BeginDisabled(!supported);
            const bool selected = index == screenDebug;
            if (ImGui::Selectable(screenDebugLabels[index], selected)) {
                RenderSettingsPatch patch;
                patch.screenSpaceDebugView = view;
                if (view != ScreenSpaceDebugView::None) {
                    patch.surfaceDebugView = SurfaceDebugView::None;
                    patch.gBufferDebugView = GBufferDebugView::None;
                    patch.deferredLightingDebugView =
                        DeferredLightingDebugView::None;
                }
                applyPatch(actions, patch);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    uint32_t maximumMip = 0;
    if (snapshot.features.requested.screenSpaceDebugView ==
        ScreenSpaceDebugView::NearestDepth) {
        maximumMip = screenStatus.depthMipLevels > 0
                         ? screenStatus.depthMipLevels - 1u
                         : 0u;
    } else if (snapshot.features.requested.screenSpaceDebugView ==
               ScreenSpaceDebugView::SceneColor) {
        maximumMip = screenStatus.colorMipLevels > 0
                         ? screenStatus.colorMipLevels - 1u
                         : 0u;
    }
    int debugMip = static_cast<int>(std::min(
        snapshot.features.requested.screenSpaceDebugMip, maximumMip));
    ImGui::BeginDisabled(maximumMip == 0);
    if (ImGui::SliderInt("Mip##ScreenSpace", &debugMip, 0,
                         static_cast<int>(maximumMip))) {
        RenderSettingsPatch patch;
        patch.screenSpaceDebugMip = static_cast<uint32_t>(debugMip);
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Resources: %.2f MiB",
                        static_cast<double>(screenStatus.estimatedMemoryBytes) /
                            (1024.0 * 1024.0));
    ImGui::TextDisabled("Depth Hierarchy: %s, %u dispatches",
                        depthHierarchyModeName(
                            screenStatus.depthHierarchyMode),
                        screenStatus.depthHierarchyDispatches);
}

void RenderSettingsPanel::drawSurfaceData(
    const RenderSettingsPanelSnapshot &snapshot,
    const RenderSettingsPanelActions &actions) const {
    const RenderSettingsSnapshot featureSnapshot =
        snapshot.features;
    const SurfaceDataStatus status = snapshot.surfaceData;
    constexpr const char *labels[] = {
        "None", "Normal", "Roughness", "Motion", "History Validity"};
    int mode = static_cast<int>(snapshot.features.requested.surfaceDebugView);
    ImGui::BeginDisabled(!featureSnapshot.support.surfaceData.supported);
    if (ImGui::Combo("Debug View", &mode, labels,
                     static_cast<int>(std::size(labels)))) {
        RenderSettingsPatch patch;
        patch.surfaceDebugView = static_cast<SurfaceDebugView>(mode);
        if (patch.surfaceDebugView != SurfaceDebugView::None) {
            patch.gBufferDebugView = GBufferDebugView::None;
            patch.deferredLightingDebugView =
                DeferredLightingDebugView::None;
            patch.screenSpaceDebugView = ScreenSpaceDebugView::None;
        }
        applyPatch(actions, patch);
    }
    ImGui::BeginDisabled(
        snapshot.features.requested.surfaceDebugView != SurfaceDebugView::Motion);
    float motionScale = snapshot.features.requested.surfaceMotionDebugScale;
    if (ImGui::DragFloat("Motion Scale", &motionScale, 0.5f, 0.1f,
                         1024.0f, "%.1f")) {
        RenderSettingsPatch patch;
        patch.surfaceMotionDebugScale = motionScale;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (featureSnapshot.support.surfaceData.supported) {
        editor::statusIndicator(
            featureSnapshot.runtime.surfaceDataActive
                ? "Surface data active"
                : "Surface data inactive",
            featureSnapshot.runtime.surfaceDataActive
                ? editor::StatusTone::Success
                : editor::StatusTone::Neutral);
        ImGui::Text("History: %u / %u items",
                    snapshot.temporalHistory.historyValidItems,
                    static_cast<uint32_t>(snapshot.renderItemCount));
        ImGui::TextDisabled("Generation %llu",
                            static_cast<unsigned long long>(
                                snapshot.temporalHistory.historyGeneration));
        if (!snapshot.temporalHistory.invalidationReason.empty()) {
            ImGui::TextDisabled("Reset: %s",
                                snapshot.temporalHistory.invalidationReason
                                    .c_str());
        }
        ImGui::TextDisabled("Formats: depth=%d normal=%d motion=%d",
                            static_cast<int>(status.depthFormat),
                            static_cast<int>(status.normalRoughnessFormat),
                            static_cast<int>(status.motionFormat));
        ImGui::TextDisabled("History buffers: %u / %u (%llu KiB)",
                            status.historyCapacities[0],
                            status.historyCapacities[1],
                            static_cast<unsigned long long>(
                                status.allocatedBytes / 1024u));
    } else {
        editor::statusIndicator("Surface data unavailable",
                                editor::StatusTone::Warning,
                                featureSnapshot.support.surfaceData
                                    .unavailableReason.c_str());
    }

    ImGui::SeparatorText("Deferred GBuffer");
    constexpr const char *gBufferLabels[] = {
        "None",       "Base Color", "Normal",   "Metallic",
        "Roughness",  "Occlusion",  "Emissive", "Motion",
        "Surface Flags"};
    int gBufferMode =
        static_cast<int>(snapshot.features.requested.gBufferDebugView);
    ImGui::BeginDisabled(!featureSnapshot.support.gBuffer.supported);
    if (ImGui::Combo("Debug View##GBuffer", &gBufferMode, gBufferLabels,
                     static_cast<int>(std::size(gBufferLabels)))) {
        RenderSettingsPatch patch;
        patch.gBufferDebugView =
            static_cast<GBufferDebugView>(gBufferMode);
        if (patch.gBufferDebugView != GBufferDebugView::None) {
            patch.surfaceDebugView = SurfaceDebugView::None;
            patch.deferredLightingDebugView =
                DeferredLightingDebugView::None;
            patch.screenSpaceDebugView = ScreenSpaceDebugView::None;
        }
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();

    if (snapshot.gBuffer.supported) {
        editor::statusIndicator(
            snapshot.gBuffer.active ? "GBuffer active" : "GBuffer inactive",
            snapshot.gBuffer.active ? editor::StatusTone::Success
                                    : editor::StatusTone::Neutral);
        ImGui::TextDisabled("Extent: %u x %u, draws: %u",
                            snapshot.gBuffer.extent.width,
                            snapshot.gBuffer.extent.height,
                            snapshot.gBuffer.drawCount);
        ImGui::TextDisabled("Resident: %.2f MiB",
                            static_cast<double>(
                                snapshot.gBuffer.residentBytes) /
                                (1024.0 * 1024.0));
    } else {
        editor::statusIndicator("GBuffer unavailable",
                                editor::StatusTone::Warning,
                                snapshot.gBuffer.unavailableReason.c_str());
    }

    ImGui::SeparatorText("Deferred Lighting Parity");
    constexpr const char *deferredLabels[] = {
        "None", "Final Color", "Baseline Diffuse", "Baseline Specular",
        "Forward Difference"};
    int deferredMode = static_cast<int>(
        snapshot.features.requested.deferredLightingDebugView);
    ImGui::BeginDisabled(!featureSnapshot.support.deferredLighting.supported);
    if (ImGui::Combo("Debug View##DeferredLighting", &deferredMode,
                     deferredLabels,
                     static_cast<int>(std::size(deferredLabels)))) {
        RenderSettingsPatch patch;
        patch.deferredLightingDebugView =
            static_cast<DeferredLightingDebugView>(deferredMode);
        if (patch.deferredLightingDebugView !=
            DeferredLightingDebugView::None) {
            patch.surfaceDebugView = SurfaceDebugView::None;
            patch.gBufferDebugView = GBufferDebugView::None;
            patch.screenSpaceDebugView = ScreenSpaceDebugView::None;
        }
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();

    if (snapshot.deferredLighting.supported) {
        editor::statusIndicator(
            snapshot.deferredLighting.active ? "Deferred lighting active"
                                             : "Deferred lighting inactive",
            snapshot.deferredLighting.active ? editor::StatusTone::Success
                                             : editor::StatusTone::Neutral);
        ImGui::TextDisabled("Extent: %u x %u, dispatch: %u x %u",
                            snapshot.deferredLighting.extent.width,
                            snapshot.deferredLighting.extent.height,
                            snapshot.deferredLighting.dispatchX,
                            snapshot.deferredLighting.dispatchY);
        ImGui::TextDisabled(
            "Resident: %.2f MiB",
            static_cast<double>(snapshot.deferredLighting.residentBytes) /
                (1024.0 * 1024.0));
    } else {
        editor::statusIndicator(
            "Deferred lighting unavailable", editor::StatusTone::Warning,
            snapshot.deferredLighting.unavailableReason.c_str());
    }
}

void RenderSettingsPanel::drawVisibility(
    const RenderSettingsPanelSnapshot &snapshot,
    const RenderSettingsPanelActions &actions) const {
    const RenderSettingsSnapshot featureSnapshot =
        snapshot.features;
    bool frustumEnabled = snapshot.features.requested.culling.frustumEnabled;
    if (ImGui::Checkbox("Camera Frustum", &frustumEnabled)) {
        RenderSettingsPatch patch;
        patch.frustumCullingEnabled = frustumEnabled;
        applyPatch(actions, patch);
    }

    bool distanceEnabled = snapshot.features.requested.culling.distanceEnabled;
    if (ImGui::Checkbox("Max Draw Distance", &distanceEnabled)) {
        RenderSettingsPatch patch;
        patch.distanceCullingEnabled = distanceEnabled;
        applyPatch(actions, patch);
    }
    ImGui::BeginDisabled(!snapshot.features.requested.culling.distanceEnabled);
    float maxDrawDistance = snapshot.features.requested.culling.maxDrawDistance;
    if (ImGui::DragFloat("Distance", &maxDrawDistance, 1.0f, 0.1f,
                         1000000.0f, "%.1f")) {
        RenderSettingsPatch patch;
        patch.maxDrawDistance = maxDrawDistance;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();

    bool smallObjectEnabled = snapshot.features.requested.culling.smallObjectEnabled;
    if (ImGui::Checkbox("Small Objects", &smallObjectEnabled)) {
        RenderSettingsPatch patch;
        patch.smallObjectCullingEnabled = smallObjectEnabled;
        applyPatch(actions, patch);
    }
    ImGui::BeginDisabled(!snapshot.features.requested.culling.smallObjectEnabled);
    float minimumPixels = snapshot.features.requested.culling.minProjectedSizePixels;
    if (ImGui::DragFloat("Minimum Size", &minimumPixels, 0.1f, 0.0f,
                         256.0f, "%.1f px")) {
        RenderSettingsPatch patch;
        patch.minProjectedSizePixels = minimumPixels;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Shadow Casters");
    bool shadowCullingEnabled =
        snapshot.features.requested.culling.shadowCullingEnabled;
    if (ImGui::Checkbox("Cull Shadow Casters", &shadowCullingEnabled)) {
        RenderSettingsPatch patch;
        patch.shadowCullingEnabled = shadowCullingEnabled;
        applyPatch(actions, patch);
    }
    ImGui::BeginDisabled(!snapshot.features.requested.culling.shadowCullingEnabled);
    float shadowDistance = snapshot.features.requested.culling.shadowDistance;
    if (ImGui::DragFloat("Shadow Distance", &shadowDistance, 1.0f,
                         kMinDirectionalShadowDistance,
                         kMaxDirectionalShadowDistance, "%.1f")) {
        RenderSettingsPatch patch;
        patch.shadowDistance = shadowDistance;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("GPU Occlusion");
    const OcclusionCullingStatus status =
        snapshot.occlusion;
    ImGui::BeginDisabled(
        !featureSnapshot.support.occlusionCulling.supported);
    bool occlusionEnabled = snapshot.features.requested.culling.occlusionEnabled;
    if (ImGui::Checkbox("Hi-Z Occlusion", &occlusionEnabled)) {
        RenderSettingsPatch patch;
        patch.occlusionCullingEnabled = occlusionEnabled;
        applyPatch(actions, patch);
    }
    ImGui::BeginDisabled(!snapshot.features.requested.culling.occlusionEnabled);
    int minimumCandidates = static_cast<int>(
        snapshot.features.requested.culling.occlusionMinCandidates);
    if (ImGui::DragInt("Minimum Candidates", &minimumCandidates, 1.0f,
                       0, 65536)) {
        RenderSettingsPatch patch;
        patch.occlusionMinCandidates =
            static_cast<uint32_t>(std::max(minimumCandidates, 0));
        applyPatch(actions, patch);
    }
    float depthBias = snapshot.features.requested.culling.occlusionDepthBias;
    if (ImGui::DragFloat("Depth Bias", &depthBias, 0.00005f, 0.0f, 0.05f,
                         "%.5f")) {
        RenderSettingsPatch patch;
        patch.occlusionDepthBias = depthBias;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (featureSnapshot.support.occlusionCulling.supported) {
        editor::statusIndicator(
            featureSnapshot.runtime.occlusionCullingActive
                ? "Occlusion active"
                : "Occlusion inactive",
            featureSnapshot.runtime.occlusionCullingActive
                ? editor::StatusTone::Success
                : editor::StatusTone::Neutral);
    } else {
        editor::statusIndicator("Occlusion unavailable",
                                editor::StatusTone::Warning,
                                featureSnapshot.support.occlusionCulling
                                    .unavailableReason.c_str());
    }

    if (ImGui::TreeNodeEx("Culling Statistics",
                          ImGuiTreeNodeFlags_DefaultOpen)) {
        const VisibilityCpuStatistics &stats = snapshot.visibilityStats;
        ImGui::Text("Source: %u  Visible: %u", stats.sourceDraws,
                    stats.cameraVisible);
        ImGui::Text("Camera culled: %u frustum, %u distance, %u small",
                    stats.frustumCulled, stats.distanceCulled,
                    stats.smallObjectCulled);
        ImGui::Text("Camera queues: %u opaque, %u transparent",
                    stats.cameraOpaque, stats.cameraTransparent);
        ImGui::Text("Invalid bounds: %u", stats.invalidBounds);
        ImGui::Text("Shadow: %u / %u visible, %u culled",
                    stats.shadowVisible, stats.shadowCandidates,
                    stats.shadowCulled);
        for (uint32_t cascade = 0; cascade < kCsmCascadeCount; ++cascade) {
            ImGui::Text("CSM %u: %u / %u draws, %u culled", cascade,
                        stats.directionalShadowDraws[cascade],
                        stats.directionalShadowCandidates[cascade],
                        stats.directionalShadowCulled[cascade]);
        }
        uint32_t pointShadowDraws = 0;
        for (uint32_t count : stats.pointShadowDraws)
            pointShadowDraws += count;
        uint32_t spotShadowDraws = 0;
        for (uint32_t count : stats.spotShadowDraws)
            spotShadowDraws += count;
        ImGui::Text("Punctual shadow draws: %u point, %u spot",
                    pointShadowDraws, spotShadowDraws);
        ImGui::Text("Depth draws: %u", stats.depthPrepassDraws);
        ImGui::Text("GPU occluded: %u / %u",
                    status.completed.occluded,
                    status.completed.candidates);
        if (status.latestUncullable != 0)
            ImGui::Text("GPU uncullable: %u", status.latestUncullable);
        if (status.completed.frameSerial != 0) {
            ImGui::TextDisabled("GPU result frame: %llu",
                                static_cast<unsigned long long>(
                                    status.completed.frameSerial));
        }
        ImGui::Text("Hi-Z mips: %u", status.hiZMipLevels);
        ImGui::Text("Indirect capacity: %u / %u",
                    status.indirectCapacities[0],
                    status.indirectCapacities[1]);
        ImGui::Text("Visibility buffers: %.2f KiB",
                    static_cast<double>(status.allocatedBytes) / 1024.0);
        ImGui::TreePop();
    }
}

void RenderSettingsPanel::drawLighting(
    const RenderSettingsPanelSnapshot &snapshot,
    const RenderSettingsPanelActions &actions) const {
    const RenderSettings &settings = snapshot.features.requested;

    bool shadowsEnabled = settings.shadowsEnabled;
    if (ImGui::Checkbox("Shadows", &shadowsEnabled)) {
        RenderSettingsPatch patch;
        patch.shadowsEnabled = shadowsEnabled;
        applyPatch(actions, patch);
    }
    if (snapshot.advanced) {
        ImGui::BeginDisabled(!settings.shadowsEnabled);
        if (ImGui::TreeNodeEx("Shadow Tuning",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
            float receiverBias = settings.shadowReceiverBias;
            if (ImGui::DragFloat("Receiver Bias", &receiverBias, 0.00005f,
                                 0.0f, 0.05f, "%.5f")) {
                RenderSettingsPatch patch;
                patch.shadowReceiverBias = receiverBias;
                applyPatch(actions, patch);
            }
            float pointBias = settings.pointShadowReceiverBiasWorld;
            if (ImGui::DragFloat("Point Bias (World)", &pointBias, 0.001f,
                                 0.0f, 1.0f, "%.3f")) {
                RenderSettingsPatch patch;
                patch.pointShadowReceiverBiasWorld = pointBias;
                applyPatch(actions, patch);
            }
            float constantBias = settings.shadowConstantBias;
            if (ImGui::DragFloat("Constant Bias", &constantBias, 0.05f,
                                 0.0f, 10.0f)) {
                RenderSettingsPatch patch;
                patch.shadowConstantBias = constantBias;
                applyPatch(actions, patch);
            }
            float slopeBias = settings.shadowSlopeBias;
            if (ImGui::DragFloat("Slope Bias", &slopeBias, 0.05f, 0.0f,
                                 10.0f)) {
                RenderSettingsPatch patch;
                patch.shadowSlopeBias = slopeBias;
                applyPatch(actions, patch);
            }
            int maxPoint = static_cast<int>(settings.maxPointShadowLights);
            if (ImGui::SliderInt("Max Point Shadows", &maxPoint, 0,
                                 static_cast<int>(kMaxPointShadowLights))) {
                RenderSettingsPatch patch;
                patch.maxPointShadowLights =
                    static_cast<uint32_t>(maxPoint);
                applyPatch(actions, patch);
            }
            float pointDistance = settings.pointShadowDistance;
            if (ImGui::DragFloat("Point Shadow Distance", &pointDistance,
                                 1.0f, kMinPunctualShadowDistance,
                                 kMaxPunctualShadowDistance, "%.1f")) {
                RenderSettingsPatch patch;
                patch.pointShadowDistance = pointDistance;
                applyPatch(actions, patch);
            }
            int maxSpot = static_cast<int>(settings.maxSpotShadowLights);
            if (ImGui::SliderInt("Max Spot Shadows", &maxSpot, 0,
                                 static_cast<int>(kMaxSpotShadowLights))) {
                RenderSettingsPatch patch;
                patch.maxSpotShadowLights = static_cast<uint32_t>(maxSpot);
                applyPatch(actions, patch);
            }
            float spotDistance = settings.spotShadowDistance;
            if (ImGui::DragFloat("Spot Shadow Distance", &spotDistance,
                                 1.0f, kMinPunctualShadowDistance,
                                 kMaxPunctualShadowDistance, "%.1f")) {
                RenderSettingsPatch patch;
                patch.spotShadowDistance = spotDistance;
                applyPatch(actions, patch);
            }
            ImGui::TextDisabled("Active: %u point, %u spot",
                                snapshot.lightStats.pointShadowLights,
                                snapshot.lightStats.spotShadowLights);
            for (uint32_t cascade = 0; cascade < kCsmCascadeCount;
                 ++cascade) {
                const CsmCascadeDiagnostics &diagnostics =
                    snapshot.lightStats.csmCascades[cascade];
                ImGui::TextDisabled(
                    "CSM %u: %.2f-%.2f, blend %.2f", cascade,
                    diagnostics.nearDistance, diagnostics.splitDistance,
                    diagnostics.blendStartDistance);
                ImGui::TextDisabled(
                    "  radius %.2f, texel %.5f, draws %u/%u%s",
                    diagnostics.stableRadius,
                    diagnostics.worldUnitsPerTexel,
                    snapshot.visibilityStats
                        .directionalShadowDraws[cascade],
                    snapshot.visibilityStats
                        .directionalShadowCandidates[cascade],
                    diagnostics.valid ? "" : ", invalid");
            }
            for (const PunctualShadowSelection &selection :
                 snapshot.lightStats.pointShadowSelections) {
                uint32_t casterDraws = 0;
                for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
                    casterDraws += snapshot.visibilityStats.pointShadowDraws[
                        selection.slot * kPointShadowFaceCount + face];
                const std::string &label = selection.name.empty()
                                               ? selection.stableKey
                                               : selection.name;
                ImGui::BulletText("Point [%u] %s, score %.2f, %u draws%s",
                                  selection.slot, label.c_str(),
                                  selection.score, casterDraws,
                                  selection.focused ? ", selected"
                                                    : selection.retained
                                                          ? ", retained"
                                                          : "");
            }
            for (const PunctualShadowSelection &selection :
                 snapshot.lightStats.spotShadowSelections) {
                const std::string &label = selection.name.empty()
                                               ? selection.stableKey
                                               : selection.name;
                ImGui::BulletText(
                    "Spot [%u] %s, score %.2f, %u draws%s", selection.slot,
                    label.c_str(), selection.score,
                    snapshot.visibilityStats.spotShadowDraws[selection.slot],
                    selection.focused ? ", selected"
                                      : selection.retained ? ", retained"
                                                           : "");
            }
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Environment");
    ImGui::BeginDisabled(!snapshot.environmentIblSupported);
    if (ImGui::BeginCombo("Environment",
                          snapshot.selectedEnvironmentName.c_str())) {
        const bool noneSelected = snapshot.selectedEnvironmentId.empty();
        if (ImGui::Selectable("None", noneSelected) &&
            actions.setEnvironment)
            actions.setEnvironment("None");
        if (noneSelected)
            ImGui::SetItemDefaultFocus();
        for (const RenderEnvironmentOption &environment :
             snapshot.environments) {
            const bool selected =
                snapshot.selectedEnvironmentId == environment.id;
            if (ImGui::Selectable(environment.displayName.c_str(), selected) &&
                actions.setEnvironment)
                actions.setEnvironment(environment.id);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    bool iblEnabled = settings.iblEnabled;
    if (ImGui::Checkbox("Image-Based Lighting", &iblEnabled)) {
        RenderSettingsPatch patch;
        patch.iblEnabled = iblEnabled;
        applyPatch(actions, patch);
    }
    bool skyboxEnabled = settings.skyboxEnabled;
    if (ImGui::Checkbox("Skybox", &skyboxEnabled)) {
        RenderSettingsPatch patch;
        patch.skyboxEnabled = skyboxEnabled;
        applyPatch(actions, patch);
    }
    float environmentIntensity = settings.environmentIntensity;
    if (ImGui::DragFloat("Environment Intensity", &environmentIntensity,
                         0.02f, 0.0f, 100.0f)) {
        RenderSettingsPatch patch;
        patch.environmentIntensity = environmentIntensity;
        applyPatch(actions, patch);
    }
    float environmentRotation = settings.environmentRotationRadians;
    if (ImGui::SliderAngle("Environment Rotation", &environmentRotation,
                           -180.0f, 180.0f, "%.1f deg",
                           ImGuiSliderFlags_AlwaysClamp)) {
        RenderSettingsPatch patch;
        patch.environmentRotationRadians = environmentRotation;
        applyPatch(actions, patch);
    }
    ImGui::EndDisabled();
    if (!snapshot.environmentIblSupported) {
        editor::statusIndicator(
            "IBL unavailable", editor::StatusTone::Warning,
            "RGBA16F cubemap and RG16F linear filtering are required.");
    } else if (snapshot.environmentLoad &&
               snapshot.environmentLoad->active) {
        ImGui::Text("Loading: %s (%u/%u)",
                    snapshot.environmentLoad->state.c_str(),
                    snapshot.environmentLoad->uploadedImages,
                    snapshot.environmentLoad->totalImages);
        if (ImGui::Button("Cancel Environment Load") &&
            actions.cancelEnvironmentLoad)
            actions.cancelEnvironmentLoad(snapshot.environmentLoad->taskId);
    } else {
        editor::statusIndicator(
            snapshot.environmentReady ? "Environment ready"
                                      : "Environment not loaded",
            snapshot.environmentReady ? editor::StatusTone::Success
                                      : editor::StatusTone::Neutral);
    }
    if (!snapshot.environmentError.empty())
        ImGui::TextWrapped("Environment error: %s",
                           snapshot.environmentError.c_str());

    ImGui::SeparatorText("Sky Atmosphere");
    editor::statusIndicator(
        snapshot.atmosphere.active
            ? "Atmosphere active"
            : snapshot.atmosphere.componentPresent ? "Atmosphere inactive"
                                                   : "No atmosphere",
        snapshot.atmosphere.active ? editor::StatusTone::Success
                                   : editor::StatusTone::Neutral,
        snapshot.atmosphere.unavailableReason.empty()
            ? nullptr
            : snapshot.atmosphere.unavailableReason.c_str());
    if (snapshot.atmosphere.componentPresent) {
        ImGui::TextDisabled("Camera altitude %.3f km",
                            snapshot.atmosphere.cameraAltitudeKm);
    }

    ImGui::SeparatorText("Ambient");
    glm::vec3 ambientColor = snapshot.ambientColor;
    if (ImGui::ColorEdit3("Ambient Color", &ambientColor.x) &&
        actions.setAmbientColor)
        actions.setAmbientColor(ambientColor);
    float ambientIntensity = snapshot.ambientIntensity;
    if (ImGui::DragFloat("Ambient Intensity", &ambientIntensity, 0.01f,
                         0.0f, 10.0f) &&
        actions.setAmbientIntensity)
        actions.setAmbientIntensity(ambientIntensity);

    const size_t effectiveLightCount = static_cast<size_t>(std::count_if(
        snapshot.sceneLights.begin(), snapshot.sceneLights.end(),
        isEffectiveSceneLight));
    if (snapshot.advanced && ImGui::TreeNodeEx("Light Diagnostics")) {
        ImGui::Text("Scene lights: %zu", snapshot.sceneLights.size());
        ImGui::Text("Active: %zu  Uploaded: %u / %u", effectiveLightCount,
                    snapshot.lightStats.totalLights,
                    snapshot.lightBuffer.limit);
        ImGui::Text("Directional %u  Point %u  Spot %u",
                    snapshot.lightStats.directionalLights,
                    snapshot.lightStats.pointLights,
                    snapshot.lightStats.spotLights);
        const ClusteredLightingStatus &clusters =
            snapshot.clusteredLighting;
        if (clusters.supported) {
            ImGui::Text("Clustered: %s  %ux%ux%u @ %upx",
                        clusters.active ? "Active" : "Inactive",
                        clusters.tilesX, clusters.tilesY,
                        clusters.depthSlices, clusters.tileSize);
            ImGui::Text("Cluster refs: %u, avg %.1f, max %u",
                        clusters.totalLightReferences,
                        clusters.averageLightReferences,
                        clusters.maxLightReferences);
            ImGui::Text("Overflow: %u clusters, %u references",
                        clusters.overflowClusters,
                        clusters.overflowLightReferences);
        } else {
            ImGui::TextDisabled("Clustered lighting unavailable: %s",
                                clusters.unavailableReason.c_str());
        }
        for (size_t index = 0; index < snapshot.sceneLights.size(); ++index) {
            const SceneLight &light = snapshot.sceneLights[index];
            const std::string label = std::to_string(index) + "  " +
                                      (light.debugName.empty()
                                           ? "Light"
                                           : light.debugName);
            if (!ImGui::TreeNode(label.c_str()))
                continue;
            ImGui::Text("Type: %s", lightTypeName(light.type));
            ImGui::Text("Intensity: %.3f %s", light.intensity,
                        lightIntensityUnit(light.type));
            ImGui::Text("Color: %.3f %.3f %.3f", light.color.r,
                        light.color.g, light.color.b);
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    if (effectiveLightCount == 0) {
        ImGui::SeparatorText("Fallback Sun");
        float azimuth = 0.0f;
        float elevation = 0.0f;
        sunAnglesFromDirection(snapshot.fallbackSunDirection, azimuth,
                               elevation);
        bool directionChanged = ImGui::SliderAngle(
            "Sun Azimuth", &azimuth, -180.0f, 180.0f, "%.1f deg",
            ImGuiSliderFlags_AlwaysClamp);
        directionChanged |= ImGui::SliderAngle(
            "Sun Elevation", &elevation, -89.0f, 89.0f, "%.1f deg",
            ImGuiSliderFlags_AlwaysClamp);
        if (directionChanged && actions.setFallbackSunDirection)
            actions.setFallbackSunDirection(
                sunDirectionFromAngles(azimuth, elevation));
        glm::vec3 sunColor = snapshot.fallbackSunColor;
        if (ImGui::ColorEdit3("Sun Color", &sunColor.x) &&
            actions.setFallbackSunColor)
            actions.setFallbackSunColor(sunColor);
        float sunIntensity = snapshot.fallbackSunIntensity;
        if (ImGui::DragFloat("Sun Intensity", &sunIntensity, 0.05f, 0.0f,
                             20.0f) &&
            actions.setFallbackSunIntensity)
            actions.setFallbackSunIntensity(sunIntensity);
    }
}

} // namespace vkr
