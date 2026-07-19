#include "control/RuntimeControlProtocol.h"

#include <json.hpp>

#include <cstdint>
#include <optional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

using Json = nlohmann::json;

class Handle {
  public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr)
            CloseHandle(value_);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const { return value_; }
    bool valid() const {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }

  private:
    HANDLE value_;
};

struct ParsedCommand {
    std::string method;
    Json params = Json::object();
    bool jsonOutput = false;
    bool waitForLoad = true;
    bool force = false;
    bool loadAfter = false;
};

void printUsage() {
    std::cerr
        << "Usage:\n"
        << "  VulkanLabCtl [--json] ping|info|quit\n"
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
        << "  VulkanLabCtl [--json] stats\n";
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
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json")
            jsonOutput = true;
        else if (std::string(argv[i]) == "--no-wait")
            waitForLoad = false;
        else if (std::string(argv[i]) == "--force")
            force = true;
        else if (std::string(argv[i]) == "--load-after")
            loadAfter = true;
        else
            args.emplace_back(argv[i]);
    }
    if (args.empty())
        throw std::invalid_argument("missing command");

    ParsedCommand parsed;
    parsed.jsonOutput = jsonOutput;
    parsed.waitForLoad = waitForLoad;
    parsed.force = force;
    parsed.loadAfter = loadAfter;
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
    } else {
        throw std::invalid_argument("unknown or incomplete command");
    }
    return parsed;
}

bool writeExact(HANDLE pipe, const void *data, DWORD size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    DWORD completed = 0;
    while (completed < size) {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes + completed, size - completed, &written,
                       nullptr) ||
            written == 0) {
            return false;
        }
        completed += written;
    }
    return true;
}

bool readExact(HANDLE pipe, void *data, DWORD size) {
    auto *bytes = static_cast<unsigned char *>(data);
    DWORD completed = 0;
    while (completed < size) {
        DWORD read = 0;
        if (!ReadFile(pipe, bytes + completed, size - completed, &read,
                      nullptr) ||
            read == 0) {
            return false;
        }
        completed += read;
    }
    return true;
}

Json sendRequest(const Json &request) {
    if (!WaitNamedPipeW(vkr::control::kPipeName, 5000))
        throw std::runtime_error(
            "VulkanLab runtime control pipe is unavailable");

    Handle pipe(CreateFileW(vkr::control::kPipeName,
                            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr));
    if (!pipe.valid())
        throw std::runtime_error("failed to connect to VulkanLab runtime "
                                 "control pipe");

    const std::string payload = request.dump();
    if (payload.empty() || payload.size() > vkr::control::kMaxMessageBytes)
        throw std::runtime_error("request exceeds protocol message limit");
    const uint32_t length = static_cast<uint32_t>(payload.size());
    if (!writeExact(pipe.get(), &length, sizeof(length)) ||
        !writeExact(pipe.get(), payload.data(), length)) {
        throw std::runtime_error("failed to write runtime control request");
    }

    uint32_t responseLength = 0;
    if (!readExact(pipe.get(), &responseLength, sizeof(responseLength)) ||
        responseLength == 0 ||
        responseLength > vkr::control::kMaxMessageBytes) {
        throw std::runtime_error("invalid runtime control response size");
    }
    std::string responsePayload(responseLength, '\0');
    if (!readExact(pipe.get(), responsePayload.data(), responseLength))
        throw std::runtime_error("failed to read runtime control response");
    return Json::parse(responsePayload);
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

Json waitForLoad(uint64_t taskId) {
    for (;;) {
        Sleep(100);
        const Json request = {
            {"id", 2},
            {"method", "load.status"},
            {"params", {{"taskId", taskId}}}};
        const Json response = sendRequest(request);
        if (!response.value("ok", false))
            return response;
        const Json &result = response.at("result");
        if (result.value("terminal", false))
            return response;
    }
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
        const Json request = {{"id", 1},
                              {"method", command.method},
                              {"params", command.params}};
        Json response = sendRequest(request);

        const bool startsLoad = command.method == "scene.load" ||
                                command.method == "scene.reload" ||
                                command.method == "texture_limit.set" ||
                                command.method == "asset.import";
        if (response.value("ok", false) && command.waitForLoad &&
            startsLoad) {
            const auto taskId = loadTaskId(response.at("result"));
            if (taskId)
                response = waitForLoad(*taskId);
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
