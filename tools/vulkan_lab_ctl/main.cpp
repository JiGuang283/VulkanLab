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
        << "  VulkanLabCtl [--json] [--no-wait] [--force] "
           "[--load-after] asset import <scene>\n"
        << "  VulkanLabCtl [--json] asset cancel [task-id]\n"
        << "  VulkanLabCtl [--json] asset cache-info\n"
        << "  VulkanLabCtl [--json] texture-limit get\n"
        << "  VulkanLabCtl [--json] texture-limit set <full|512|1024|2048>\n"
        << "  VulkanLabCtl [--json] shader list|current\n"
        << "  VulkanLabCtl [--json] shader set <name>\n"
        << "  VulkanLabCtl [--json] camera get\n"
        << "  VulkanLabCtl [--json] camera set --position X,Y,Z --yaw Y --pitch P\n"
        << "  VulkanLabCtl [--json] render status\n"
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
                 argument == "--timeout-ms") {
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
            else
                timeoutMs = argv[i];
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
    } else if (args == std::vector<std::string>{"render", "status"}) {
        parsed.method = "render.status";
    } else if (args == std::vector<std::string>{"render", "wait"}) {
        parsed.method = "render.status";
        parsed.renderWait = true;
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
    } else if (method == "camera.get" || method == "camera.set") {
        const auto &position = result.at("position");
        std::cout << std::fixed << std::setprecision(3) << "position: "
                  << position.at(0).get<float>() << ", "
                  << position.at(1).get<float>() << ", "
                  << position.at(2).get<float>() << '\n'
                  << "yaw: " << result.at("yaw").get<float>()
                  << ", pitch: " << result.at("pitch").get<float>()
                  << '\n';
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
        if (result.value("stable", false)) {
            std::cout << "stable frames: "
                      << result.at("stableFrames").get<uint64_t>() << "/"
                      << result.at("stableFrameTarget").get<uint32_t>()
                      << '\n';
        }
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
                                command.method == "asset.import";
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
