#include "control/RuntimeControlClientWin32.h"

#include <json.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

struct ParsedCommand {
    std::string method;
    Json params = Json::object();
    vkr::control::RuntimeControlEndpoint endpoint =
        vkr::control::makeRuntimeControlEndpoint();
    bool jsonOutput = false;
    bool waitForLoad = true;
    bool force = false;
    bool loadAfter = false;
    bool includeGui = false;
    bool renderWait = false;
    uint32_t stableFrames = 8;
    uint32_t timeoutMs = 30000;
};

void printUsage() {
    std::cerr
        << "Usage:\n"
        << "  VulkanLabCtl [--pipe <suffix>] [--json] ping|info|quit\n"
        << "  VulkanLabCtl [--json] scene list|current|reload\n"
        << "  VulkanLabCtl [--json] [--no-wait] scene load <name>\n"
        << "  VulkanLabCtl [--json] load status [task-id]\n"
        << "  VulkanLabCtl [--json] load cancel [task-id]\n"
        << "  VulkanLabCtl [--json] asset catalog\n"
        << "  VulkanLabCtl [--json] asset status [scene]\n"
        << "  VulkanLabCtl [--json] asset validation <scene-id>\n"
        << "  VulkanLabCtl [--json] [--no-wait] [--force] "
           "[--load-after] asset import <scene>\n"
        << "  VulkanLabCtl [--json] asset cancel [task-id]\n"
        << "  VulkanLabCtl [--json] asset cache-info\n"
        << "  VulkanLabCtl [--json] texture-limit get\n"
        << "  VulkanLabCtl [--json] texture-limit set <full|512|1024|2048>\n"
        << "  VulkanLabCtl [--json] shader list|current\n"
        << "  VulkanLabCtl [--json] shader set <name>\n"
        << "  VulkanLabCtl [--json] environment list|current|reload\n"
        << "  VulkanLabCtl [--json] [--no-wait] environment set <name|None>\n"
        << "  VulkanLabCtl [--json] camera get\n"
        << "  VulkanLabCtl [--json] camera set --position X,Y,Z --yaw Y --pitch P\n"
        << "  VulkanLabCtl [--json] window resize <width> <height>\n"
        << "  VulkanLabCtl [--json] render status\n"
        << "  VulkanLabCtl [--json] render-settings get\n"
        << "  VulkanLabCtl [--json] render-settings set "
           "[--shadows on|off] [--receiver-bias N] "
           "[--constant-bias N] [--slope-bias N] "
           "[--max-point-shadows N] [--point-shadow-distance N] "
           "[--max-spot-shadows N] [--spot-shadow-distance N] "
           "[--exposure N] [--tone-mapper passthrough|reinhard|aces] "
           "[--ibl on|off] [--skybox on|off] "
           "[--environment-intensity N] [--environment-rotation-deg N] "
           "[--bloom on|off] [--bloom-threshold N] "
           "[--bloom-soft-knee N] [--bloom-intensity N] "
           "[--frustum on|off] [--shadow-culling on|off] "
           "[--shadow-distance N] [--distance-culling on|off] "
           "[--max-draw-distance N] [--small-object-culling on|off] "
           "[--min-projected-pixels N] [--occlusion on|off] "
           "[--occlusion-bias N] "
           "[--surface-debug none|normal|roughness|motion|history-validity] "
           "[--surface-motion-scale N] [--ao off|ssao|cacao|gtao] "
           "[--ssao-quality low|medium|high] [--ssao-radius N] "
           "[--ssao-bias N] [--ssao-intensity N] [--ssao-power N] "
           "[--cacao-quality lowest|low|medium|high|highest] "
           "[--cacao-resolution native|half] [--cacao-radius N] "
           "[--cacao-intensity N] [--cacao-power N] "
           "[--gtao-quality low|medium|high] [--gtao-radius N] "
           "[--gtao-falloff N] [--gtao-intensity N] [--gtao-power N] "
           "[--gtao-temporal-weight N] "
           "[--taa off|taa] [--taa-history-weight N] [--taa-sharpness N] "
           "[--reflection ibl-only|ssr] [--ssr-quality low|medium|high] "
           "[--ssr-max-distance N] [--ssr-thickness N] "
           "[--ssr-max-roughness N] [--ssr-intensity N] "
           "[--ssr-history-weight N] "
           "[--gi ambient-or-ibl|ssgi|ddgi|ssgi-ddgi] "
           "[--ssgi-quality low|medium|high] "
           "[--ssgi-max-distance N] [--ssgi-thickness N] "
           "[--ssgi-intensity N] [--ssgi-radiance-clamp N] "
           "[--ssgi-history-weight N] "
           "[--ddgi-radiance-clamp N] "
           "[--ddgi-debug none|irradiance|distance|classification] "
           "[--screen-space-debug none|nearest-depth|scene-color|ssao-raw|ssao-filtered|cacao-output|gtao-raw|gtao-temporal|gtao-filtered|gtao-rejection|gtao-history-weight|taa-history|taa-rejection|taa-history-weight|ssr-raw|ssr-temporal|ssr-filtered|ssr-confidence|ssr-rejection|ssgi-raw|ssgi-temporal|ssgi-filtered|ssgi-confidence|ssgi-variance|ssgi-rejection] "
           "[--screen-space-debug-mip N]\n"
        << "  VulkanLabCtl [--json] render wait [--stable-frames N] "
           "[--timeout-ms N]\n"
        << "  VulkanLabCtl [--json] capture screenshot <relative.png> "
           "[--no-gui|--include-gui]\n"
        << "  VulkanLabCtl [--json] capture status <task-id>\n"
        << "  VulkanLabCtl [--json] capture cancel <task-id>\n"
        << "  VulkanLabCtl [--json] stats\n";
}

float parseFiniteFloat(const std::string &value, const char *name) {
    size_t consumed = 0;
    const float parsed = std::stof(value, &consumed);
    if (consumed != value.size() || !std::isfinite(parsed))
        throw std::invalid_argument(std::string(name) +
                                    " must be a finite number");
    return parsed;
}

Json parsePosition(const std::string &value) {
    Json result = Json::array();
    size_t start = 0;
    for (size_t component = 0; component < 3; ++component) {
        const size_t separator = value.find(',', start);
        if ((component < 2 && separator == std::string::npos) ||
            (component == 2 && separator != std::string::npos)) {
            throw std::invalid_argument(
                "--position must use the form X,Y,Z");
        }
        const size_t end = separator == std::string::npos
                               ? value.size()
                               : separator;
        if (end == start)
            throw std::invalid_argument(
                "--position must use the form X,Y,Z");
        result.push_back(parseFiniteFloat(
            value.substr(start, end - start), "--position component"));
        start = end + 1;
    }
    return result;
}

uint32_t parsePositiveUint32(const std::string &value, const char *name) {
    size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0 || parsed > UINT32_MAX)
        throw std::invalid_argument(std::string(name) +
                                    " must be in 1..4294967295");
    return static_cast<uint32_t>(parsed);
}

uint32_t parseUint32(const std::string &value, const char *name) {
    size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed > UINT32_MAX)
        throw std::invalid_argument(std::string(name) +
                                    " must be an unsigned 32-bit integer");
    return static_cast<uint32_t>(parsed);
}

uint32_t parseTextureLimit(const std::string &value) {
    if (value == "full" || value == "Full" || value == "FULL")
        return 0;
    size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed);
    if (consumed != value.size() ||
        (parsed != 512 && parsed != 1024 && parsed != 2048)) {
        throw std::invalid_argument(
            "texture limit must be full, 512, 1024, or 2048");
    }
    return static_cast<uint32_t>(parsed);
}

bool parseOnOff(const std::string &value, const char *name) {
    if (value == "on")
        return true;
    if (value == "off")
        return false;
    throw std::invalid_argument(std::string(name) + " must be on or off");
}

ParsedCommand parseCommand(int argc, char **argv) {
    std::vector<std::string> args;
    bool jsonOutput = false;
    bool waitForLoad = true;
    bool force = false;
    bool loadAfter = false;
    bool includeGui = false;
    std::string pipeSuffix;
    std::optional<std::string> position;
    std::optional<std::string> yaw;
    std::optional<std::string> pitch;
    std::optional<std::string> stableFrames;
    std::optional<std::string> timeoutMs;
    std::optional<std::string> shadows;
    std::optional<std::string> receiverBias;
    std::optional<std::string> pointReceiverBias;
    std::optional<std::string> constantBias;
    std::optional<std::string> slopeBias;
    std::optional<std::string> maxPointShadows;
    std::optional<std::string> pointShadowDistance;
    std::optional<std::string> maxSpotShadows;
    std::optional<std::string> spotShadowDistance;
    std::optional<std::string> exposure;
    std::optional<std::string> toneMapper;
    std::optional<std::string> ibl;
    std::optional<std::string> skybox;
    std::optional<std::string> environmentIntensity;
    std::optional<std::string> environmentRotationDegrees;
    std::optional<std::string> bloom;
    std::optional<std::string> bloomThreshold;
    std::optional<std::string> bloomSoftKnee;
    std::optional<std::string> bloomIntensity;
    std::optional<std::string> frustumCulling;
    std::optional<std::string> shadowCulling;
    std::optional<std::string> shadowDistance;
    std::optional<std::string> distanceCulling;
    std::optional<std::string> maxDrawDistance;
    std::optional<std::string> smallObjectCulling;
    std::optional<std::string> minProjectedPixels;
    std::optional<std::string> occlusionCulling;
    std::optional<std::string> occlusionBias;
    std::optional<std::string> surfaceDebug;
    std::optional<std::string> surfaceMotionScale;
    std::optional<std::string> ambientOcclusion;
    std::optional<std::string> ssaoQuality;
    std::optional<std::string> ssaoRadius;
    std::optional<std::string> ssaoBias;
    std::optional<std::string> ssaoIntensity;
    std::optional<std::string> ssaoPower;
    std::optional<std::string> cacaoQuality;
    std::optional<std::string> cacaoResolution;
    std::optional<std::string> cacaoRadius;
    std::optional<std::string> cacaoIntensity;
    std::optional<std::string> cacaoPower;
    std::optional<std::string> gtaoQuality;
    std::optional<std::string> gtaoRadius;
    std::optional<std::string> gtaoFalloff;
    std::optional<std::string> gtaoIntensity;
    std::optional<std::string> gtaoPower;
    std::optional<std::string> gtaoTemporalWeight;
    std::optional<std::string> temporalAntiAliasing;
    std::optional<std::string> taaHistoryWeight;
    std::optional<std::string> taaSharpness;
    std::optional<std::string> reflectionMode;
    std::optional<std::string> ssrQuality;
    std::optional<std::string> ssrMaxDistance;
    std::optional<std::string> ssrThickness;
    std::optional<std::string> ssrMaxRoughness;
    std::optional<std::string> ssrIntensity;
    std::optional<std::string> ssrHistoryWeight;
    std::optional<std::string> globalIlluminationMode;
    std::optional<std::string> ssgiQuality;
    std::optional<std::string> ssgiMaxDistance;
    std::optional<std::string> ssgiThickness;
    std::optional<std::string> ssgiIntensity;
    std::optional<std::string> ssgiRadianceClamp;
    std::optional<std::string> ssgiHistoryWeight;
    std::optional<std::string> ddgiRadianceClamp;
    std::optional<std::string> ddgiDebug;
    std::optional<std::string> screenSpaceDebug;
    std::optional<std::string> screenSpaceDebugMip;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--pipe") {
            if (++i >= argc)
                throw std::invalid_argument("--pipe requires a suffix");
            pipeSuffix = argv[i];
            if (pipeSuffix.empty())
                throw std::invalid_argument(
                    "--pipe requires a non-empty suffix");
        } else if (argument == "--json")
            jsonOutput = true;
        else if (argument == "--position" || argument == "--yaw" ||
                 argument == "--pitch" ||
                 argument == "--stable-frames" ||
                 argument == "--timeout-ms" ||
                 argument == "--shadows" ||
                 argument == "--receiver-bias" ||
                 argument == "--point-receiver-bias" ||
                 argument == "--constant-bias" ||
                 argument == "--slope-bias" ||
                 argument == "--max-point-shadows" ||
                 argument == "--point-shadow-distance" ||
                 argument == "--max-spot-shadows" ||
                 argument == "--spot-shadow-distance" ||
                 argument == "--exposure" ||
                 argument == "--tone-mapper" ||
                 argument == "--ibl" ||
                 argument == "--skybox" ||
                 argument == "--environment-intensity" ||
                 argument == "--environment-rotation-deg" ||
                 argument == "--bloom" ||
                 argument == "--bloom-threshold" ||
                 argument == "--bloom-soft-knee" ||
                 argument == "--bloom-intensity" ||
                 argument == "--frustum" ||
                 argument == "--shadow-culling" ||
                 argument == "--shadow-distance" ||
                 argument == "--distance-culling" ||
                 argument == "--max-draw-distance" ||
                 argument == "--small-object-culling" ||
                 argument == "--min-projected-pixels" ||
                 argument == "--occlusion" ||
                 argument == "--occlusion-bias" ||
                  argument == "--surface-debug" ||
                  argument == "--surface-motion-scale" ||
                  argument == "--ao" ||
                  argument == "--ssao-quality" ||
                  argument == "--ssao-radius" ||
                  argument == "--ssao-bias" ||
                   argument == "--ssao-intensity" ||
                   argument == "--ssao-power" ||
                   argument == "--cacao-quality" ||
                   argument == "--cacao-resolution" ||
                   argument == "--cacao-radius" ||
                   argument == "--cacao-intensity" ||
                   argument == "--cacao-power" ||
                   argument == "--gtao-quality" ||
                   argument == "--gtao-radius" ||
                   argument == "--gtao-falloff" ||
                   argument == "--gtao-intensity" ||
                   argument == "--gtao-power" ||
                   argument == "--gtao-temporal-weight" ||
                  argument == "--taa" ||
                  argument == "--taa-history-weight" ||
                  argument == "--taa-sharpness" ||
                  argument == "--reflection" ||
                  argument == "--ssr-quality" ||
                  argument == "--ssr-max-distance" ||
                  argument == "--ssr-thickness" ||
                  argument == "--ssr-max-roughness" ||
                  argument == "--ssr-intensity" ||
                  argument == "--ssr-history-weight" ||
                  argument == "--gi" ||
                  argument == "--ssgi-quality" ||
                  argument == "--ssgi-max-distance" ||
                  argument == "--ssgi-thickness" ||
                  argument == "--ssgi-intensity" ||
                  argument == "--ssgi-radiance-clamp" ||
                  argument == "--ssgi-history-weight" ||
                  argument == "--ddgi-radiance-clamp" ||
                  argument == "--ddgi-debug" ||
                  argument == "--screen-space-debug" ||
                  argument == "--screen-space-debug-mip") {
            if (++i >= argc)
                throw std::invalid_argument(argument + " requires a value");
            if (argument == "--position")
                position = argv[i];
            else if (argument == "--yaw")
                yaw = argv[i];
            else if (argument == "--pitch")
                pitch = argv[i];
            else if (argument == "--stable-frames")
                stableFrames = argv[i];
            else if (argument == "--timeout-ms")
                timeoutMs = argv[i];
            else if (argument == "--shadows")
                shadows = argv[i];
            else if (argument == "--receiver-bias")
                receiverBias = argv[i];
            else if (argument == "--point-receiver-bias")
                pointReceiverBias = argv[i];
            else if (argument == "--constant-bias")
                constantBias = argv[i];
            else if (argument == "--slope-bias")
                slopeBias = argv[i];
            else if (argument == "--max-point-shadows")
                maxPointShadows = argv[i];
            else if (argument == "--point-shadow-distance")
                pointShadowDistance = argv[i];
            else if (argument == "--max-spot-shadows")
                maxSpotShadows = argv[i];
            else if (argument == "--spot-shadow-distance")
                spotShadowDistance = argv[i];
            else if (argument == "--exposure")
                exposure = argv[i];
            else if (argument == "--tone-mapper")
                toneMapper = argv[i];
            else if (argument == "--ibl")
                ibl = argv[i];
            else if (argument == "--skybox")
                skybox = argv[i];
            else if (argument == "--environment-intensity")
                environmentIntensity = argv[i];
            else if (argument == "--environment-rotation-deg")
                environmentRotationDegrees = argv[i];
            else if (argument == "--bloom")
                bloom = argv[i];
            else if (argument == "--bloom-threshold")
                bloomThreshold = argv[i];
            else if (argument == "--bloom-soft-knee")
                bloomSoftKnee = argv[i];
            else if (argument == "--bloom-intensity")
                bloomIntensity = argv[i];
            else if (argument == "--frustum")
                frustumCulling = argv[i];
            else if (argument == "--shadow-culling")
                shadowCulling = argv[i];
            else if (argument == "--shadow-distance")
                shadowDistance = argv[i];
            else if (argument == "--distance-culling")
                distanceCulling = argv[i];
            else if (argument == "--max-draw-distance")
                maxDrawDistance = argv[i];
            else if (argument == "--small-object-culling")
                smallObjectCulling = argv[i];
            else if (argument == "--min-projected-pixels")
                minProjectedPixels = argv[i];
            else if (argument == "--occlusion")
                occlusionCulling = argv[i];
            else if (argument == "--occlusion-bias")
                occlusionBias = argv[i];
            else if (argument == "--surface-debug")
                surfaceDebug = argv[i];
            else if (argument == "--surface-motion-scale")
                surfaceMotionScale = argv[i];
            else if (argument == "--ao")
                ambientOcclusion = argv[i];
            else if (argument == "--ssao-quality")
                ssaoQuality = argv[i];
            else if (argument == "--ssao-radius")
                ssaoRadius = argv[i];
            else if (argument == "--ssao-bias")
                ssaoBias = argv[i];
            else if (argument == "--ssao-intensity")
                ssaoIntensity = argv[i];
            else if (argument == "--ssao-power")
                ssaoPower = argv[i];
            else if (argument == "--cacao-quality")
                cacaoQuality = argv[i];
            else if (argument == "--cacao-resolution")
                cacaoResolution = argv[i];
            else if (argument == "--cacao-radius")
                cacaoRadius = argv[i];
            else if (argument == "--cacao-intensity")
                cacaoIntensity = argv[i];
            else if (argument == "--cacao-power")
                cacaoPower = argv[i];
            else if (argument == "--gtao-quality")
                gtaoQuality = argv[i];
            else if (argument == "--gtao-radius")
                gtaoRadius = argv[i];
            else if (argument == "--gtao-falloff")
                gtaoFalloff = argv[i];
            else if (argument == "--gtao-intensity")
                gtaoIntensity = argv[i];
            else if (argument == "--gtao-power")
                gtaoPower = argv[i];
            else if (argument == "--gtao-temporal-weight")
                gtaoTemporalWeight = argv[i];
            else if (argument == "--taa")
                temporalAntiAliasing = argv[i];
            else if (argument == "--taa-history-weight")
                taaHistoryWeight = argv[i];
            else if (argument == "--taa-sharpness")
                taaSharpness = argv[i];
            else if (argument == "--reflection")
                reflectionMode = argv[i];
            else if (argument == "--ssr-quality")
                ssrQuality = argv[i];
            else if (argument == "--ssr-max-distance")
                ssrMaxDistance = argv[i];
            else if (argument == "--ssr-thickness")
                ssrThickness = argv[i];
            else if (argument == "--ssr-max-roughness")
                ssrMaxRoughness = argv[i];
            else if (argument == "--ssr-intensity")
                ssrIntensity = argv[i];
            else if (argument == "--ssr-history-weight")
                ssrHistoryWeight = argv[i];
            else if (argument == "--gi")
                globalIlluminationMode = argv[i];
            else if (argument == "--ssgi-quality")
                ssgiQuality = argv[i];
            else if (argument == "--ssgi-max-distance")
                ssgiMaxDistance = argv[i];
            else if (argument == "--ssgi-thickness")
                ssgiThickness = argv[i];
            else if (argument == "--ssgi-intensity")
                ssgiIntensity = argv[i];
            else if (argument == "--ssgi-radiance-clamp")
                ssgiRadianceClamp = argv[i];
            else if (argument == "--ssgi-history-weight")
                ssgiHistoryWeight = argv[i];
            else if (argument == "--ddgi-radiance-clamp")
                ddgiRadianceClamp = argv[i];
            else if (argument == "--ddgi-debug")
                ddgiDebug = argv[i];
            else if (argument == "--screen-space-debug")
                screenSpaceDebug = argv[i];
            else
                screenSpaceDebugMip = argv[i];
        }
        else if (argument == "--no-wait")
            waitForLoad = false;
        else if (argument == "--force")
            force = true;
        else if (argument == "--load-after")
            loadAfter = true;
        else if (argument == "--include-gui")
            includeGui = true;
        else if (argument == "--no-gui")
            includeGui = false;
        else
            args.push_back(argument);
    }
    if (args.empty())
        throw std::invalid_argument("missing command");

    ParsedCommand parsed;
    parsed.endpoint = vkr::control::makeRuntimeControlEndpoint(pipeSuffix);
    parsed.jsonOutput = jsonOutput;
    parsed.waitForLoad = waitForLoad;
    parsed.force = force;
    parsed.loadAfter = loadAfter;
    parsed.includeGui = includeGui;
    if (stableFrames)
        parsed.stableFrames =
            parsePositiveUint32(*stableFrames, "--stable-frames");
    if (timeoutMs)
        parsed.timeoutMs = parsePositiveUint32(*timeoutMs, "--timeout-ms");
    if (args == std::vector<std::string>{"ping"}) {
        parsed.method = "system.ping";
    } else if (args == std::vector<std::string>{"info"}) {
        parsed.method = "system.info";
    } else if (args == std::vector<std::string>{"quit"}) {
        parsed.method = "app.quit";
    } else if (args == std::vector<std::string>{"stats"}) {
        parsed.method = "stats.last_load";
    } else if (args.size() == 2 && args[0] == "scene" &&
               args[1] == "list") {
        parsed.method = "scene.list";
    } else if (args.size() == 2 && args[0] == "scene" &&
               args[1] == "current") {
        parsed.method = "scene.current";
    } else if (args.size() == 2 && args[0] == "scene" &&
               args[1] == "reload") {
        parsed.method = "scene.reload";
    } else if (args.size() == 3 && args[0] == "scene" &&
               args[1] == "load") {
        parsed.method = "scene.load";
        parsed.params = {{"name", args[2]}};
    } else if ((args.size() == 2 || args.size() == 3) &&
               args[0] == "load" && args[1] == "status") {
        parsed.method = "load.status";
        if (args.size() == 3)
            parsed.params = {{"taskId", std::stoull(args[2])}};
    } else if ((args.size() == 2 || args.size() == 3) &&
               args[0] == "load" && args[1] == "cancel") {
        parsed.method = "load.cancel";
        if (args.size() == 3)
            parsed.params = {{"taskId", std::stoull(args[2])}};
    } else if (args == std::vector<std::string>{"asset", "catalog"}) {
        parsed.method = "asset.catalog";
    } else if ((args.size() == 2 || args.size() == 3) &&
               args[0] == "asset" && args[1] == "status") {
        parsed.method = "asset.status";
        if (args.size() == 3)
            parsed.params = {{"name", args[2]}};
    } else if (args.size() == 3 && args[0] == "asset" &&
               args[1] == "validation") {
        parsed.method = "asset.validation";
        parsed.params = {{"name", args[2]}};
    } else if (args.size() == 3 && args[0] == "asset" &&
               args[1] == "import") {
        parsed.method = "asset.import";
        parsed.params = {{"name", args[2]},
                         {"force", force},
                         {"loadAfter", loadAfter}};
    } else if ((args.size() == 2 || args.size() == 3) &&
               args[0] == "asset" && args[1] == "cancel") {
        parsed.method = "asset.cancel";
        if (args.size() == 3)
            parsed.params = {{"taskId", std::stoull(args[2])}};
    } else if (args == std::vector<std::string>{"asset", "cache-info"}) {
        parsed.method = "asset.cache_info";
    } else if (args.size() == 2 && args[0] == "texture-limit" &&
               args[1] == "get") {
        parsed.method = "texture_limit.get";
    } else if (args.size() == 3 && args[0] == "texture-limit" &&
               args[1] == "set") {
        parsed.method = "texture_limit.set";
        parsed.params = {{"value", parseTextureLimit(args[2])}};
    } else if (args.size() == 2 && args[0] == "shader" &&
               args[1] == "list") {
        parsed.method = "shader.list";
    } else if (args.size() == 2 && args[0] == "shader" &&
               args[1] == "current") {
        parsed.method = "shader.current";
    } else if (args.size() == 3 && args[0] == "shader" &&
               args[1] == "set") {
        parsed.method = "shader.set";
        parsed.params = {{"name", args[2]}};
    } else if (args.size() == 2 && args[0] == "environment" &&
               args[1] == "list") {
        parsed.method = "environment.list";
    } else if (args.size() == 2 && args[0] == "environment" &&
               args[1] == "current") {
        parsed.method = "environment.current";
    } else if (args.size() == 2 && args[0] == "environment" &&
               args[1] == "reload") {
        parsed.method = "environment.reload";
    } else if (args.size() == 3 && args[0] == "environment" &&
               args[1] == "set") {
        parsed.method = "environment.set";
        parsed.params = {{"name", args[2]}};
    } else if (args == std::vector<std::string>{"camera", "get"}) {
        parsed.method = "camera.get";
    } else if (args == std::vector<std::string>{"camera", "set"}) {
        if (!position || !yaw || !pitch) {
            throw std::invalid_argument(
                "camera set requires --position, --yaw, and --pitch");
        }
        parsed.method = "camera.set";
        parsed.params = {{"position", parsePosition(*position)},
                         {"yaw", parseFiniteFloat(*yaw, "--yaw")},
                         {"pitch", parseFiniteFloat(*pitch, "--pitch")}};
    } else if (args.size() == 4 && args[0] == "window" &&
               args[1] == "resize") {
        parsed.method = "window.resize";
        const uint32_t width =
            parsePositiveUint32(args[2], "window width");
        const uint32_t height =
            parsePositiveUint32(args[3], "window height");
        if (width > 16384 || height > 16384)
            throw std::invalid_argument(
                "window dimensions must be in 1..16384");
        parsed.params = {{"width", width}, {"height", height}};
    } else if (args == std::vector<std::string>{"render", "status"}) {
        parsed.method = "render.status";
    } else if (args == std::vector<std::string>{"render", "wait"}) {
        parsed.method = "render.status";
        parsed.renderWait = true;
    } else if (args ==
               std::vector<std::string>{"render-settings", "get"}) {
        parsed.method = "render_settings.get";
    } else if (args ==
               std::vector<std::string>{"render-settings", "set"}) {
        parsed.method = "render_settings.set";
        if (shadows)
            parsed.params["shadowsEnabled"] =
                parseOnOff(*shadows, "--shadows");
        if (receiverBias)
            parsed.params["shadowReceiverBias"] =
                parseFiniteFloat(*receiverBias, "--receiver-bias");
        if (pointReceiverBias)
            parsed.params["pointShadowReceiverBiasWorld"] =
                parseFiniteFloat(*pointReceiverBias,
                                 "--point-receiver-bias");
        if (constantBias)
            parsed.params["shadowConstantBias"] =
                parseFiniteFloat(*constantBias, "--constant-bias");
        if (slopeBias)
            parsed.params["shadowSlopeBias"] =
                parseFiniteFloat(*slopeBias, "--slope-bias");
        if (maxPointShadows)
            parsed.params["maxPointShadowLights"] =
                parseUint32(*maxPointShadows, "--max-point-shadows");
        if (pointShadowDistance)
            parsed.params["pointShadowDistance"] = parseFiniteFloat(
                *pointShadowDistance, "--point-shadow-distance");
        if (maxSpotShadows)
            parsed.params["maxSpotShadowLights"] =
                parseUint32(*maxSpotShadows, "--max-spot-shadows");
        if (spotShadowDistance)
            parsed.params["spotShadowDistance"] = parseFiniteFloat(
                *spotShadowDistance, "--spot-shadow-distance");
        if (exposure)
            parsed.params["exposureEv"] =
                parseFiniteFloat(*exposure, "--exposure");
        if (toneMapper) {
            if (*toneMapper != "passthrough" && *toneMapper != "reinhard" &&
                *toneMapper != "aces") {
                throw std::invalid_argument(
                    "--tone-mapper must be passthrough, reinhard, or aces");
            }
            parsed.params["toneMapper"] = *toneMapper;
        }
        if (ibl)
            parsed.params["iblEnabled"] = parseOnOff(*ibl, "--ibl");
        if (skybox) {
            parsed.params["skyboxEnabled"] =
                parseOnOff(*skybox, "--skybox");
        }
        if (environmentIntensity) {
            parsed.params["environmentIntensity"] = parseFiniteFloat(
                *environmentIntensity, "--environment-intensity");
        }
        if (environmentRotationDegrees) {
            constexpr float kDegreesToRadians =
                3.14159265358979323846f / 180.0f;
            parsed.params["environmentRotationRadians"] =
                parseFiniteFloat(*environmentRotationDegrees,
                                 "--environment-rotation-deg") *
                kDegreesToRadians;
        }
        if (bloom)
            parsed.params["bloomEnabled"] =
                parseOnOff(*bloom, "--bloom");
        if (bloomThreshold) {
            parsed.params["bloomThreshold"] =
                parseFiniteFloat(*bloomThreshold, "--bloom-threshold");
        }
        if (bloomSoftKnee) {
            parsed.params["bloomSoftKnee"] =
                parseFiniteFloat(*bloomSoftKnee, "--bloom-soft-knee");
        }
        if (bloomIntensity) {
            parsed.params["bloomIntensity"] =
                parseFiniteFloat(*bloomIntensity, "--bloom-intensity");
        }
        if (frustumCulling) {
            parsed.params["frustumCullingEnabled"] =
                parseOnOff(*frustumCulling, "--frustum");
        }
        if (shadowCulling) {
            parsed.params["shadowCullingEnabled"] =
                parseOnOff(*shadowCulling, "--shadow-culling");
        }
        if (shadowDistance) {
            parsed.params["shadowDistance"] =
                parseFiniteFloat(*shadowDistance, "--shadow-distance");
        }
        if (distanceCulling) {
            parsed.params["distanceCullingEnabled"] =
                parseOnOff(*distanceCulling, "--distance-culling");
        }
        if (maxDrawDistance) {
            parsed.params["maxDrawDistance"] = parseFiniteFloat(
                *maxDrawDistance, "--max-draw-distance");
        }
        if (smallObjectCulling) {
            parsed.params["smallObjectCullingEnabled"] =
                parseOnOff(*smallObjectCulling,
                           "--small-object-culling");
        }
        if (minProjectedPixels) {
            parsed.params["minProjectedSizePixels"] = parseFiniteFloat(
                *minProjectedPixels, "--min-projected-pixels");
        }
        if (occlusionCulling) {
            parsed.params["occlusionCullingEnabled"] =
                parseOnOff(*occlusionCulling, "--occlusion");
        }
        if (occlusionBias) {
            parsed.params["occlusionDepthBias"] =
                parseFiniteFloat(*occlusionBias, "--occlusion-bias");
        }
        if (surfaceDebug) {
            if (*surfaceDebug != "none" && *surfaceDebug != "normal" &&
                *surfaceDebug != "roughness" &&
                *surfaceDebug != "motion" &&
                *surfaceDebug != "history-validity") {
                throw std::invalid_argument(
                    "--surface-debug must be none, normal, roughness, "
                    "motion, or history-validity");
            }
            parsed.params["surfaceDebugView"] = *surfaceDebug;
        }
        if (surfaceMotionScale) {
            parsed.params["surfaceMotionDebugScale"] = parseFiniteFloat(
                *surfaceMotionScale, "--surface-motion-scale");
        }
        if (ambientOcclusion) {
            if (*ambientOcclusion != "off" &&
                *ambientOcclusion != "ssao" &&
                *ambientOcclusion != "cacao" &&
                *ambientOcclusion != "gtao") {
                throw std::invalid_argument(
                    "--ao must be off, ssao, cacao, or gtao");
            }
            parsed.params["ambientOcclusionMode"] = *ambientOcclusion;
        }
        if (ssaoQuality) {
            if (*ssaoQuality != "low" && *ssaoQuality != "medium" &&
                *ssaoQuality != "high") {
                throw std::invalid_argument(
                    "--ssao-quality must be low, medium, or high");
            }
            parsed.params["ssaoQuality"] = *ssaoQuality;
        }
        if (ssaoRadius)
            parsed.params["ssaoRadius"] =
                parseFiniteFloat(*ssaoRadius, "--ssao-radius");
        if (ssaoBias)
            parsed.params["ssaoBias"] =
                parseFiniteFloat(*ssaoBias, "--ssao-bias");
        if (ssaoIntensity)
            parsed.params["ssaoIntensity"] =
                parseFiniteFloat(*ssaoIntensity, "--ssao-intensity");
        if (ssaoPower)
            parsed.params["ssaoPower"] =
                parseFiniteFloat(*ssaoPower, "--ssao-power");
        if (cacaoQuality) {
            if (*cacaoQuality != "lowest" && *cacaoQuality != "low" &&
                *cacaoQuality != "medium" && *cacaoQuality != "high" &&
                *cacaoQuality != "highest") {
                throw std::invalid_argument(
                    "--cacao-quality must be lowest, low, medium, high, or "
                    "highest");
            }
            parsed.params["cacaoQuality"] = *cacaoQuality;
        }
        if (cacaoResolution) {
            if (*cacaoResolution != "native" &&
                *cacaoResolution != "half") {
                throw std::invalid_argument(
                    "--cacao-resolution must be native or half");
            }
            parsed.params["cacaoResolution"] = *cacaoResolution;
        }
        if (cacaoRadius)
            parsed.params["cacaoRadius"] =
                parseFiniteFloat(*cacaoRadius, "--cacao-radius");
        if (cacaoIntensity)
            parsed.params["cacaoIntensity"] =
                parseFiniteFloat(*cacaoIntensity, "--cacao-intensity");
        if (cacaoPower)
            parsed.params["cacaoPower"] =
                parseFiniteFloat(*cacaoPower, "--cacao-power");
        if (gtaoQuality) {
            if (*gtaoQuality != "low" && *gtaoQuality != "medium" &&
                *gtaoQuality != "high") {
                throw std::invalid_argument(
                    "--gtao-quality must be low, medium, or high");
            }
            parsed.params["gtaoQuality"] = *gtaoQuality;
        }
        if (gtaoRadius)
            parsed.params["gtaoRadius"] =
                parseFiniteFloat(*gtaoRadius, "--gtao-radius");
        if (gtaoFalloff)
            parsed.params["gtaoFalloff"] =
                parseFiniteFloat(*gtaoFalloff, "--gtao-falloff");
        if (gtaoIntensity)
            parsed.params["gtaoIntensity"] =
                parseFiniteFloat(*gtaoIntensity, "--gtao-intensity");
        if (gtaoPower)
            parsed.params["gtaoPower"] =
                parseFiniteFloat(*gtaoPower, "--gtao-power");
        if (gtaoTemporalWeight)
            parsed.params["gtaoTemporalWeight"] = parseFiniteFloat(
                *gtaoTemporalWeight, "--gtao-temporal-weight");
        if (temporalAntiAliasing) {
            if (*temporalAntiAliasing != "off" &&
                *temporalAntiAliasing != "taa") {
                throw std::invalid_argument("--taa must be off or taa");
            }
            parsed.params["temporalAntiAliasingMode"] =
                *temporalAntiAliasing;
        }
        if (taaHistoryWeight) {
            parsed.params["taaHistoryWeight"] = parseFiniteFloat(
                *taaHistoryWeight, "--taa-history-weight");
        }
        if (taaSharpness) {
            parsed.params["taaSharpness"] =
                parseFiniteFloat(*taaSharpness, "--taa-sharpness");
        }
        if (reflectionMode) {
            if (*reflectionMode != "ibl-only" && *reflectionMode != "ssr")
                throw std::invalid_argument(
                    "--reflection must be ibl-only or ssr");
            parsed.params["reflectionMode"] = *reflectionMode;
        }
        if (ssrQuality) {
            if (*ssrQuality != "low" && *ssrQuality != "medium" &&
                *ssrQuality != "high")
                throw std::invalid_argument(
                    "--ssr-quality must be low, medium, or high");
            parsed.params["ssrQuality"] = *ssrQuality;
        }
        if (ssrMaxDistance)
            parsed.params["ssrMaxDistance"] = parseFiniteFloat(
                *ssrMaxDistance, "--ssr-max-distance");
        if (ssrThickness)
            parsed.params["ssrThickness"] = parseFiniteFloat(
                *ssrThickness, "--ssr-thickness");
        if (ssrMaxRoughness)
            parsed.params["ssrMaxRoughness"] = parseFiniteFloat(
                *ssrMaxRoughness, "--ssr-max-roughness");
        if (ssrIntensity)
            parsed.params["ssrIntensity"] = parseFiniteFloat(
                *ssrIntensity, "--ssr-intensity");
        if (ssrHistoryWeight)
            parsed.params["ssrHistoryWeight"] = parseFiniteFloat(
                *ssrHistoryWeight, "--ssr-history-weight");
        if (globalIlluminationMode) {
            if (*globalIlluminationMode != "ambient-or-ibl" &&
                *globalIlluminationMode != "ssgi" &&
                *globalIlluminationMode != "ddgi" &&
                *globalIlluminationMode != "ssgi-ddgi")
                throw std::invalid_argument(
                    "--gi must be ambient-or-ibl, ssgi, ddgi, or ssgi-ddgi");
            parsed.params["globalIlluminationMode"] =
                *globalIlluminationMode;
        }
        if (ssgiQuality) {
            if (*ssgiQuality != "low" && *ssgiQuality != "medium" &&
                *ssgiQuality != "high")
                throw std::invalid_argument(
                    "--ssgi-quality must be low, medium, or high");
            parsed.params["ssgiQuality"] = *ssgiQuality;
        }
        if (ssgiMaxDistance)
            parsed.params["ssgiMaxDistance"] = parseFiniteFloat(
                *ssgiMaxDistance, "--ssgi-max-distance");
        if (ssgiThickness)
            parsed.params["ssgiThickness"] = parseFiniteFloat(
                *ssgiThickness, "--ssgi-thickness");
        if (ssgiIntensity)
            parsed.params["ssgiIntensity"] = parseFiniteFloat(
                *ssgiIntensity, "--ssgi-intensity");
        if (ssgiRadianceClamp)
            parsed.params["ssgiRadianceClamp"] = parseFiniteFloat(
                *ssgiRadianceClamp, "--ssgi-radiance-clamp");
        if (ssgiHistoryWeight)
            parsed.params["ssgiHistoryWeight"] = parseFiniteFloat(
                *ssgiHistoryWeight, "--ssgi-history-weight");
        if (ddgiRadianceClamp)
            parsed.params["ddgiRadianceClamp"] = parseFiniteFloat(
                *ddgiRadianceClamp, "--ddgi-radiance-clamp");
        if (ddgiDebug) {
            if (*ddgiDebug != "none" && *ddgiDebug != "irradiance" &&
                *ddgiDebug != "distance" &&
                *ddgiDebug != "classification") {
                throw std::invalid_argument(
                    "--ddgi-debug must be none, irradiance, distance, or classification");
            }
            parsed.params["ddgiDebugView"] = *ddgiDebug;
        }
        if (screenSpaceDebug) {
            if (*screenSpaceDebug != "none" &&
                *screenSpaceDebug != "nearest-depth" &&
                *screenSpaceDebug != "scene-color" &&
                *screenSpaceDebug != "ssao-raw" &&
                *screenSpaceDebug != "ssao-filtered" &&
                *screenSpaceDebug != "cacao-output" &&
                *screenSpaceDebug != "gtao-raw" &&
                *screenSpaceDebug != "gtao-temporal" &&
                *screenSpaceDebug != "gtao-filtered" &&
                *screenSpaceDebug != "gtao-rejection" &&
                *screenSpaceDebug != "gtao-history-weight" &&
                *screenSpaceDebug != "taa-history" &&
                *screenSpaceDebug != "taa-rejection" &&
                *screenSpaceDebug != "taa-history-weight" &&
                *screenSpaceDebug != "ssr-raw" &&
                *screenSpaceDebug != "ssr-temporal" &&
                *screenSpaceDebug != "ssr-filtered" &&
                *screenSpaceDebug != "ssr-confidence" &&
                *screenSpaceDebug != "ssr-rejection" &&
                *screenSpaceDebug != "ssgi-raw" &&
                *screenSpaceDebug != "ssgi-temporal" &&
                *screenSpaceDebug != "ssgi-filtered" &&
                *screenSpaceDebug != "ssgi-confidence" &&
                *screenSpaceDebug != "ssgi-variance" &&
                *screenSpaceDebug != "ssgi-rejection") {
                throw std::invalid_argument(
                    "--screen-space-debug must be none, nearest-depth, "
                    "scene-color, ssao-raw, ssao-filtered, cacao-output, "
                    "gtao-raw, gtao-temporal, gtao-filtered, gtao-rejection, "
                    "gtao-history-weight, taa-history, taa-rejection, or "
                    "taa-history-weight, ssr-raw, ssr-temporal, "
                    "ssr-filtered, ssr-confidence, ssr-rejection, ssgi-raw, "
                    "ssgi-temporal, ssgi-filtered, ssgi-confidence, "
                    "ssgi-variance, or ssgi-rejection");
            }
            parsed.params["screenSpaceDebugView"] = *screenSpaceDebug;
        }
        if (screenSpaceDebugMip) {
            parsed.params["screenSpaceDebugMip"] =
                parseUint32(*screenSpaceDebugMip,
                            "--screen-space-debug-mip");
        }
        if (parsed.params.empty())
            throw std::invalid_argument(
                "render-settings set requires at least one option");
    } else if (args.size() == 3 && args[0] == "capture" &&
               args[1] == "screenshot") {
        parsed.method = "capture.screenshot";
        parsed.params = {{"path", args[2]}, {"includeGui", includeGui}};
    } else if (args.size() == 3 && args[0] == "capture" &&
               args[1] == "status") {
        parsed.method = "capture.status";
        parsed.params = {{"taskId", std::stoull(args[2])}};
    } else if (args.size() == 3 && args[0] == "capture" &&
               args[1] == "cancel") {
        parsed.method = "capture.cancel";
        parsed.params = {{"taskId", std::stoull(args[2])}};
    } else {
        throw std::invalid_argument("unknown or incomplete command");
    }
    return parsed;
}

std::optional<uint64_t> loadTaskId(const Json &result) {
    if (result.contains("taskId") && result["taskId"].is_number_unsigned())
        return result["taskId"].get<uint64_t>();
    if (result.contains("loadTask") && result["loadTask"].is_object() &&
        result["loadTask"].contains("taskId")) {
        return result["loadTask"]["taskId"].get<uint64_t>();
    }
    return std::nullopt;
}

Json waitForLoad(const vkr::RuntimeControlClientWin32 &client,
                 uint64_t taskId) {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const Json request = {
            {"id", 2},
            {"method", "load.status"},
            {"params", {{"taskId", taskId}}}};
        const Json response = client.send(request);
        if (!response.value("ok", false))
            return response;
        const Json &result = response.at("result");
        if (result.value("terminal", false))
            return response;
    }
}

bool terminalLoadFailed(const Json &load) {
    if (!load.is_object() || !load.value("terminal", false))
        return false;
    const std::string state = load.value("state", std::string{});
    return state == "Failed" || state == "Cancelled";
}

Json waitForRender(const vkr::RuntimeControlClientWin32 &client,
                   uint32_t stableFrameTarget, uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    std::optional<uint64_t> generation;
    uint64_t lastPresented = 0;
    uint64_t stableFrames = 0;
    bool sawMinimized = false;
    uint64_t requestId = 1;

    while (std::chrono::steady_clock::now() < deadline) {
        const Json response =
            client.invoke(requestId++, "render.status");
        if (!response.value("ok", false))
            return response;
        const Json &status = response.at("result");

        Json load = status.value("loadTask", Json(nullptr));
        if (load.is_object() && !load.value("terminal", false) &&
            load.contains("taskId")) {
            const Json loadResponse = client.invoke(
                requestId++, "load.status",
                {{"taskId", load.at("taskId")}});
            if (!loadResponse.value("ok", false))
                return loadResponse;
            load = loadResponse.at("result");
        }
        if (terminalLoadFailed(load)) {
            return vkr::makeRuntimeError(
                1, "render_load_failed",
                "Scene load reached terminal state " +
                    load.value("state", std::string("Unknown")) + ".");
        }

        const uint64_t currentGeneration =
            status.value("sceneGeneration", uint64_t{0});
        const uint64_t presented =
            status.value("presentedFrames", uint64_t{0});
        const bool minimized = status.value("minimized", false);
        sawMinimized = sawMinimized || minimized;
        const bool loadReady =
            load.is_null() || load.value("terminal", false);
        const bool ready = status.value("rendering", false) && loadReady &&
                           status.value("pendingUpload", uint64_t{0}) == 0 &&
                           !minimized &&
                           !status.value("swapchainRecreatePending", false);

        if (!generation || *generation != currentGeneration ||
            presented < lastPresented || !ready) {
            stableFrames = 0;
        } else {
            stableFrames += presented - lastPresented;
        }
        generation = currentGeneration;
        lastPresented = presented;

        if (ready && stableFrames >= stableFrameTarget) {
            Json result = status;
            result["stable"] = true;
            result["stableFrames"] = stableFrames;
            result["stableFrameTarget"] = stableFrameTarget;
            return vkr::makeRuntimeSuccess(1, std::move(result));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return vkr::makeRuntimeError(
        1, sawMinimized ? "window_not_rendering" : "render_wait_timeout",
        sawMinimized
            ? "The window was minimized and did not present stable frames."
            : "Timed out waiting for stable presented frames.");
}

void printStats(const Json &stats) {
    const auto &timings = stats.at("timingsMs");
    const auto &counts = stats.at("counts");
    const auto &bytes = stats.at("bytes");
    const auto &sync = stats.at("synchronization");
    const double uploadMiB =
        static_cast<double>(bytes.at("textureUpload").get<uint64_t>() +
                            bytes.at("vertexUpload").get<uint64_t>() +
                            bytes.at("indexUpload").get<uint64_t>()) /
        (1024.0 * 1024.0);
    std::cout << "scene: " << stats.at("scene").get<std::string>() << '\n'
              << "success: " << std::boolalpha
              << stats.at("success").get<bool>() << '\n'
              << std::fixed << std::setprecision(2)
              << "load: " << timings.at("total").get<double>() << " ms\n"
              << "textures: " << counts.at("gpuTextures").get<uint64_t>()
              << ", meshes: " << counts.at("gpuMeshes").get<uint64_t>()
              << ", upload: " << uploadMiB << " MiB\n"
              << "legacy submits/waits: "
              << sync.at("legacySubmits").get<uint64_t>() << "/"
              << sync.at("queueWaitIdleCalls").get<uint64_t>() << '\n'
              << "batch submits/fence waits: "
              << sync.at("batchSubmits").get<uint64_t>() << "/"
              << sync.at("fenceWaitCalls").get<uint64_t>() << '\n';
}

void printHuman(const std::string &method, const Json &result) {
    if (method == "system.ping") {
        std::cout << result.at("message").get<std::string>() << '\n';
    } else if (method == "scene.list") {
        for (const auto &name : result.at("scenes"))
            std::cout << name.get<std::string>() << '\n';
    } else if (method == "shader.list") {
        for (const auto &name : result.at("shaders"))
            std::cout << name.get<std::string>() << '\n';
    } else if (method == "environment.list") {
        for (const auto &entry : result.at("entries")) {
            std::cout
                << entry.at("name").get<std::string>() << " ["
                << (entry.value("ready", false) ? "Ready" : "Unavailable")
                << "]\n";
        }
    } else if (method == "environment.current") {
        std::cout
            << (result.at("publishedId").is_null()
                    ? std::string("<none>")
                    : result.at("publishedId").get<std::string>())
            << (result.value("ready", false) ? " [Ready]" : "") << '\n';
    } else if (method == "scene.current" || method == "shader.current") {
        std::cout << (result.at("name").is_null()
                          ? std::string("<none>")
                          : result.at("name").get<std::string>())
                  << '\n';
    } else if (method == "texture_limit.get" ||
               method == "texture_limit.set") {
        const uint32_t value = result.at("value").get<uint32_t>();
        std::cout << (value == 0 ? std::string("Full")
                                : std::to_string(value))
                  << '\n';
        if (result.contains("loadStats"))
            printStats(result.at("loadStats"));
    } else if (method == "scene.load" || method == "scene.reload") {
        if (result.contains("loadStats"))
            printStats(result.at("loadStats"));
        else if (result.contains("taskId"))
            std::cout << "task " << result.at("taskId").get<uint64_t>()
                      << ": " << result.value("state", "Queued") << '\n';
        else
            std::cout << result.at("scene").get<std::string>() << '\n';
    } else if (method == "load.status") {
        std::cout << "task " << result.at("taskId").get<uint64_t>()
                  << ": " << result.at("state").get<std::string>() << '\n';
        if (result.contains("loadStats"))
            printStats(result.at("loadStats"));
        if (result.contains("error"))
            std::cout << "error: " << result.at("error").get<std::string>()
                      << '\n';
    } else if (method == "load.cancel") {
        std::cout << "cancel requested for task "
                  << result.at("taskId").get<uint64_t>() << '\n';
    } else if (method == "asset.catalog") {
        for (const auto &entry : result.at("entries")) {
            std::cout << entry.at("scene").get<std::string>() << " ["
                      << entry.at("state").get<std::string>() << "] "
                      << entry.at("profileId").get<std::string>() << '\n';
        }
    } else if (method == "asset.status") {
        std::cout << result.at("scene").get<std::string>() << ": "
                  << result.at("state").get<std::string>() << " ("
                  << result.at("profileId").get<std::string>() << ")\n";
        if (!result.value("reason", std::string{}).empty())
            std::cout << result.at("reason").get<std::string>() << '\n';
    } else if (method == "asset.validation") {
        std::cout << result.at("scene").get<std::string>() << ": "
                  << result.at("state").get<std::string>() << '\n';
        if (result.contains("validator")) {
            std::cout << "validator: "
                      << result.at("validator").value("version", "unknown")
                      << '\n';
        }
        if (result.contains("counts")) {
            const Json &counts = result.at("counts");
            std::cout << "errors " << counts.value("errors", 0)
                      << ", warnings " << counts.value("warnings", 0)
                      << ", infos " << counts.value("infos", 0)
                      << ", hints " << counts.value("hints", 0) << '\n';
        }
        if (!result.value("reason", std::string{}).empty())
            std::cout << result.at("reason").get<std::string>() << '\n';
    } else if (method == "asset.import") {
        if (result.contains("taskId"))
            std::cout << "asset task "
                      << result.at("taskId").get<uint64_t>() << ": "
                      << result.value("state", "Queued") << '\n';
        else
            std::cout << result.dump(2) << '\n';
    } else if (method == "asset.cancel") {
        std::cout << "cancel requested for asset task "
                  << result.at("taskId").get<uint64_t>() << '\n';
    } else if (method == "asset.cache_info") {
        std::cout << result.at("root").get<std::string>() << '\n'
                  << result.at("files").get<uint64_t>() << " files, "
                  << std::fixed << std::setprecision(2)
                  << static_cast<double>(result.at("bytes").get<uint64_t>()) /
                         (1024.0 * 1024.0)
                  << " MiB\n";
    } else if (method == "shader.set") {
        std::cout << result.at("shader").get<std::string>() << '\n';
    } else if (method == "environment.set" ||
               method == "environment.reload") {
        if (result.contains("taskId") &&
            !result.at("taskId").is_null()) {
            std::cout << "environment task "
                      << result.at("taskId").get<uint64_t>() << '\n';
        } else {
            std::cout << "environment: none\n";
        }
    } else if (method == "camera.get" || method == "camera.set") {
        const auto &position = result.at("position");
        std::cout << std::fixed << std::setprecision(3) << "position: "
                  << position.at(0).get<float>() << ", "
                  << position.at(1).get<float>() << ", "
                  << position.at(2).get<float>() << '\n'
                  << "yaw: " << result.at("yaw").get<float>()
                  << ", pitch: " << result.at("pitch").get<float>()
                  << '\n';
    } else if (method == "window.resize") {
        std::cout << result.at("width").get<uint32_t>() << "x"
                  << result.at("height").get<uint32_t>() << '\n';
    } else if (method == "render.status") {
        const Json &scene = result.at("scene");
        std::cout << "scene: "
                  << (scene.is_object()
                          ? scene.at("name").get<std::string>()
                          : std::string("<none>"))
                  << ", generation: "
                  << result.at("sceneGeneration").get<uint64_t>() << '\n'
                  << "frames: "
                  << result.at("presentedFrames").get<uint64_t>()
                  << ", submitted/completed: "
                  << result.at("frameSerial").get<uint64_t>() << "/"
                  << result.at("completedSubmissionSerial").get<uint64_t>()
                  << ", pending upload: "
                  << result.at("pendingUpload").get<uint64_t>() << '\n'
                  << "rendering: " << std::boolalpha
                  << result.at("rendering").get<bool>()
                  << ", minimized: "
                  << result.at("minimized").get<bool>() << '\n';
        if (result.contains("culling")) {
            const Json &culling = result.at("culling");
            std::cout << "culling visible/source: "
                      << culling.value("cameraVisible", 0u) << "/"
                      << culling.value("sourceDraws", 0u)
                      << ", frustum/distance/small: "
                      << culling.value("frustumCulled", 0u) << "/"
                      << culling.value("distanceCulled", 0u) << "/"
                      << culling.value("smallObjectCulled", 0u)
                      << ", GPU occluded: "
                      << culling.value("gpuOccluded", 0u) << "/"
                      << culling.value("occlusionCandidates", 0u) << '\n';
        }
        if (result.contains("surfaceData")) {
            const Json &surface = result.at("surfaceData");
            std::cout << "Surface Data: "
                      << (surface.value("supported", false) ? "supported"
                                                            : "unsupported")
                      << ", active: "
                      << (surface.value("active", false) ? "yes" : "no")
                      << ", debug: "
                      << surface.value("debugView", std::string("none"))
                      << ", history: "
                      << surface.value("historyValidItems", 0u) << "/"
                      << surface.value("itemCount", 0u) << '\n';
        }
        const Json &gpuTimings = result.at("gpuTimings");
        if (gpuTimings.value("available", false)) {
            std::cout << "GPU frame "
                      << gpuTimings.at("frameSerial").get<uint64_t>()
                      << ": " << std::fixed << std::setprecision(3)
                      << gpuTimings.at("totalMs").get<double>()
                      << " ms total\n";
            for (const auto &[name, milliseconds] :
                 gpuTimings.at("passes").items()) {
                std::cout << "  " << name << ": "
                          << milliseconds.get<double>() << " ms\n";
            }
        } else {
            std::cout << "GPU timings: unavailable\n";
        }
        if (result.value("stable", false)) {
            std::cout << "stable frames: "
                      << result.at("stableFrames").get<uint64_t>() << "/"
                      << result.at("stableFrameTarget").get<uint32_t>()
                      << '\n';
        }
    } else if (method == "render_settings.get" ||
               method == "render_settings.set") {
        std::cout << "shadows: "
                  << (result.at("shadowsEnabled").get<bool>() ? "on" : "off")
                  << ", map: " << result.at("shadowMapSize").get<uint32_t>()
                  << "\nreceiver/constant/slope bias: "
                  << result.at("shadowReceiverBias").get<float>() << "/"
                  << result.at("shadowConstantBias").get<float>() << "/"
                  << result.at("shadowSlopeBias").get<float>()
                  << "\npoint world bias: "
                  << result.at("pointShadowReceiverBiasWorld").get<float>()
                  << "\nexposure: " << result.at("exposureEv").get<float>()
                  << " EV, tone mapper: "
                  << result.at("toneMapper").get<std::string>()
                  << "\nIBL/skybox: "
                  << (result.at("iblEnabled").get<bool>() ? "on" : "off")
                  << "/"
                  << (result.at("skyboxEnabled").get<bool>() ? "on"
                                                              : "off")
                  << ", environment intensity: "
                  << result.at("environmentIntensity").get<float>()
                  << ", rotation: "
                  << result.at("environmentRotationRadians").get<float>()
                  << " rad\nBloom: "
                  << (result.at("bloomEnabled").get<bool>() ? "on" : "off")
                  << ", active: "
                  << (result.at("bloomActive").get<bool>() ? "yes" : "no")
                  << ", available: "
                  << (result.at("bloomAvailable").get<bool>() ? "yes" : "no")
                  << "\nthreshold/knee/intensity: "
                  << result.at("bloomThreshold").get<float>() << "/"
                  << result.at("bloomSoftKnee").get<float>() << "/"
                  << result.at("bloomIntensity").get<float>()
                  << "\nCulling frustum/shadow/distance/small/occlusion: "
                  << (result.at("frustumCullingEnabled").get<bool>()
                          ? "on"
                          : "off")
                  << "/"
                  << (result.at("shadowCullingEnabled").get<bool>()
                          ? "on"
                          : "off")
                  << "/"
                  << (result.at("distanceCullingEnabled").get<bool>()
                          ? "on"
                          : "off")
                  << "/"
                  << (result.at("smallObjectCullingEnabled").get<bool>()
                          ? "on"
                          : "off")
                  << "/"
                  << (result.at("occlusionCullingEnabled").get<bool>()
                          ? "on"
                          : "off")
                  << "\nshadow/max distance/min pixels/occlusion bias: "
                  << result.at("shadowDistance").get<float>() << "/"
                  << result.at("maxDrawDistance").get<float>() << "/"
                  << result.at("minProjectedSizePixels").get<float>()
                  << "/" << result.at("occlusionDepthBias").get<float>()
                  << "\nSurface debug: "
                  << result.at("surfaceDebugView").get<std::string>()
                  << ", motion scale: "
                  << result.at("surfaceMotionDebugScale").get<float>()
                  << ", available/active: "
                  << (result.at("surfaceDataAvailable").get<bool>() ? "yes"
                                                                     : "no")
                  << "/"
                  << (result.at("surfaceDataActive").get<bool>() ? "yes"
                                                                   : "no")
                  << "\nAO: "
                  << result.at("ambientOcclusionMode").get<std::string>()
                  << ", active/available: "
                  << (result.at("ssaoActive").get<bool>() ? "yes" : "no")
                  << "/"
                  << (result.at("ssaoAvailable").get<bool>() ? "yes" : "no")
                  << ", quality/radius/bias/intensity/power: "
                  << result.at("ssaoQuality").get<std::string>() << "/"
                  << result.at("ssaoRadius").get<float>() << "/"
                  << result.at("ssaoBias").get<float>() << "/"
                  << result.at("ssaoIntensity").get<float>() << "/"
                   << result.at("ssaoPower").get<float>()
                   << "\nCACAO active/available: "
                   << (result.at("cacaoActive").get<bool>() ? "yes" : "no")
                   << "/"
                   << (result.at("cacaoAvailable").get<bool>() ? "yes" : "no")
                   << ", quality/resolution/radius/intensity/power: "
                   << result.at("cacaoQuality").get<std::string>() << "/"
                   << result.at("cacaoResolution").get<std::string>() << "/"
                   << result.at("cacaoRadius").get<float>() << "/"
                   << result.at("cacaoIntensity").get<float>() << "/"
                   << result.at("cacaoPower").get<float>()
                  << "\nGTAO active/available/history: "
                  << (result.at("gtaoActive").get<bool>() ? "yes" : "no")
                  << "/"
                  << (result.at("gtaoAvailable").get<bool>() ? "yes" : "no")
                  << "/"
                  << (result.at("gtaoHistoryValid").get<bool>() ? "valid"
                                                                  : "reset")
                  << ", quality/radius/falloff/intensity/power/history: "
                  << result.at("gtaoQuality").get<std::string>() << "/"
                  << result.at("gtaoRadius").get<float>() << "/"
                  << result.at("gtaoFalloff").get<float>() << "/"
                  << result.at("gtaoIntensity").get<float>() << "/"
                  << result.at("gtaoPower").get<float>() << "/"
                  << result.at("gtaoTemporalWeight").get<float>()
                  << "\nSSR active/available/history: "
                  << (result.value("ssrActive", false) ? "yes" : "no")
                  << "/"
                  << (result.value("ssrAvailable", false) ? "yes" : "no")
                  << "/"
                  << (result.value("ssrHistoryValid", false) ? "valid"
                                                               : "reset")
                  << ", mode/quality/distance/thickness/roughness/intensity/history: "
                  << result.value("reflectionMode", std::string{"ibl-only"}) << "/"
                  << result.value("ssrQuality", std::string{"medium"}) << "/"
                  << result.value("ssrMaxDistance", 0.0f) << "/"
                  << result.value("ssrThickness", 0.0f) << "/"
                  << result.value("ssrMaxRoughness", 0.0f) << "/"
                  << result.value("ssrIntensity", 0.0f) << "/"
                  << result.value("ssrHistoryWeight", 0.0f)
                  << "\nSSGI active/available/history: "
                  << (result.value("ssgiActive", false) ? "yes" : "no")
                  << "/"
                  << (result.value("ssgiAvailable", false) ? "yes" : "no")
                  << "/"
                  << (result.value("ssgiHistoryValid", false) ? "valid"
                                                                 : "reset")
                  << ", mode/quality/distance/thickness/intensity/clamp/history: "
                  << result.value("globalIlluminationMode",
                                  std::string{"ambient-or-ibl"}) << "/"
                  << result.value("ssgiQuality", std::string{"medium"}) << "/"
                  << result.value("ssgiMaxDistance", 0.0f) << "/"
                  << result.value("ssgiThickness", 0.0f) << "/"
                  << result.value("ssgiIntensity", 0.0f) << "/"
                  << result.value("ssgiRadianceClamp", 0.0f) << "/"
                  << result.value("ssgiHistoryWeight", 0.0f)
                  << "\nDDGI active/available/component: "
                  << (result.value("ddgiActive", false) ? "yes" : "no")
                  << "/"
                  << (result.value("ddgiSupported", false) ? "yes" : "no")
                  << "/"
                  << (result.value("ddgiComponentPresent", false) ? "yes"
                                                                     : "no")
                  << ", clamp/debug: "
                  << result.value("ddgiRadianceClamp", 0.0f) << "/"
                  << result.value("ddgiDebugView", std::string{"none"})
                  << "\nScreen-space debug: "
                  << result.at("screenSpaceDebugView").get<std::string>()
                  << ", mip: "
                  << result.at("screenSpaceDebugMip").get<uint32_t>()
                  << '\n';
        const std::string bloomReason =
            result.value("bloomUnavailableReason", std::string{});
        if (!bloomReason.empty())
            std::cout << "Bloom unavailable: " << bloomReason << '\n';
        const std::string ssaoReason =
            result.value("ssaoUnavailableReason", std::string{});
        if (!ssaoReason.empty())
            std::cout << "SSAO unavailable: " << ssaoReason << '\n';
        const std::string cacaoReason =
            result.value("cacaoUnavailableReason", std::string{});
        if (!cacaoReason.empty())
            std::cout << "CACAO unavailable: " << cacaoReason << '\n';
        const std::string gtaoReason =
            result.value("gtaoUnavailableReason", std::string{});
        if (!gtaoReason.empty())
            std::cout << "GTAO unavailable: " << gtaoReason << '\n';
        const std::string occlusionReason =
            result.value("occlusionUnavailableReason", std::string{});
        if (!occlusionReason.empty()) {
            std::cout << "Occlusion unavailable: " << occlusionReason
                      << '\n';
        }
        const std::string surfaceReason =
            result.value("surfaceDataUnavailableReason", std::string{});
        if (!surfaceReason.empty())
            std::cout << "Surface Data unavailable: " << surfaceReason
                      << '\n';
    } else if (method == "capture.screenshot" ||
               method == "capture.status" ||
               method == "capture.cancel") {
        std::cout << "capture " << result.at("taskId").get<uint64_t>()
                  << ": " << result.at("state").get<std::string>() << '\n';
        if (result.at("result").is_object()) {
            const Json &capture = result.at("result");
            std::cout << capture.at("width").get<uint32_t>() << "x"
                      << capture.at("height").get<uint32_t>() << " -> "
                      << capture.at("outputPath").get<std::string>() << '\n'
                      << "sha256: "
                      << capture.at("sha256").get<std::string>() << '\n';
        }
        if (!result.at("error").is_null())
            std::cout << "error: "
                      << result.at("error").get<std::string>() << '\n';
    } else if (method == "stats.last_load") {
        printStats(result);
    } else if (method == "app.quit") {
        std::cout << "quitting\n";
    } else {
        std::cout << result.dump(2) << '\n';
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        const ParsedCommand command = parseCommand(argc, argv);
        const vkr::RuntimeControlClientWin32 client(command.endpoint);
        Json response = command.renderWait
                            ? waitForRender(client, command.stableFrames,
                                            command.timeoutMs)
                            : client.invoke(1, command.method,
                                            command.params);

        const bool startsLoad = command.method == "scene.load" ||
                                command.method == "scene.reload" ||
                                command.method == "texture_limit.set" ||
                                command.method == "asset.import" ||
                                command.method == "environment.set" ||
                                command.method == "environment.reload";
        if (response.value("ok", false) && command.waitForLoad &&
            startsLoad) {
            const auto taskId = loadTaskId(response.at("result"));
            if (taskId)
                response = waitForLoad(client, *taskId);
        }

        if (command.jsonOutput)
            std::cout << response.dump(2) << '\n';

        if (!response.value("ok", false)) {
            const Json error = response.value("error", Json::object());
            if (!command.jsonOutput) {
                std::cerr << "error[" << error.value("code", "unknown")
                          << "]: " << error.value("message", "unknown error")
                          << '\n';
            }
            return 1;
        }
        if (!command.jsonOutput) {
            const Json &result = response.at("result");
            const std::string outputMethod =
                startsLoad && result.contains("state") ? "load.status"
                                                        : command.method;
            printHuman(outputMethod, result);
        }
        if (startsLoad && response.at("result").contains("state")) {
            const std::string state =
                response.at("result").at("state").get<std::string>();
            if (state == "Failed" || state == "Cancelled")
                return 1;
        }
        return 0;
    } catch (const std::invalid_argument &e) {
        std::cerr << "error: " << e.what() << '\n';
        printUsage();
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
}
