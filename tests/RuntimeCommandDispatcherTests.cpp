#include "control/RuntimeCommandDispatcher.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void requireDispatcher(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class FakeRuntimeHost final : public vkr::RuntimeControlHost {
  public:
    vkr::ControlJson reply(std::string action,
                           vkr::ControlJson arguments =
                               vkr::ControlJson::object()) {
        lastAction = action;
        return {{"action", std::move(action)},
                {"arguments", std::move(arguments)}};
    }

    vkr::ControlJson runtimeSystemInfo() override {
        if (failSystemInfo)
            throw std::runtime_error("host failure");
        return reply("system.info");
    }
    vkr::ControlJson runtimeSceneList() override {
        return reply("scene.list");
    }
    vkr::ControlJson runtimeSceneCurrent() override {
        return reply("scene.current");
    }
    vkr::ControlJson runtimeSceneLoad(const std::string &name) override {
        return reply("scene.load", {{"name", name}});
    }
    vkr::ControlJson runtimeSceneReload() override {
        return reply("scene.reload");
    }
    vkr::ControlJson
    runtimeLoadStatus(std::optional<uint64_t> taskId) override {
        return reply("load.status", {{"taskId", taskId
                                                   ? vkr::ControlJson(*taskId)
                                                   : vkr::ControlJson(nullptr)}});
    }
    vkr::ControlJson
    runtimeLoadCancel(std::optional<uint64_t> taskId) override {
        return reply("load.cancel", {{"taskId", taskId
                                                   ? vkr::ControlJson(*taskId)
                                                   : vkr::ControlJson(nullptr)}});
    }
    vkr::ControlJson runtimeTextureLimitGet() override {
        return reply("texture_limit.get");
    }
    vkr::ControlJson runtimeTextureLimitSet(uint32_t value) override {
        return reply("texture_limit.set", {{"value", value}});
    }
    vkr::ControlJson runtimeAssetCatalog() override {
        return reply("asset.catalog");
    }
    vkr::ControlJson runtimeAssetStatus(
        const std::optional<std::string> &name) override {
        return reply("asset.status", {{"name", name
                                                   ? vkr::ControlJson(*name)
                                                   : vkr::ControlJson(nullptr)}});
    }
    vkr::ControlJson runtimeAssetImport(const std::string &name, bool force,
                                        bool loadAfter) override {
        return reply("asset.import", {{"name", name},
                                      {"force", force},
                                      {"loadAfter", loadAfter}});
    }
    vkr::ControlJson
    runtimeAssetCancel(std::optional<uint64_t> taskId) override {
        return reply("asset.cancel", {{"taskId", taskId
                                                    ? vkr::ControlJson(*taskId)
                                                    : vkr::ControlJson(nullptr)}});
    }
    vkr::ControlJson
    runtimeAssetValidation(const std::string &name) override {
        return reply("asset.validation", {{"name", name}});
    }
    vkr::ControlJson runtimeAssetCacheInfo() override {
        return reply("asset.cache_info");
    }
    vkr::ControlJson runtimeShaderList() override {
        return reply("shader.list");
    }
    vkr::ControlJson runtimeShaderCurrent() override {
        return reply("shader.current");
    }
    vkr::ControlJson runtimeShaderSet(const std::string &name) override {
        if (name == "host-error")
            throw vkr::RuntimeCommandError("shader_not_found",
                                           "host rejected shader");
        return reply("shader.set", {{"name", name}});
    }
    vkr::ControlJson runtimeCameraGet() override {
        return reply("camera.get");
    }
    vkr::ControlJson
    runtimeCameraSet(const vkr::RuntimeCameraPose &pose) override {
        return reply("camera.set",
                     {{"position", pose.position},
                      {"yaw", pose.yaw},
                      {"pitch", pose.pitch}});
    }
    vkr::ControlJson runtimeWindowResize(uint32_t width,
                                         uint32_t height) override {
        if (!automationMode) {
            throw vkr::RuntimeCommandError(
                "automation_required",
                "window.resize is available only in automation mode.");
        }
        return reply("window.resize",
                     {{"width", width}, {"height", height}});
    }
    vkr::ControlJson runtimeRenderStatus() override {
        return reply("render.status");
    }
    vkr::ControlJson runtimeRenderPathGet() override {
        return reply("render_path.get");
    }
    vkr::ControlJson
    runtimeRenderPathSet(vkr::RenderPathRequest request) override {
        return reply("render_path.set",
                     {{"renderPath", vkr::renderPathRequestName(request)}});
    }
    vkr::ControlJson runtimeRenderSettingsGet() override {
        return reply("render_settings.get");
    }
    vkr::ControlJson runtimeRenderSettingsSet(
        const vkr::RenderSettingsPatch &patch) override {
        vkr::ControlJson arguments = vkr::ControlJson::object();
        if (patch.shadowsEnabled)
            arguments["shadowsEnabled"] = *patch.shadowsEnabled;
        if (patch.shadowReceiverBias)
            arguments["shadowReceiverBias"] = *patch.shadowReceiverBias;
        if (patch.shadowConstantBias)
            arguments["shadowConstantBias"] = *patch.shadowConstantBias;
        if (patch.shadowSlopeBias)
            arguments["shadowSlopeBias"] = *patch.shadowSlopeBias;
        if (patch.exposureEv)
            arguments["exposureEv"] = *patch.exposureEv;
        if (patch.toneMapper)
            arguments["toneMapper"] = vkr::toneMapperName(*patch.toneMapper);
        if (patch.iblEnabled)
            arguments["iblEnabled"] = *patch.iblEnabled;
        if (patch.skyboxEnabled)
            arguments["skyboxEnabled"] = *patch.skyboxEnabled;
        if (patch.environmentIntensity)
            arguments["environmentIntensity"] =
                *patch.environmentIntensity;
        if (patch.environmentRotationRadians)
            arguments["environmentRotationRadians"] =
                *patch.environmentRotationRadians;
        if (patch.bloomEnabled)
            arguments["bloomEnabled"] = *patch.bloomEnabled;
        if (patch.bloomThreshold)
            arguments["bloomThreshold"] = *patch.bloomThreshold;
        if (patch.bloomSoftKnee)
            arguments["bloomSoftKnee"] = *patch.bloomSoftKnee;
        if (patch.bloomIntensity)
            arguments["bloomIntensity"] = *patch.bloomIntensity;
        if (patch.gBufferDebugView) {
            arguments["gBufferDebugView"] =
                vkr::gBufferDebugViewName(*patch.gBufferDebugView);
        }
        if (patch.deferredLightingDebugView) {
            arguments["deferredLightingDebugView"] =
                vkr::deferredLightingDebugViewName(
                    *patch.deferredLightingDebugView);
        }
        return reply("render_settings.set", std::move(arguments));
    }
    vkr::ControlJson runtimeEnvironmentList() override {
        return reply("environment.list");
    }
    vkr::ControlJson runtimeEnvironmentCurrent() override {
        return reply("environment.current");
    }
    vkr::ControlJson
    runtimeEnvironmentSet(const std::string &name) override {
        return reply("environment.set", {{"name", name}});
    }
    vkr::ControlJson runtimeEnvironmentReload() override {
        return reply("environment.reload");
    }
    vkr::ControlJson runtimeCaptureScreenshot(const std::string &path,
                                              bool includeGui) override {
        return reply("capture.screenshot",
                     {{"path", path}, {"includeGui", includeGui}});
    }
    vkr::ControlJson runtimeCaptureStatus(uint64_t taskId) override {
        return reply("capture.status", {{"taskId", taskId}});
    }
    vkr::ControlJson runtimeCaptureCancel(uint64_t taskId) override {
        return reply("capture.cancel", {{"taskId", taskId}});
    }
    vkr::ControlJson runtimeLastLoadStats() override {
        return reply("stats.last_load");
    }
    vkr::ControlJson runtimeQuit() override { return reply("app.quit"); }

    std::string lastAction;
    bool failSystemInfo = false;
    bool automationMode = true;
};

struct DispatchCase {
    std::string method;
    vkr::ControlJson params;
    vkr::ControlJson expectedResult;
    bool requestQuit = false;
};

void testAllProtocolMethods() {
    const auto result = [](const char *action,
                           vkr::ControlJson arguments =
                               vkr::ControlJson::object()) {
        return vkr::ControlJson{{"action", action},
                                {"arguments", std::move(arguments)}};
    };
    const std::vector<DispatchCase> cases = {
        {"system.ping", {}, {{"message", "pong"}}},
        {"system.info", {}, result("system.info")},
        {"scene.list", {}, result("scene.list")},
        {"scene.current", {}, result("scene.current")},
        {"scene.load", {{"name", "Scene"}},
         result("scene.load", {{"name", "Scene"}})},
        {"scene.reload", {}, result("scene.reload")},
        {"load.status", {},
         result("load.status", {{"taskId", nullptr}})},
        {"load.status", {{"taskId", uint64_t{42}}},
         result("load.status", {{"taskId", uint64_t{42}}})},
        {"load.cancel", {},
         result("load.cancel", {{"taskId", nullptr}})},
        {"texture_limit.get", {}, result("texture_limit.get")},
        {"texture_limit.set", {{"value", uint64_t{1024}}},
         result("texture_limit.set", {{"value", 1024}})},
        {"asset.catalog", {}, result("asset.catalog")},
        {"asset.status", {},
         result("asset.status", {{"name", nullptr}})},
        {"asset.status", {{"name", "scene-id"}},
         result("asset.status", {{"name", "scene-id"}})},
        {"asset.import",
         {{"name", "scene-id"}, {"force", true}, {"loadAfter", true}},
         result("asset.import", {{"name", "scene-id"},
                                 {"force", true},
                                 {"loadAfter", true}})},
        {"asset.cancel", {{"taskId", uint64_t{99}}},
         result("asset.cancel", {{"taskId", uint64_t{99}}})},
        {"asset.cache_info", {}, result("asset.cache_info")},
        {"shader.list", {}, result("shader.list")},
        {"shader.current", {}, result("shader.current")},
        {"shader.set", {{"name", "PBR"}},
         result("shader.set", {{"name", "PBR"}})},
        {"environment.list", {}, result("environment.list")},
        {"environment.current", {}, result("environment.current")},
        {"environment.set", {{"name", "studio"}},
         result("environment.set", {{"name", "studio"}})},
        {"environment.reload", {}, result("environment.reload")},
        {"camera.get", {}, result("camera.get")},
        {"camera.set",
         {{"position", {1.0, 2.0, 3.0}},
          {"yaw", -135.0},
          {"pitch", -30.0}},
         result("camera.set",
                {{"position", {1.0f, 2.0f, 3.0f}},
                 {"yaw", -135.0f},
                 {"pitch", -30.0f}})},
        {"window.resize", {{"width", uint64_t{1024}},
                            {"height", uint64_t{720}}},
         result("window.resize", {{"width", 1024}, {"height", 720}})},
        {"render.status", {}, result("render.status")},
        {"render_settings.get", {}, result("render_settings.get")},
        {"render_settings.set",
         {{"shadowsEnabled", false},
          {"shadowReceiverBias", 0.002},
          {"shadowConstantBias", 1.5},
          {"shadowSlopeBias", 2.0},
          {"exposureEv", -1.0},
          {"toneMapper", "reinhard"},
          {"iblEnabled", true},
          {"skyboxEnabled", true},
          {"environmentIntensity", 1.5},
          {"environmentRotationRadians", 0.5},
          {"bloomEnabled", true},
          {"bloomThreshold", 1.25},
          {"bloomSoftKnee", 0.4},
          {"bloomIntensity", 0.2},
          {"gBufferDebugView", "normal"},
          {"deferredLightingDebugView", "none"}},
         result("render_settings.set",
                {{"shadowsEnabled", false},
                 {"shadowReceiverBias", 0.002f},
                 {"shadowConstantBias", 1.5f},
                 {"shadowSlopeBias", 2.0f},
                 {"exposureEv", -1.0f},
                 {"toneMapper", "reinhard"},
                 {"iblEnabled", true},
                 {"skyboxEnabled", true},
                 {"environmentIntensity", 1.5f},
                 {"environmentRotationRadians", 0.5f},
                 {"bloomEnabled", true},
                 {"bloomThreshold", 1.25f},
                 {"bloomSoftKnee", 0.4f},
                 {"bloomIntensity", 0.2f},
                 {"gBufferDebugView", "normal"},
                 {"deferredLightingDebugView", "none"}})},
        {"capture.screenshot", {{"path", "suite/frame.png"}},
         result("capture.screenshot",
                {{"path", "suite/frame.png"}, {"includeGui", false}})},
        {"capture.screenshot",
         {{"path", "suite/gui.png"}, {"includeGui", true}},
         result("capture.screenshot",
                {{"path", "suite/gui.png"}, {"includeGui", true}})},
        {"capture.status", {{"taskId", uint64_t{100}}},
         result("capture.status", {{"taskId", uint64_t{100}}})},
        {"capture.cancel", {{"taskId", uint64_t{101}}},
         result("capture.cancel", {{"taskId", uint64_t{101}}})},
        {"stats.last_load", {}, result("stats.last_load")},
        {"app.quit", {}, result("app.quit"), true},
    };

    vkr::RuntimeCommandDispatcher dispatcher;
    FakeRuntimeHost host;
    uint64_t id = 10;
    for (const DispatchCase &item : cases) {
        vkr::RuntimeCommand command;
        command.id = id++;
        command.method = item.method;
        command.params = item.params;
        const vkr::RuntimeDispatchResult dispatched =
            dispatcher.dispatch(command, host);
        requireDispatcher(
            dispatched.response ==
                vkr::makeRuntimeSuccess(command.id, item.expectedResult),
            "runtime command success snapshot changed");
        requireDispatcher(dispatched.requestQuit == item.requestQuit,
                          "runtime quit routing changed");
    }
}

void requireError(const std::string &method, vkr::ControlJson params,
                  const std::string &code, const std::string &message) {
    vkr::RuntimeCommand command;
    command.id = 77;
    command.method = method;
    command.params = std::move(params);
    FakeRuntimeHost host;
    const vkr::RuntimeDispatchResult dispatched =
        vkr::RuntimeCommandDispatcher{}.dispatch(command, host);
    requireDispatcher(
        dispatched.response ==
            vkr::makeRuntimeError(command.id, code, message),
        "runtime command error snapshot changed");
    requireDispatcher(!dispatched.requestQuit,
                      "failed command requested application quit");
}

void testValidationAndErrorMapping() {
    requireError("scene.load", {}, "invalid_params",
                 "Parameter 'name' must be a string.");
    requireError("load.status", {{"taskId", -1}}, "invalid_params",
                 "Parameter 'taskId' must be an unsigned integer.");
    requireError("asset.import",
                 {{"name", "scene"}, {"force", "yes"}},
                 "invalid_params", "Parameter 'force' must be a boolean.");
    requireError("texture_limit.set", {{"value", 4294967296ULL}},
                 "invalid_params",
                 "Parameter 'value' must be an unsigned 32-bit integer.");
    requireError("missing.method", {}, "method_not_found",
                 "Unknown method 'missing.method'.");
    requireError("shader.set", {{"name", "host-error"}},
                 "shader_not_found", "host rejected shader");
    requireError("environment.set", {}, "invalid_params",
                 "Parameter 'name' must be a string.");
    requireError(
        "camera.set",
        {{"position", {1.0, 2.0}}, {"yaw", 0.0}, {"pitch", 0.0}},
        "invalid_params",
        "Parameter 'position' must be an array of three finite numbers.");
    requireError(
        "camera.set",
        {{"position", {1.0, "bad", 3.0}},
         {"yaw", 0.0},
         {"pitch", 0.0}},
        "invalid_params",
        "Parameter 'position' must be an array of three finite numbers.");
    requireError(
        "camera.set",
        {{"position", {1.0, 2.0, 3.0}},
         {"yaw", std::numeric_limits<double>::infinity()},
         {"pitch", 0.0}},
        "invalid_params", "Parameter 'yaw' must be a finite number.");
    requireError(
        "camera.set",
        {{"position", {1.0, 2.0, 3.0}},
         {"yaw", 0.0},
         {"pitch", "bad"}},
        "invalid_params", "Parameter 'pitch' must be a finite number.");
    requireError("capture.screenshot", {}, "invalid_params",
                 "Parameter 'path' must be a string.");
    requireError("capture.screenshot",
                 {{"path", "frame.png"}, {"includeGui", "no"}},
                 "invalid_params",
                 "Parameter 'includeGui' must be a boolean.");
    requireError("render_settings.set", {}, "invalid_params",
                 "render_settings.set requires at least one setting.");
    requireError("render_settings.set", {{"toneMapper", "bad"}},
                 "invalid_params",
                 "Parameter 'toneMapper' must be passthrough, reinhard, or aces.");
    requireError("render_settings.set", {{"exposureEv", 11.0}},
                 "invalid_params",
                 "Parameter 'exposureEv' is outside the supported range.");
    requireError(
        "render_settings.set", {{"environmentIntensity", -0.1}},
        "invalid_params",
        "Parameter 'environmentIntensity' is outside the supported range.");
    requireError("capture.status", {}, "invalid_params",
                 "Parameter 'taskId' must be an unsigned integer.");
    requireError("capture.cancel", {{"taskId", -1}}, "invalid_params",
                 "Parameter 'taskId' must be an unsigned integer.");
    requireError("window.resize",
                 {{"width", uint64_t{0}}, {"height", uint64_t{720}}},
                 "invalid_params",
                 "Parameter 'width' must be in 1..16384.");
    requireError("window.resize",
                 {{"width", uint64_t{1024}},
                  {"height", uint64_t{16385}}},
                 "invalid_params",
                 "Parameter 'height' must be in 1..16384.");

    vkr::RuntimeCommand resizeCommand;
    resizeCommand.id = 89;
    resizeCommand.method = "window.resize";
    resizeCommand.params = {{"width", uint64_t{1024}},
                            {"height", uint64_t{720}}};
    FakeRuntimeHost interactiveHost;
    interactiveHost.automationMode = false;
    const auto resizeResult =
        vkr::RuntimeCommandDispatcher{}.dispatch(resizeCommand,
                                                 interactiveHost);
    requireDispatcher(
        resizeResult.response == vkr::makeRuntimeError(
                                     resizeCommand.id,
                                     "automation_required",
                                     "window.resize is available only in "
                                     "automation mode."),
        "non-automation resize was not rejected");

    vkr::RuntimeCommand command;
    command.id = 88;
    command.method = "system.info";
    FakeRuntimeHost host;
    host.failSystemInfo = true;
    const auto dispatched =
        vkr::RuntimeCommandDispatcher{}.dispatch(command, host);
    requireDispatcher(
        dispatched.response == vkr::makeRuntimeError(
                                   command.id, "command_failed",
                                   "host failure"),
        "unexpected host exception mapping changed");
}

} // namespace

void runRuntimeCommandDispatcherTests() {
    testAllProtocolMethods();
    testValidationAndErrorMapping();
}
