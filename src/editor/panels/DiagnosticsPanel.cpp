#include "DiagnosticsPanel.h"

#include "editor/EditorWidgets.h"

#include <RuntimeFeatures.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace vkr {
namespace {

double bytesToMiB(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double signedBytesToMiB(int64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

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

} // namespace

void DiagnosticsPanel::draw(const DiagnosticsPanelSnapshot &snapshot,
                            const DiagnosticsPanelActions &actions) {
    fpsHistory_[historyCursor_] = snapshot.fps;
    gpuHistory_[historyCursor_] = std::max(snapshot.gpuFrameMs, 0.0f);
    historyCursor_ = (historyCursor_ + 1) % kHistorySize;
    historyCount_ = std::min(historyCount_ + 1, kHistorySize);

    if (!ImGui::BeginTabBar("BottomDrawerTabs"))
        return;
    if (ImGui::BeginTabItem("Tasks")) {
        ImGui::PushID("TasksTab");
        if (snapshot.sceneLoadActive && actions.drawSceneLoad)
            actions.drawSceneLoad();
        if (actions.drawTasks)
            actions.drawTasks();
        ImGui::PopID();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Performance")) {
        ImGui::PushID("PerformanceTab");
        drawPerformance(snapshot, actions);
        ImGui::PopID();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Load Stats")) {
        ImGui::PushID("LoadStatsTab");
        drawLoadStats(snapshot);
        ImGui::PopID();
        ImGui::EndTabItem();
    }
    if (snapshot.captureAvailable && ImGui::BeginTabItem("Capture")) {
        ImGui::PushID("CaptureTab");
        if (actions.drawCapture)
            actions.drawCapture();
        ImGui::PopID();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void DiagnosticsPanel::drawPerformance(
    const DiagnosticsPanelSnapshot &snapshot,
    const DiagnosticsPanelActions &actions) {
    if (editor::beginPropertyGrid("PerformanceSummary", 0.36f)) {
        editor::propertyLabel("FPS");
        ImGui::Text("%.1f", snapshot.fps);
        editor::propertyLabel("Input Mode");
        ImGui::TextUnformatted(snapshot.cameraDragging ? "CameraDrag" : "UI");
        editor::propertyLabel("Objects");
        ImGui::Text("%zu", snapshot.renderableCount);
        editor::propertyLabel("Tracy");
        if (snapshot.tracyCompiled) {
            editor::statusIndicator(
                snapshot.tracyConnected ? "Connected" : "Waiting",
                snapshot.tracyConnected ? editor::StatusTone::Success
                                        : editor::StatusTone::Neutral,
                snapshot.tracyGpuAvailable
                    ? "Vulkan GPU profiling available"
                    : "Vulkan GPU profiling unavailable");
        } else {
            ImGui::TextDisabled("Not compiled");
        }
        editor::endPropertyGrid();
    }

    if (historyCount_ > 1) {
        std::array<float, kHistorySize> fps{};
        std::array<float, kHistorySize> gpu{};
        const size_t start =
            (historyCursor_ + kHistorySize - historyCount_) % kHistorySize;
        for (size_t index = 0; index < historyCount_; ++index) {
            const size_t source = (start + index) % kHistorySize;
            fps[index] = fpsHistory_[source];
            gpu[index] = gpuHistory_[source];
        }
        const float maxFps = std::max(
            60.0f,
            *std::max_element(fps.begin(), fps.begin() + historyCount_));
        const float maxGpu = std::max(
            1.0f,
            *std::max_element(gpu.begin(), gpu.begin() + historyCount_));
        ImGui::PlotLines("FPS History", fps.data(),
                         static_cast<int>(historyCount_), 0, nullptr, 0.0f,
                         maxFps * 1.1f, ImVec2(0.0f, 54.0f));
        ImGui::PlotLines("GPU ms History", gpu.data(),
                         static_cast<int>(historyCount_), 0, nullptr, 0.0f,
                         maxGpu * 1.1f, ImVec2(0.0f, 54.0f));
    }

    if (ImGui::CollapsingHeader("Rendering Path",
                                ImGuiTreeNodeFlags_DefaultOpen) &&
        editor::beginPropertyGrid("RenderingPathSummary", 0.42f)) {
        editor::propertyLabel("Requested");
        ImGui::TextUnformatted(
            renderPathRequestName(snapshot.renderPath.requested).data());
        editor::propertyLabel("Active");
        ImGui::TextUnformatted(
            renderPathModeName(snapshot.renderPath.active).data());
        editor::propertyLabel("View Mode");
        ImGui::TextUnformatted(snapshot.renderPath.viewMode.c_str());
        editor::propertyLabel("Opaque MRTs");
        ImGui::Text("%u%s",
                    snapshot.renderPath.products.colorAttachmentCount,
                    snapshot.renderPath.products.multisampled ? " MSAA"
                                                              : "");
        editor::propertyLabel("Sampled Surface");
        ImGui::Text("D:%s N:%s M:%s",
                    snapshot.renderPath.products.sampledDepth ? "yes" : "no",
                    snapshot.renderPath.products.normalRoughness ? "yes"
                                                                : "no",
                    snapshot.renderPath.products.motion ? "yes" : "no");
        editor::propertyLabel("Lighting Baselines");
        ImGui::Text("Diffuse:%s Specular:%s",
                    snapshot.renderPath.products.baselineDiffuse ? "yes"
                                                                : "no",
                    snapshot.renderPath.products.baselineSpecular ? "yes"
                                                                 : "no");
        editor::propertyLabel("Deferred Contract");
        ImGui::Text("%s, %u attachments, %u B/px",
                    snapshot.renderPath.gBuffer.implemented ? "Implemented"
                                                            : "Defined",
                    snapshot.renderPath.gBuffer.attachmentCount,
                    snapshot.renderPath.gBuffer.nominalBytesPerPixel);
        editor::propertyLabel("GBuffer Runtime");
        if (snapshot.gBuffer.supported) {
            ImGui::Text("%s, %ux%u, %u draws, %.2f MiB",
                        snapshot.gBuffer.active ? "Active" : "Resident",
                        snapshot.gBuffer.extent.width,
                        snapshot.gBuffer.extent.height,
                        snapshot.gBuffer.drawCount,
                        static_cast<double>(snapshot.gBuffer.residentBytes) /
                            (1024.0 * 1024.0));
        } else {
            ImGui::TextUnformatted("Unavailable");
        }
        editor::propertyLabel("Deferred Lighting");
        if (snapshot.deferredLighting.supported) {
            ImGui::Text("%s, %ux%u, %ux%u groups, %.2f MiB",
                        snapshot.deferredLighting.active ? "Active"
                                                         : "Resident",
                        snapshot.deferredLighting.extent.width,
                        snapshot.deferredLighting.extent.height,
                        snapshot.deferredLighting.dispatchX,
                        snapshot.deferredLighting.dispatchY,
                        static_cast<double>(
                            snapshot.deferredLighting.residentBytes) /
                            (1024.0 * 1024.0));
        } else {
            ImGui::TextUnformatted("Unavailable");
        }
        editor::propertyLabel("Clustered Lighting");
        const ClusteredLightingStatus &clusters =
            snapshot.clusteredLighting;
        if (clusters.supported) {
            ImGui::Text("%s, %ux%ux%u, %u refs, %.1f avg, %u/%u overflow, %.2f MiB",
                        clusters.active ? "Active" : "Inactive",
                        clusters.tilesX, clusters.tilesY,
                        clusters.depthSlices,
                        clusters.totalLightReferences,
                        clusters.averageLightReferences,
                        clusters.overflowClusters,
                        clusters.overflowLightReferences,
                        bytesToMiB(clusters.allocatedBytes));
        } else {
            ImGui::TextUnformatted("Unavailable");
        }
        editor::endPropertyGrid();
    }

    if (ImGui::CollapsingHeader("GPU Pass Timings",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const GpuPassTimings &timings = snapshot.gpuTimings;
        if (!build::kGpuProfiling) {
            ImGui::TextUnformatted("Not compiled");
        } else if (!timings.available) {
            ImGui::TextUnformatted("Unavailable");
        } else if (editor::beginPropertyGrid("GpuPassBreakdown", 0.42f)) {
            for (const GpuPassTiming &pass : timings.passes) {
                editor::propertyLabel(pass.name.c_str());
                const float fraction =
                    timings.totalMs > 0.0
                        ? static_cast<float>(pass.milliseconds /
                                             timings.totalMs)
                        : 0.0f;
                char overlay[48]{};
                std::snprintf(overlay, sizeof(overlay), "%.3f ms",
                              pass.milliseconds);
                ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f),
                                   ImVec2(-1.0f, 0.0f), overlay);
            }
            editor::propertyLabel("Total");
            ImGui::Text("%.3f ms", timings.totalMs);
            editor::endPropertyGrid();
        }
    }

    if (!ImGui::CollapsingHeader("Render Graph",
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const RenderGraphDiagnostics &graph = snapshot.renderGraph;
    if (editor::beginPropertyGrid("RenderGraphSummary", 0.46f)) {
        editor::propertyLabel("Topology");
        ImGui::Text("%016llx",
                    static_cast<unsigned long long>(graph.topologyHash));
        editor::propertyLabel("Passes");
        ImGui::Text("%u active / %u culled", graph.activePasses,
                    graph.culledPasses);
        editor::propertyLabel("Dependencies");
        ImGui::Text("%u", graph.dependencyEdges);
        editor::propertyLabel("Auto Barriers");
        ImGui::Text("%u (%u layout, %u hazard)", graph.automaticBarriers,
                    graph.layoutBarriers, graph.hazardBarriers);
        editor::propertyLabel("Image Memory");
        ImGui::Text("%.1f / %.1f MiB active/resident",
                    bytesToMiB(graph.activeImageBytes),
                    bytesToMiB(graph.residentImageBytes));
        editor::propertyLabel("Declared / Retiring");
        ImGui::Text("%.1f / %.1f MiB", bytesToMiB(graph.logicalImageBytes),
                    bytesToMiB(graph.retiringImageBytes));
        editor::endPropertyGrid();
    }
    if (ImGui::TreeNode("Execution Order")) {
        for (uint32_t index = 0; index < graph.executionOrder.size(); ++index)
            ImGui::Text("%02u  %s", index,
                        graph.executionOrder[index].c_str());
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Culled Passes")) {
        for (const std::string &name : graph.culledNames)
            ImGui::TextDisabled("%s", name.c_str());
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Resources")) {
        for (const auto &resource : graph.resources) {
            ImGui::PushID(static_cast<int>(resource.index));
            if (ImGui::TreeNode("Resource", "%s  [%s, v%u]",
                                resource.name.c_str(),
                                resource.lifetime.c_str(),
                                resource.versions)) {
                ImGui::Text("Memory: %.2f MiB",
                            bytesToMiB(resource.residentBytes));
                ImGui::Text("Layout: %d -> %d",
                            static_cast<int>(resource.initialLayout),
                            static_cast<int>(resource.finalLayout));
                for (const std::string &producer : resource.producers)
                    ImGui::BulletText("Write: %s", producer.c_str());
                for (const std::string &consumer : resource.consumers)
                    ImGui::BulletText("Read: %s", consumer.c_str());
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Buffers")) {
        for (const auto &buffer : graph.buffers) {
            ImGui::PushID(static_cast<int>(buffer.index));
            if (ImGui::TreeNode("Buffer", "%s  [%s, v%u]",
                                buffer.name.c_str(), buffer.lifetime.c_str(),
                                buffer.versions)) {
                if (buffer.declaredRangeBytes > 0)
                    ImGui::Text("Declared range: %.2f KiB",
                                buffer.declaredRangeBytes / 1024.0);
                else
                    ImGui::TextDisabled("Declared range: whole buffer");
                for (const std::string &producer : buffer.producers)
                    ImGui::BulletText("Write: %s", producer.c_str());
                for (const std::string &consumer : buffer.consumers)
                    ImGui::BulletText("Read: %s", consumer.c_str());
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (ImGui::Button("Export JSON") && actions.exportRenderGraphJson)
        actions.exportRenderGraphJson();
    ImGui::SameLine();
    if (ImGui::Button("Export DOT") && actions.exportRenderGraphDot)
        actions.exportRenderGraphDot();
}

void DiagnosticsPanel::drawLoadStats(
    const DiagnosticsPanelSnapshot &snapshot) {
    if (snapshot.lastSceneLoad &&
        ImGui::CollapsingHeader("Last Scene Load",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const SceneLoadStats &stats = *snapshot.lastSceneLoad;
        const ResourceLoadStats &resources = stats.resources;
        if (editor::beginPropertyGrid("SceneLoadSummary", 0.44f)) {
            editor::propertyLabel("Scene");
            ImGui::TextUnformatted(stats.sceneName.c_str());
            editor::propertyLabel("Status");
            ImGui::TextUnformatted(stats.success ? "Success" : "Failed");
            editor::propertyLabel("Texture Limit");
            ImGui::TextUnformatted(textureLimitLabel(stats.maxTextureSize));
            editor::propertyLabel("Total");
            ImGui::Text("%.2f ms", stats.totalMs);
            editor::propertyLabel("CPU Prepare / GPU Build");
            ImGui::Text("%.2f / %.2f ms", stats.cpuPrepareMs,
                        stats.gpuBuildMs);
            editor::propertyLabel("KTX Read / Transcode");
            ImGui::Text("%.2f / %.2f ms", resources.derivedTextureReadMs,
                        resources.derivedTextureTranscodeMs);
            editor::propertyLabel("Batch Submit / Wait");
            ImGui::Text("%.2f ms", resources.batchSubmitWaitMs);
            editor::endPropertyGrid();
        }
        if (ImGui::TreeNodeEx("Detailed Timing")) {
            ImGui::Text("Device Idle: %.2f ms (%llu calls)",
                        stats.deviceIdleMs,
                        static_cast<unsigned long long>(
                            stats.deviceWaitIdleCalls));
            ImGui::Text("Parse %.2f  Read %.2f  Decode %.2f  Resize %.2f ms",
                        stats.gltfParseMs, stats.textureFileReadMs,
                        stats.textureDecodeMs, resources.textureResizeMs);
            ImGui::Text("Texture upload %.2f  Mesh CPU %.2f  Mesh upload %.2f ms",
                        resources.textureUploadMs, stats.meshCpuMs,
                        resources.meshUploadMs);
            ImGui::Text("Material %.2f  Hierarchy %.2f  Worker wait %.2f ms",
                        stats.materialSetupMs, stats.hierarchyMs,
                        stats.workerQueueWaitMs);
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Resources")) {
            ImGui::Text("Textures: %llu decoded, %llu GPU, %llu resized",
                        static_cast<unsigned long long>(
                            resources.textureDecodeCount),
                        static_cast<unsigned long long>(
                            resources.gpuTextureCount),
                        static_cast<unsigned long long>(
                            resources.resizedTextureCount));
            ImGui::Text("Native BC7 %llu  UASTC %llu  Transcodes %llu",
                        static_cast<unsigned long long>(
                            resources.nativeBc7CacheHits),
                        static_cast<unsigned long long>(
                            resources.basisUastcCacheHits),
                        static_cast<unsigned long long>(
                            resources.basisTranscodeCount));
            ImGui::Text("Meshes %llu  Vertices %llu  Indices %llu",
                        static_cast<unsigned long long>(resources.gpuMeshCount),
                        static_cast<unsigned long long>(resources.vertexCount),
                        static_cast<unsigned long long>(resources.indexCount));
            ImGui::Text("Materials %llu  Primitives %llu",
                        static_cast<unsigned long long>(stats.materialCount),
                        static_cast<unsigned long long>(stats.primitiveCount));
            ImGui::Text("Texture upload / GPU: %.2f / %.2f MiB",
                        bytesToMiB(resources.textureUploadBytes),
                        bytesToMiB(resources.textureGpuBytesEstimated));
            ImGui::Text("Mesh upload: %.2f MiB",
                        bytesToMiB(resources.vertexUploadBytes +
                                   resources.indexUploadBytes));
            ImGui::Text("Batch submits %llu  Fence waits %llu  Peak staging %.2f MiB",
                        static_cast<unsigned long long>(resources.batchSubmits),
                        static_cast<unsigned long long>(resources.fenceWaitCalls),
                        bytesToMiB(resources.peakStagingBytes));
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("VMA Memory")) {
            const int64_t allocationDelta = memoryDelta(
                stats.allocatorAfter.allocationBytes,
                stats.allocatorBefore.allocationBytes);
            const int64_t blockDelta =
                memoryDelta(stats.allocatorAfter.blockBytes,
                            stats.allocatorBefore.blockBytes);
            ImGui::Text("Allocations: %llu -> %llu",
                        static_cast<unsigned long long>(
                            stats.allocatorBefore.allocationCount),
                        static_cast<unsigned long long>(
                            stats.allocatorAfter.allocationCount));
            ImGui::Text("Allocation bytes: %.2f -> %.2f MiB (%+.2f)",
                        bytesToMiB(stats.allocatorBefore.allocationBytes),
                        bytesToMiB(stats.allocatorAfter.allocationBytes),
                        signedBytesToMiB(allocationDelta));
            ImGui::Text("Block bytes: %.2f -> %.2f MiB (%+.2f)",
                        bytesToMiB(stats.allocatorBefore.blockBytes),
                        bytesToMiB(stats.allocatorAfter.blockBytes),
                        signedBytesToMiB(blockDelta));
            ImGui::TreePop();
        }
    } else if (!snapshot.lastSceneLoad) {
        editor::emptyState("No scene load statistics are available.");
    }

    if (ImGui::CollapsingHeader("Material Resources",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const MaterialBindingStatus &status = snapshot.materialBinding;
        ImGui::Text("Requested %s  Active %s",
                    materialBindingModeName(status.requested),
                    materialBindingModeName(status.active));
        if (!status.fallbackReason.empty())
            ImGui::TextWrapped("Fallback: %s", status.fallbackReason.c_str());
        ImGui::Text("Textures %u / %u  Retiring %u", status.activeTextures,
                    status.textureCapacity, status.retiringTextures);
        ImGui::Text("Materials %u / %u  Retiring %u", status.activeMaterials,
                    status.materialCapacity, status.retiringMaterials);
        ImGui::Text("High water: textures %u  materials %u",
                    status.textureHighWaterMark,
                    status.materialHighWaterMark);
        ImGui::Text("Descriptor writes %llu  Slot reuse T/M %llu / %llu",
                    static_cast<unsigned long long>(status.descriptorWrites),
                    static_cast<unsigned long long>(status.textureSlotReuses),
                    static_cast<unsigned long long>(status.materialSlotReuses));
    }

    if (!ImGui::CollapsingHeader("Model Asset Repository",
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const AssetRepositorySnapshot &repository = snapshot.modelRepository;
    ImGui::Text("Ready %llu  Loading %llu  Failed %llu  Retiring %llu",
                static_cast<unsigned long long>(repository.readyCount),
                static_cast<unsigned long long>(repository.loadingCount),
                static_cast<unsigned long long>(repository.failedCount),
                static_cast<unsigned long long>(repository.retiringCount));
    ImGui::Text("CPU prepares %llu  GPU builds %llu  Hits %llu  Coalesced %llu",
                static_cast<unsigned long long>(repository.cpuPrepareStarts),
                static_cast<unsigned long long>(repository.gpuBuildStarts),
                static_cast<unsigned long long>(repository.readyHits),
                static_cast<unsigned long long>(repository.coalescedRequests));
    if (ImGui::BeginTable("ModelAssetRepositoryRecords", 5,
                          ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Model");
        ImGui::TableSetupColumn("Profile");
        ImGui::TableSetupColumn("Gen");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Users");
        ImGui::TableHeadersRow();
        for (const ModelAssetRecordSnapshot &record : repository.records) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(record.key.modelId.value().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(record.key.profileId.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%llu", static_cast<unsigned long long>(
                                    record.generation));
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(modelAssetStateName(record.state));
            if (!record.error.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", record.error.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", static_cast<unsigned long long>(
                                    record.consumerCount));
        }
        ImGui::EndTable();
    }
}

} // namespace vkr
