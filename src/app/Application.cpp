#include "Application.h"
#include "UniformData.h"

#include "control/NamedPipeServerWin32.h"
#include "control/RuntimeCommand.h"
#include "control/RuntimeControlProtocol.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/Log.h"
#include "core/ResourcePoolSelfTest.h"
#include "core/SwapChain.h"
#include "core/UploadContext.h"
#include "core/VulkanContext.h"
#include "render/GuiSystem.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTextureSlot.h"
#include "render/PipelineCache.h"
#include "render/Renderer.h"
#include "scene/SceneFactory.h"
#include "scene/SceneLight.h"
#include "window/InputManager.h"
#include "window/Window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vkr {

namespace {

glm::vec3 normalizeOrFallback(const glm::vec3 &v,
                              const glm::vec3 &fallback) {
    const float len2 = glm::dot(v, v);
    if (len2 <= 1.0e-6f)
        return glm::normalize(fallback);
    return glm::normalize(v);
}

SceneLight makeDefaultSun(const glm::vec3 &direction, const glm::vec3 &color,
                          float intensity) {
    SceneLight light{};
    light.type = LightType::Directional;
    light.directionWS =
        normalizeOrFallback(direction, glm::vec3(0.3f, 0.8f, 0.5f));
    light.color = color;
    light.intensity = std::max(intensity, 0.0f);
    return light;
}

GpuLight makeGpuLight(const SceneLight &light) {
    GpuLight gpu{};
    const glm::vec3 direction = normalizeOrFallback(
        light.directionWS,
        light.type == LightType::Directional ? glm::vec3(0.3f, 0.8f, 0.5f)
                                             : glm::vec3(0.0f, -1.0f, 0.0f));

    float innerConeCos = glm::clamp(light.innerConeCos, -1.0f, 1.0f);
    float outerConeCos = glm::clamp(light.outerConeCos, -1.0f, 1.0f);
    if (light.type == LightType::Spot && innerConeCos < outerConeCos)
        std::swap(innerConeCos, outerConeCos);

    gpu.positionRange =
        glm::vec4(light.positionWS, std::max(light.range, 0.0f));
    gpu.directionInnerCos = glm::vec4(direction, innerConeCos);
    gpu.colorIntensity =
        glm::vec4(glm::max(light.color, glm::vec3(0.0f)),
                  std::max(light.intensity, 0.0f));
    gpu.params =
        glm::vec4(static_cast<float>(static_cast<uint32_t>(light.type)),
                  outerConeCos, 0.0f, 0.0f);
    return gpu;
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

bool isTransparentMaterial(const MaterialParams &params) {
    return params.alphaMode == AlphaMode::Blend ||
           params.transmissionFactor > 0.0f;
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

double bytesToMiB(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double signedBytesToMiB(int64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

bool asciiEqualsIgnoreCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto left = static_cast<unsigned char>(a[i]);
        const auto right = static_cast<unsigned char>(b[i]);
        if (std::tolower(left) != std::tolower(right))
            return false;
    }
    return true;
}

ControlJson allocatorSnapshotToJson(const AllocatorMemorySnapshot &snapshot) {
    return {{"allocationCount", snapshot.allocationCount},
            {"allocationBytes", snapshot.allocationBytes},
            {"blockBytes", snapshot.blockBytes}};
}

ControlJson sceneLoadStatsToJson(const SceneLoadStats &stats) {
    const ResourceLoadStats &r = stats.resources;
    return {
        {"scene", stats.sceneName},
        {"textureLimit", stats.maxTextureSize},
        {"success", stats.success},
        {"timingsMs",
         {{"total", stats.totalMs},
          {"deviceIdle", stats.deviceIdleMs},
          {"teardown", stats.teardownMs},
          {"sceneFactory", stats.sceneFactoryMs},
          {"gltfParse", stats.gltfParseMs},
          {"textureFileRead", stats.textureFileReadMs},
          {"textureDecode", stats.textureDecodeMs},
          {"textureResize", r.textureResizeMs},
          {"textureUpload", r.textureUploadMs},
          {"materialSetup", stats.materialSetupMs},
          {"meshCpu", stats.meshCpuMs},
          {"meshUpload", r.meshUploadMs},
          {"batchSubmitWait", r.batchSubmitWaitMs},
          {"hierarchy", stats.hierarchyMs}}},
        {"counts",
         {{"deviceWaitIdleCalls", stats.deviceWaitIdleCalls},
          {"materials", stats.materialCount},
          {"objects", stats.objectCount},
          {"textureDecodes", r.textureDecodeCount},
          {"gpuTextures", r.gpuTextureCount},
          {"resizedTextures", r.resizedTextureCount},
          {"gpuMeshes", r.gpuMeshCount},
          {"vertices", r.vertexCount},
          {"indices", r.indexCount}}},
        {"bytes",
         {{"encodedSources", r.encodedSourceBytes},
          {"decodedRgba", r.decodedRgbaBytes},
          {"textureUpload", r.textureUploadBytes},
          {"textureGpuEstimated", r.textureGpuBytesEstimated},
          {"vertexUpload", r.vertexUploadBytes},
          {"indexUpload", r.indexUploadBytes},
          {"peakStaging", r.peakStagingBytes}}},
        {"synchronization",
         {{"legacySubmits", r.singleTimeSubmits},
          {"queueWaitIdleCalls", r.queueWaitIdleCalls},
          {"batchSubmits", r.batchSubmits},
          {"fenceWaitCalls", r.fenceWaitCalls}}},
        {"vma",
         {{"before", allocatorSnapshotToJson(stats.allocatorBefore)},
          {"after", allocatorSnapshotToJson(stats.allocatorAfter)},
          {"delta",
           {{"allocationCount",
             memoryDelta(stats.allocatorAfter.allocationCount,
                         stats.allocatorBefore.allocationCount)},
            {"allocationBytes",
             memoryDelta(stats.allocatorAfter.allocationBytes,
                         stats.allocatorBefore.allocationBytes)},
            {"blockBytes", memoryDelta(stats.allocatorAfter.blockBytes,
                                        stats.allocatorBefore.blockBytes)}}}}}};
}

class RuntimeCommandError : public std::runtime_error {
  public:
    RuntimeCommandError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const { return code_; }

  private:
    std::string code_;
};

std::string requiredStringParam(const RuntimeCommand &command,
                                const char *name) {
    if (!command.params.contains(name) ||
        !command.params[name].is_string()) {
        throw RuntimeCommandError(
            "invalid_params",
            std::string("Parameter '") + name + "' must be a string.");
    }
    return command.params[name].get<std::string>();
}

void validateSceneLoadStats(const SceneLoadStats &stats) {
    const double detailedMax =
        std::max({stats.deviceIdleMs, stats.teardownMs, stats.sceneFactoryMs,
                  stats.gltfParseMs, stats.textureFileReadMs,
                  stats.textureDecodeMs, stats.resources.textureResizeMs,
                  stats.resources.textureUploadMs, stats.materialSetupMs,
                  stats.meshCpuMs, stats.resources.meshUploadMs,
                  stats.resources.batchSubmitWaitMs, stats.hierarchyMs});
    if (stats.totalMs + 0.01 < detailedMax) {
        VKR_LOG_WARN("LoadStats",
                     "Scene '{}' has inconsistent timing: total={:.2f}ms, "
                     "largest stage={:.2f}ms.",
                     stats.sceneName, stats.totalMs, detailedMax);
    }
    if (stats.resources.gpuTextureCount > 0 &&
        stats.resources.textureUploadBytes == 0) {
        VKR_LOG_WARN("LoadStats",
                     "Scene '{}' created textures but recorded no texture "
                     "upload bytes.",
                     stats.sceneName);
    }
    if (stats.resources.gpuMeshCount > 0 &&
        stats.resources.vertexUploadBytes == 0) {
        VKR_LOG_WARN("LoadStats",
                     "Scene '{}' created meshes but recorded no vertex "
                     "upload bytes.",
                     stats.sceneName);
    }
    if (stats.resources.batchSubmits != stats.resources.fenceWaitCalls) {
        VKR_LOG_WARN("LoadStats",
                     "Scene '{}' has mismatched batch synchronization: "
                     "submits={}, fence waits={}.",
                     stats.sceneName, stats.resources.batchSubmits,
                     stats.resources.fenceWaitCalls);
    }
}

void logSceneLoadStats(const SceneLoadStats &stats) {
    const int64_t allocationDelta =
        memoryDelta(stats.allocatorAfter.allocationBytes,
                    stats.allocatorBefore.allocationBytes);
    const int64_t blockDelta = memoryDelta(stats.allocatorAfter.blockBytes,
                                           stats.allocatorBefore.blockBytes);

    VKR_LOG_INFO(
        "LoadStats",
        "scene='{}' success={} limit={} total={:.2f}ms factory={:.2f}ms "
        "textures={} meshes={} materials={} objects={} upload={:.2f}MiB "
        "legacySubmits={} batchSubmits={} queueWaits={} fenceWaits={} "
        "vmaAllocationDelta={:.2f}MiB vmaBlockDelta={:.2f}MiB",
        stats.sceneName, stats.success,
        stats.maxTextureSize == 0 ? std::string("Full")
                                  : std::to_string(stats.maxTextureSize),
        stats.totalMs, stats.sceneFactoryMs,
        stats.resources.gpuTextureCount, stats.resources.gpuMeshCount,
        stats.materialCount, stats.objectCount,
        bytesToMiB(stats.resources.textureUploadBytes +
                   stats.resources.vertexUploadBytes +
                   stats.resources.indexUploadBytes),
        stats.resources.singleTimeSubmits,
        stats.resources.batchSubmits,
        stats.resources.queueWaitIdleCalls,
        stats.resources.fenceWaitCalls,
        signedBytesToMiB(allocationDelta), signedBytesToMiB(blockDelta));

    VKR_LOG_DEBUG(
        "LoadStats",
        "timings idle={:.2f}ms teardown={:.2f}ms parse={:.2f}ms "
        "imageRead={:.2f}ms decode={:.2f}ms resize={:.2f}ms "
        "textureUpload={:.2f}ms material={:.2f}ms meshCpu={:.2f}ms "
        "meshUpload={:.2f}ms batchWait={:.2f}ms hierarchy={:.2f}ms; "
        "peakStaging={:.2f}MiB; "
        "textureBytes "
        "encoded={:.2f}MiB decoded={:.2f}MiB baseUpload={:.2f}MiB "
        "gpuEstimated={:.2f}MiB; VMA allocations {} -> {}, bytes "
        "{:.2f}MiB -> {:.2f}MiB, blocks {:.2f}MiB -> {:.2f}MiB",
        stats.deviceIdleMs, stats.teardownMs, stats.gltfParseMs,
        stats.textureFileReadMs, stats.textureDecodeMs,
        stats.resources.textureResizeMs, stats.resources.textureUploadMs,
        stats.materialSetupMs, stats.meshCpuMs,
        stats.resources.meshUploadMs, stats.resources.batchSubmitWaitMs,
        stats.hierarchyMs,
        bytesToMiB(stats.resources.peakStagingBytes),
        bytesToMiB(stats.resources.encodedSourceBytes),
        bytesToMiB(stats.resources.decodedRgbaBytes),
        bytesToMiB(stats.resources.textureUploadBytes),
        bytesToMiB(stats.resources.textureGpuBytesEstimated),
        stats.allocatorBefore.allocationCount,
        stats.allocatorAfter.allocationCount,
        bytesToMiB(stats.allocatorBefore.allocationBytes),
        bytesToMiB(stats.allocatorAfter.allocationBytes),
        bytesToMiB(stats.allocatorBefore.blockBytes),
        bytesToMiB(stats.allocatorAfter.blockBytes));
}

} // namespace

Application::Application(const Config &config) : config_(config) {}

Application::~Application() {
    if (runtimeControlServer_)
        runtimeControlServer_->stop();
    if (device_)
        vkDeviceWaitIdle(device_->logicalDevice());
}

void Application::run() {
    init();
    runtimeCommandQueue_ = std::make_unique<RuntimeCommandQueue>();
    runtimeControlServer_ =
        std::make_unique<NamedPipeServerWin32>(*runtimeCommandQueue_);
    runtimeControlServer_->start();
    try {
        mainLoop();
    } catch (...) {
        runtimeControlServer_->stop();
        throw;
    }
    runtimeControlServer_->stop();
}

void Application::registerScene(SceneEntry entry) {
    sceneRegistry_.push_back(std::move(entry));
}

void Application::init() {
#ifndef NDEBUG
    runResourcePoolSelfTest();
#endif

    shaderVariants_.assign(kShaderVariants.begin(), kShaderVariants.end());

    window_ = std::make_unique<Window>(
        config_.windowWidth, config_.windowHeight, config_.windowTitle);
    input_ = std::make_unique<InputManager>(*window_);

    auto extensions = Window::getRequiredVulkanExtensions();
    context_ = std::make_unique<VulkanContext>(
        [this](VkInstance inst) { return window_->createSurface(inst); },
        std::move(extensions));
    device_ = std::make_unique<Device>(*context_);
    descriptorAllocator_ = std::make_unique<DescriptorAllocator>(*device_);
    swapChain_ =
        std::make_unique<SwapChain>(*device_, context_->surface(), [this]() {
            return window_->framebufferExtent();
        });
    frameSync_ = std::make_unique<FrameSync>(*device_, *swapChain_);
    renderer_ = std::make_unique<Renderer>(
        *device_, *swapChain_, *frameSync_, *descriptorAllocator_,
        sizeof(GlobalUBO));

    window_->setResizeCallback(
        [this](int, int) { frameSync_->notifyResize(); });

    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));

    if (sceneRegistry_.empty())
        throw std::runtime_error("No scenes registered; call "
                                 "Application::registerScene before run().");

    const int start = std::clamp(config_.defaultSceneIndex, 0,
                                 static_cast<int>(sceneRegistry_.size()) - 1);
    pipelineCache_ = std::make_unique<PipelineCache>(*device_);
    sceneLoadContext_.maxTextureSize = config_.gltfMaxTextureSize;
    loadScene(start);

    // ImGui on top of the main render pass.
    gui_ = std::make_unique<GuiSystem>(
        context_->instance(), *device_, renderer_->renderPass(),
        window_->handle(), swapChain_->imageCount(), swapChain_->imageCount());
}

void Application::loadScene(int index, bool replaceCurrent) {
    const auto &entry = sceneRegistry_[index];
    SceneLoadStats stats{};
    stats.sceneName = entry.name;
    stats.maxTextureSize = sceneLoadContext_.maxTextureSize;
    const auto totalStart = std::chrono::steady_clock::now();

    if (replaceCurrent) {
        {
            ScopedLoadTimer idleTimer(&stats.deviceIdleMs);
            ++stats.deviceWaitIdleCalls;
            vkDeviceWaitIdle(device_->logicalDevice());
        }
        {
            ScopedLoadTimer teardownTimer(&stats.teardownMs);
            pipelineCache_->clear();
            currentScene_.reset();
        }
    }

    stats.allocatorBefore = device_->allocatorMemorySnapshot();
    const UploadSyncCounters syncBefore = frameSync_->uploadSyncCounters();
    sceneLoadContext_.loadStats = &stats;

    try {
        std::unique_ptr<Scene> loadedScene;
        {
            ScopedLoadTimer factoryTimer(&stats.sceneFactoryMs);
            UploadContext upload(*device_, &stats.resources);
            loadedScene = entry.factory(*device_, upload,
                                        *descriptorAllocator_,
                                        sceneLoadContext_);
            upload.finish();
        }
        sceneLoadContext_.loadStats = nullptr;

        stats.materialCount = loadedScene ? loadedScene->materials().size() : 0;
        stats.objectCount = loadedScene ? loadedScene->objects().size() : 0;
        stats.allocatorAfter = device_->allocatorMemorySnapshot();
        const UploadSyncCounters syncAfter = frameSync_->uploadSyncCounters();
        stats.resources.singleTimeSubmits +=
            syncAfter.singleTimeSubmits - syncBefore.singleTimeSubmits;
        stats.resources.queueWaitIdleCalls +=
            syncAfter.queueWaitIdleCalls - syncBefore.queueWaitIdleCalls;

        currentScene_ = std::move(loadedScene);
        currentSceneIndex_ = index;
        if (currentScene_->initialCamera) {
            const auto &p = *currentScene_->initialCamera;
            camera_.setPosition(p.position);
            camera_.setYawPitch(p.yaw, p.pitch);
        }
        applySceneCameraDefaults();
        stats.success = true;
    } catch (...) {
        sceneLoadContext_.loadStats = nullptr;
        stats.allocatorAfter = device_->allocatorMemorySnapshot();
        const UploadSyncCounters syncAfter = frameSync_->uploadSyncCounters();
        stats.resources.singleTimeSubmits +=
            syncAfter.singleTimeSubmits - syncBefore.singleTimeSubmits;
        stats.resources.queueWaitIdleCalls +=
            syncAfter.queueWaitIdleCalls - syncBefore.queueWaitIdleCalls;
        stats.totalMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - totalStart)
                            .count();
        lastSceneLoadStats_ = stats;
        validateSceneLoadStats(stats);
        logSceneLoadStats(stats);
        throw;
    }

    stats.totalMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - totalStart)
                        .count();
    lastSceneLoadStats_ = stats;
    validateSceneLoadStats(stats);
    logSceneLoadStats(stats);
}

void Application::reloadCurrentScene() {
    if (currentSceneIndex_ < 0 ||
        currentSceneIndex_ >= static_cast<int>(sceneRegistry_.size()))
        return;

    const int index = currentSceneIndex_;
    loadScene(index, true);

    VKR_LOG_INFO("Scene", "Reloaded {} with glTF texture limit {}",
                 sceneRegistry_[index].name,
                 sceneLoadContext_.maxTextureSize == 0
                     ? std::string("Full")
                     : std::to_string(sceneLoadContext_.maxTextureSize));
}

void Application::switchScene(int index) {
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        return;
    if (index == currentSceneIndex_)
        return;

    loadScene(index, true);

    VKR_LOG_INFO("Scene", "Switched to {}", sceneRegistry_[index].name);
}

void Application::setTextureLimit(uint32_t limit) {
    if (limit != 0 && limit != 512 && limit != 1024 && limit != 2048)
        throw RuntimeCommandError("invalid_texture_limit",
                                  "Texture limit must be 0, 512, 1024, or "
                                  "2048.");
    if (sceneLoadContext_.maxTextureSize == limit)
        return;

    sceneLoadContext_.maxTextureSize = limit;
    VKR_LOG_INFO("Renderer", "glTF texture limit set to {}",
                 textureLimitLabel(limit));
    reloadCurrentScene();
}

void Application::setShaderVariant(int index) {
    if (index < 0 || index >= static_cast<int>(shaderVariants_.size()))
        throw RuntimeCommandError("invalid_shader", "Invalid shader index.");
    if (currentShaderVariantIndex_ == index)
        return;
    currentShaderVariantIndex_ = index;
    VKR_LOG_INFO("Renderer", "Shader variant switched to {}",
                 shaderVariants_[index].displayName);
}

int Application::findSceneIndexByName(const std::string &name) const {
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
        if (asciiEqualsIgnoreCase(sceneRegistry_[i].name, name))
            return i;
    }
    return -1;
}

int Application::findShaderVariantIndexByName(const std::string &name) const {
    for (int i = 0; i < static_cast<int>(shaderVariants_.size()); ++i) {
        if (asciiEqualsIgnoreCase(shaderVariants_[i].displayName, name))
            return i;
    }
    return -1;
}

void Application::processRuntimeCommand() {
    if (!runtimeCommandQueue_)
        return;
    std::shared_ptr<RuntimeCommand> command = runtimeCommandQueue_->popNext();
    if (!command)
        return;

    ControlJson result = ControlJson::object();
    bool requestQuit = false;
    try {
        if (command->method == "system.ping") {
            result = {{"message", "pong"}};
        } else if (command->method == "system.info") {
            result = {
                {"application", "VulkanLab"},
                {"protocolVersion", control::kProtocolVersion},
                {"pipe", control::kPipeNameUtf8},
                {"scene", currentSceneIndex_ >= 0
                              ? ControlJson(sceneRegistry_[currentSceneIndex_]
                                                .name)
                              : ControlJson(nullptr)},
                {"textureLimit", sceneLoadContext_.maxTextureSize},
                {"shader", shaderVariants_.empty()
                               ? ControlJson(nullptr)
                               : ControlJson(currentShaderVariant().displayName)}};
        } else if (command->method == "scene.list") {
            ControlJson scenes = ControlJson::array();
            for (const auto &entry : sceneRegistry_)
                scenes.push_back(entry.name);
            result = {{"scenes", std::move(scenes)}};
        } else if (command->method == "scene.current") {
            result = {{"name", currentSceneIndex_ >= 0
                                   ? ControlJson(sceneRegistry_[currentSceneIndex_]
                                                     .name)
                                   : ControlJson(nullptr)}};
        } else if (command->method == "scene.load") {
            const std::string name = requiredStringParam(*command, "name");
            const int index = findSceneIndexByName(name);
            if (index < 0) {
                ControlJson candidates = ControlJson::array();
                for (const auto &entry : sceneRegistry_)
                    candidates.push_back(entry.name);
                throw RuntimeCommandError(
                    "scene_not_found",
                    "Unknown scene '" + name + "'. Available scenes: " +
                        candidates.dump());
            }
            pendingSceneIndex_ = -1;
            switchScene(index);
            result = {{"scene", sceneRegistry_[index].name}};
            if (lastSceneLoadStats_)
                result["loadStats"] =
                    sceneLoadStatsToJson(*lastSceneLoadStats_);
        } else if (command->method == "scene.reload") {
            if (currentSceneIndex_ < 0)
                throw RuntimeCommandError("no_current_scene",
                                          "No scene is currently loaded.");
            reloadCurrentScene();
            result = {{"scene", sceneRegistry_[currentSceneIndex_].name}};
            if (lastSceneLoadStats_)
                result["loadStats"] =
                    sceneLoadStatsToJson(*lastSceneLoadStats_);
        } else if (command->method == "texture_limit.get") {
            result = {{"value", sceneLoadContext_.maxTextureSize}};
        } else if (command->method == "texture_limit.set") {
            if (!command->params.contains("value") ||
                !command->params["value"].is_number_unsigned()) {
                throw RuntimeCommandError(
                    "invalid_params",
                    "Parameter 'value' must be an unsigned integer.");
            }
            setTextureLimit(command->params["value"].get<uint32_t>());
            result = {{"value", sceneLoadContext_.maxTextureSize}};
            if (lastSceneLoadStats_)
                result["loadStats"] =
                    sceneLoadStatsToJson(*lastSceneLoadStats_);
        } else if (command->method == "shader.list") {
            ControlJson shaders = ControlJson::array();
            for (const auto &variant : shaderVariants_)
                shaders.push_back(variant.displayName);
            result = {{"shaders", std::move(shaders)}};
        } else if (command->method == "shader.current") {
            result = {{"name", shaderVariants_.empty()
                                   ? ControlJson(nullptr)
                                   : ControlJson(currentShaderVariant()
                                                     .displayName)}};
        } else if (command->method == "shader.set") {
            const std::string name = requiredStringParam(*command, "name");
            const int index = findShaderVariantIndexByName(name);
            if (index < 0) {
                ControlJson candidates = ControlJson::array();
                for (const auto &variant : shaderVariants_)
                    candidates.push_back(variant.displayName);
                throw RuntimeCommandError(
                    "shader_not_found",
                    "Unknown shader '" + name + "'. Available shaders: " +
                        candidates.dump());
            }
            setShaderVariant(index);
            result = {{"shader", shaderVariants_[index].displayName}};
        } else if (command->method == "stats.last_load") {
            if (!lastSceneLoadStats_)
                throw RuntimeCommandError("no_load_stats",
                                          "No scene load statistics exist.");
            result = sceneLoadStatsToJson(*lastSceneLoadStats_);
        } else if (command->method == "app.quit") {
            result = {{"quitting", true}};
            requestQuit = true;
        } else {
            throw RuntimeCommandError("method_not_found",
                                      "Unknown method '" + command->method +
                                          "'.");
        }

        command->response.set_value(
            makeRuntimeSuccess(command->id, std::move(result)));
        if (requestQuit)
            pendingQuitCommand_ = command;
    } catch (const RuntimeCommandError &e) {
        VKR_LOG_WARN("Control", "Command {} failed: {}", command->method,
                     e.what());
        command->response.set_value(
            makeRuntimeError(command->id, e.code(), e.what()));
    } catch (const std::exception &e) {
        VKR_LOG_ERROR("Control", "Command {} failed: {}", command->method,
                      e.what());
        command->response.set_value(
            makeRuntimeError(command->id, "command_failed", e.what()));
    }
}

void Application::applySceneCameraDefaults() {
    constexpr float kFallbackNear = 0.05f;
    constexpr float kFallbackFar = 200.0f;
    constexpr float kMinSceneFar = 50.0f;
    constexpr float kRadiusFarScale = 4.0f;
    constexpr float kMinAutoNear = 0.01f;
    constexpr float kMaxAutoNear = 0.05f;

    if (!currentScene_ || !currentScene_->bounds().valid) {
        camera_.setClipPlanes(kFallbackNear, kFallbackFar);
        return;
    }

    const Bounds &bounds = currentScene_->bounds();
    const float distanceToCenter =
        glm::length(camera_.position() - bounds.center);
    const float farPlane =
        std::max(kMinSceneFar,
                 distanceToCenter + bounds.radius * kRadiusFarScale);
    const float nearPlane =
        std::max(kMinAutoNear, std::min(kMaxAutoNear, farPlane / 10000.0f));
    camera_.setClipPlanes(nearPlane, farPlane);
}

void Application::updateInputMode() {
    auto &io = ImGui::GetIO();

    if (mode_ == InputMode::UI) {
        const bool pressed = input_->isMousePressed(MouseButton::Right);
        const bool overUI = io.WantCaptureMouse || ImGui::IsAnyItemActive();
        if (pressed && !overUI) {
            savedCursor_ = input_->cursorPos();
            input_->setCursorCaptured(true);
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
            mode_ = InputMode::CameraDrag;
        }
    } else { // CameraDrag
        if (input_->isMouseReleased(MouseButton::Right)) {
            input_->setCursorCaptured(false);
            input_->setCursorPos(savedCursor_);
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
            mode_ = InputMode::UI;
        }
    }
}

void Application::processCameraInput(float dt) {
    glm::vec3 move{0.0f};
    if (input_->isKeyDown(Key::W))
        move.z += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::S))
        move.z -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::A))
        move.x -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::D))
        move.x += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::Q))
        move.y -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::E))
        move.y += config_.moveSpeed * dt;
    camera_.translate(move);

    const auto d = input_->mouseDelta();
    camera_.rotate(-d.x * config_.mouseSensitivity,
                   -d.y * config_.mouseSensitivity);
}

void Application::updateUniforms(uint32_t frameIndex) {
    GlobalUBO ubo{};
    ubo.view = camera_.viewMatrix();
    ubo.proj = camera_.projectionMatrix();
    ubo.cameraPosWS = glm::vec4(camera_.position(), 1.0f);
    ubo.ambientColorIntensity =
        glm::vec4(glm::max(ambientColor_, glm::vec3(0.0f)),
                  std::max(ambientIntensity_, 0.0f));

    uint32_t directionalCount = 0;
    uint32_t punctualCount = 0;
    uint32_t ignoredCount = 0;

    const auto uploadLight = [&](const SceneLight &light) {
        switch (light.type) {
        case LightType::Directional:
            if (directionalCount < kMaxDirectionalLights) {
                ubo.directionalLights[directionalCount++] =
                    makeGpuLight(light);
            } else {
                ++ignoredCount;
            }
            break;
        case LightType::Point:
        case LightType::Spot:
            if (punctualCount < kMaxPunctualLights) {
                ubo.punctualLights[punctualCount++] = makeGpuLight(light);
            } else {
                ++ignoredCount;
            }
            break;
        }
    };

    const auto *sceneLights =
        currentScene_ ? &currentScene_->lights() : nullptr;
    if (sceneLights && !sceneLights->empty()) {
        for (const auto &light : *sceneLights)
            uploadLight(light);
    } else {
        uploadLight(makeDefaultSun(defaultSunDirection_, defaultSunColor_,
                                   defaultSunIntensity_));
    }

    ubo.lightCounts =
        glm::vec4(static_cast<float>(directionalCount),
                  static_cast<float>(punctualCount), 0.0f, 0.0f);
    lastUploadedDirectionalLights_ = directionalCount;
    lastUploadedPunctualLights_ = punctualCount;
    if (ignoredCount != lastIgnoredLights_) {
        if (ignoredCount > 0) {
            VKR_LOG_WARN("Lighting",
                         "Ignored {} scene lights beyond GPU light limits.",
                         ignoredCount);
        }
        lastIgnoredLights_ = ignoredCount;
    }
    std::memcpy(renderer_->mappedUniformBuffer(frameIndex), &ubo, sizeof(ubo));
}

void Application::drawGui() {
    ImGui::Begin("Renderer");
    if (!shaderVariants_.empty()) {
        const char *current =
            shaderVariants_[currentShaderVariantIndex_].displayName;
        if (ImGui::BeginCombo("Shader", current)) {
            for (int i = 0; i < static_cast<int>(shaderVariants_.size());
                 ++i) {
                const bool selected = (i == currentShaderVariantIndex_);
                if (ImGui::Selectable(shaderVariants_[i].displayName,
                                      selected))
                    setShaderVariant(i);
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    constexpr uint32_t textureLimits[] = {0, 2048, 1024, 512};
    if (ImGui::BeginCombo("Texture Limit",
                          textureLimitLabel(sceneLoadContext_.maxTextureSize))) {
        for (uint32_t limit : textureLimits) {
            const bool selected = sceneLoadContext_.maxTextureSize == limit;
            if (ImGui::Selectable(textureLimitLabel(limit), selected)) {
                if (!selected)
                    setTextureLimit(limit);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::End();

    ImGui::Begin("Scene");
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
        const bool selected = (i == currentSceneIndex_);
        if (ImGui::Selectable(sceneRegistry_[i].name.c_str(), selected))
            pendingSceneIndex_ = i;
    }
    ImGui::End();

    ImGui::Begin("Lighting");
    ImGui::ColorEdit3("Ambient Color", &ambientColor_.x);
    ImGui::DragFloat("Ambient Intensity", &ambientIntensity_, 0.01f, 0.0f,
                     10.0f);
    const size_t sceneLightCount = currentScene_ ? currentScene_->lights().size()
                                                 : 0;
    ImGui::Text("Scene lights: %zu", sceneLightCount);
    ImGui::Text("Uploaded: %u directional, %u punctual",
                lastUploadedDirectionalLights_, lastUploadedPunctualLights_);
    if (lastIgnoredLights_ > 0)
        ImGui::Text("Ignored: %u", lastIgnoredLights_);
    if (sceneLightCount == 0) {
        ImGui::Separator();
        ImGui::DragFloat3("Sun Direction", &defaultSunDirection_.x, 0.01f,
                          -1.0f, 1.0f);
        ImGui::ColorEdit3("Sun Color", &defaultSunColor_.x);
        ImGui::DragFloat("Sun Intensity", &defaultSunIntensity_, 0.05f, 0.0f,
                         20.0f);
    }
    ImGui::End();

    ImGui::Begin("Materials");
    if (!currentScene_) {
        ImGui::Text("Materials: 0");
    } else {
        const auto &materials = currentScene_->materials();
        ImGui::Text("Materials: %zu", materials.size());
        for (size_t i = 0; i < materials.size(); ++i) {
            const auto &material = materials[i];
            if (!material) {
                ImGui::Text("#%zu <null>", i);
                continue;
            }

            const auto &params = material->params();
            const std::string label =
                "#" + std::to_string(i) + " " +
                (params.debugName.empty() ? "<unnamed>" : params.debugName);
            if (!ImGui::TreeNode(label.c_str()))
                continue;

            ImGui::Text("Alpha Mode: %s", alphaModeName(params.alphaMode));
            ImGui::Text("Alpha Cutoff: %.3f", params.alphaCutoff);
            ImGui::Text("Double Sided: %s",
                        params.doubleSided ? "true" : "false");
            ImGui::Text("Transmission: %.3f", params.transmissionFactor);
            ImGui::Text("Emissive Strength: %.3f",
                        params.emissiveStrength);
            ImGui::Text("Metallic Factor: %.3f", params.metallicFactor);
            ImGui::Text("Roughness Factor: %.3f", params.roughnessFactor);
            ImGui::Text("Normal Scale: %.3f", params.normalScale);
            ImGui::Text("Occlusion Strength: %.3f",
                        params.occlusionStrength);
            ImGui::Text("Occlusion UV: %u", params.occlusionTexCoord);
            ImGui::Text("Base Color Factor: %.3f %.3f %.3f %.3f",
                        params.baseColorFactor.r, params.baseColorFactor.g,
                        params.baseColorFactor.b, params.baseColorFactor.a);
            ImGui::Text("Emissive Factor: %.3f %.3f %.3f",
                        params.emissiveFactor.r, params.emissiveFactor.g,
                        params.emissiveFactor.b);
            ImGui::Separator();
            ImGui::Text("Render Queue: %s",
                        isTransparentMaterial(params) ? "Transparent"
                                                      : "Opaque");
            ImGui::Text("Cull: %s", params.doubleSided ? "None" : "Back");
            ImGui::Separator();
            const auto &textures = material->textures();
            for (size_t slotIndex = 0; slotIndex < kMaterialTextureSlotCount;
                 ++slotIndex) {
                const auto slot =
                    static_cast<MaterialTextureSlot>(slotIndex);
                ImGui::Text("%s: %s", slotName(slot),
                            textures[slotIndex] ? "Bound" : "Missing");
            }
            ImGui::TreePop();
        }
    }
    ImGui::End();

    ImGui::Begin("Camera");
    const auto cameraPos = camera_.position();
    ImGui::Text("Position: (%.2f, %.2f, %.2f)", cameraPos.x, cameraPos.y,
                cameraPos.z);
    float nearPlane = camera_.nearPlane();
    float farPlane = camera_.farPlane();
    bool  clipChanged = false;
    clipChanged |= ImGui::DragFloat("Near Plane", &nearPlane, 0.001f, 0.001f,
                                    100.0f, "%.4f");
    clipChanged |= ImGui::DragFloat("Far Plane", &farPlane, 0.1f, 1.0f,
                                    100000.0f, "%.2f");
    if (clipChanged)
        camera_.setClipPlanes(nearPlane, farPlane);

    if (currentScene_ && currentScene_->bounds().valid) {
        const Bounds &bounds = currentScene_->bounds();
        ImGui::Separator();
        ImGui::Text("Bounds Center: (%.2f, %.2f, %.2f)", bounds.center.x,
                    bounds.center.y, bounds.center.z);
        ImGui::Text("Bounds Radius: %.2f", bounds.radius);
    } else {
        ImGui::Separator();
        ImGui::Text("Bounds: unavailable");
    }
    ImGui::End();

    ImGui::Begin("Stats");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    const auto p = camera_.position();
    ImGui::Text("Camera: (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
    ImGui::Text("Mode:   %s", mode_ == InputMode::UI ? "UI" : "CameraDrag");
    if (currentScene_)
        ImGui::Text("Objects: %zu", currentScene_->objects().size());
    if (lastSceneLoadStats_ &&
        ImGui::CollapsingHeader("Last Scene Load",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const SceneLoadStats &stats = *lastSceneLoadStats_;
        const ResourceLoadStats &resources = stats.resources;
        const int64_t allocationDelta =
            memoryDelta(stats.allocatorAfter.allocationBytes,
                        stats.allocatorBefore.allocationBytes);
        const int64_t blockDelta =
            memoryDelta(stats.allocatorAfter.blockBytes,
                        stats.allocatorBefore.blockBytes);

        ImGui::Text("Scene: %s", stats.sceneName.c_str());
        ImGui::Text("Status: %s", stats.success ? "Success" : "Failed");
        ImGui::Text("Texture Limit: %s",
                    textureLimitLabel(stats.maxTextureSize));
        ImGui::Separator();
        ImGui::Text("Total: %.2f ms", stats.totalMs);
        ImGui::Text("Device Idle: %.2f ms (%llu calls)",
                    stats.deviceIdleMs,
                    static_cast<unsigned long long>(
                        stats.deviceWaitIdleCalls));
        ImGui::Text("Teardown: %.2f ms", stats.teardownMs);
        ImGui::Text("Scene Factory: %.2f ms", stats.sceneFactoryMs);
        ImGui::Text("glTF Parse: %.2f ms", stats.gltfParseMs);
        ImGui::Text("Image Read: %.2f ms", stats.textureFileReadMs);
        ImGui::Text("Image Decode: %.2f ms", stats.textureDecodeMs);
        ImGui::Text("Texture Resize: %.2f ms", resources.textureResizeMs);
        ImGui::Text("Texture Upload: %.2f ms", resources.textureUploadMs);
        ImGui::Text("Material Setup: %.2f ms", stats.materialSetupMs);
        ImGui::Text("Mesh CPU: %.2f ms", stats.meshCpuMs);
        ImGui::Text("Mesh Upload: %.2f ms", resources.meshUploadMs);
        ImGui::Text("Batch Submit/Wait: %.2f ms",
                    resources.batchSubmitWaitMs);
        ImGui::Text("Hierarchy: %.2f ms", stats.hierarchyMs);
        ImGui::Separator();
        ImGui::Text("Textures: %llu decoded, %llu GPU, %llu resized",
                    static_cast<unsigned long long>(
                        resources.textureDecodeCount),
                    static_cast<unsigned long long>(resources.gpuTextureCount),
                    static_cast<unsigned long long>(
                        resources.resizedTextureCount));
        ImGui::Text("Meshes: %llu  Vertices: %llu  Indices: %llu",
                    static_cast<unsigned long long>(resources.gpuMeshCount),
                    static_cast<unsigned long long>(resources.vertexCount),
                    static_cast<unsigned long long>(resources.indexCount));
        ImGui::Text("Materials: %llu  Objects: %llu",
                    static_cast<unsigned long long>(stats.materialCount),
                    static_cast<unsigned long long>(stats.objectCount));
        ImGui::Text("Texture bytes: encoded %.2f, decoded %.2f MiB",
                    bytesToMiB(resources.encodedSourceBytes),
                    bytesToMiB(resources.decodedRgbaBytes));
        ImGui::Text("Texture upload: %.2f MiB  GPU estimate: %.2f MiB",
                    bytesToMiB(resources.textureUploadBytes),
                    bytesToMiB(resources.textureGpuBytesEstimated));
        ImGui::Text("Mesh upload: %.2f MiB",
                    bytesToMiB(resources.vertexUploadBytes +
                               resources.indexUploadBytes));
        ImGui::Text("Legacy submits: %llu  Queue waits: %llu",
                    static_cast<unsigned long long>(
                        resources.singleTimeSubmits),
                    static_cast<unsigned long long>(
                        resources.queueWaitIdleCalls));
        ImGui::Text("Batch submits: %llu  Fence waits: %llu",
                    static_cast<unsigned long long>(resources.batchSubmits),
                    static_cast<unsigned long long>(
                        resources.fenceWaitCalls));
        ImGui::Text("Peak staging: %.2f MiB",
                    bytesToMiB(resources.peakStagingBytes));
        ImGui::Separator();
        ImGui::Text("VMA allocations: %llu -> %llu",
                    static_cast<unsigned long long>(
                        stats.allocatorBefore.allocationCount),
                    static_cast<unsigned long long>(
                        stats.allocatorAfter.allocationCount));
        ImGui::Text("VMA allocation bytes: %.2f -> %.2f MiB (%+.2f)",
                    bytesToMiB(stats.allocatorBefore.allocationBytes),
                    bytesToMiB(stats.allocatorAfter.allocationBytes),
                    signedBytesToMiB(allocationDelta));
        ImGui::Text("VMA block bytes: %.2f -> %.2f MiB (%+.2f)",
                    bytesToMiB(stats.allocatorBefore.blockBytes),
                    bytesToMiB(stats.allocatorAfter.blockBytes),
                    signedBytesToMiB(blockDelta));
    }
    ImGui::Text("(Hold RMB in viewport to fly, WASD/Q/E to move)");
    ImGui::End();
}

void Application::handleSwapChainRecreate() {
    renderer_->recreateSwapChain();
    pipelineCache_->clear();
    frameSync_->onSwapChainRecreated();
    gui_->onSwapChainRecreated(swapChain_->imageCount());
    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));
}

const ShaderVariant &Application::currentShaderVariant() const {
    if (shaderVariants_.empty())
        return defaultShaderVariant();
    const int index = std::clamp(currentShaderVariantIndex_, 0,
                                 static_cast<int>(shaderVariants_.size()) - 1);
    return shaderVariants_[index];
}

void Application::mainLoop() {
    auto startTime = std::chrono::high_resolution_clock::now();
    auto lastTime = startTime;

    while (!window_->shouldClose()) {
        window_->pollEvents();
        input_->update();

        processRuntimeCommand();
        if (pendingQuitCommand_ &&
            pendingQuitCommand_->responseDelivered.load()) {
            window_->setShouldClose(true);
            pendingQuitCommand_.reset();
            break;
        }

        // 1. 帧外：场景切�?
        if (pendingSceneIndex_ != -1) {
            switchScene(pendingSceneIndex_);
            pendingSceneIndex_ = -1;
        }

        // 2. 时间
        auto  now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // 3. ImGui 新帧
        gui_->beginFrame();

        // 4. 模式切换 + 输入
        updateInputMode();
        if (mode_ == InputMode::CameraDrag)
            processCameraInput(dt);
        if (input_->isKeyDown(Key::Escape))
            window_->setShouldClose(true);

        // 5. 场景 tick
        float t = std::chrono::duration<float>(now - startTime).count();
        if (currentScene_)
            currentScene_->update(dt, t);

        // 6. UI
        drawGui();

        // 7. 渲染
        auto ctx = frameSync_->beginFrame();
        if (!ctx) {
            if (frameSync_->swapChainNeedsRecreation())
                handleSwapChainRecreate();
            ImGui::EndFrame();
            input_->endFrame();
            continue;
        }

        updateUniforms(ctx->frameIndex);
        renderQueue_.clear();
        if (currentScene_)
            currentScene_->collectRenderCommands(renderQueue_);
        renderQueue_.sortOpaque();
        renderQueue_.sortTransparent(camera_.position());

        renderer_->renderFrame(*ctx, renderQueue_, *pipelineCache_, *gui_,
                               currentShaderVariant());
        frameSync_->endFrame(*ctx);

        if (frameSync_->swapChainNeedsRecreation())
            handleSwapChainRecreate();

        // 8. 帧末：丢弃本帧鼠标增量
        input_->endFrame();
    }

    vkDeviceWaitIdle(device_->logicalDevice());
}

} // namespace vkr
