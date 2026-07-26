#include "RenderTestRunner.h"

#include "ImageComparator.h"
#include "ManagedProcessWin32.h"
#include "RenderTestSpec.h"
#include "assets/ContentHash.h"
#include "assets/SceneCatalog.h"
#include "control/RuntimeControlClientWin32.h"
#include "control/RuntimeControlProtocol.h"

#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace vkr::render_test {
namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

constexpr size_t kMaximumRecordedSteps = 512;
constexpr uint32_t kProtocolPollMs = 50;

class RunnerFailure : public std::runtime_error {
  public:
    RunnerFailure(std::string code, std::string message, int exitCode = 1)
        : std::runtime_error(std::move(message)), code_(std::move(code)),
          exitCode_(exitCode) {}

    const std::string &code() const { return code_; }
    int exitCode() const { return exitCode_; }

  private:
    std::string code_;
    int exitCode_ = 1;
};

double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string utcTimestamp(bool fileSafe = false) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc,
                            fileSafe ? "%Y%m%dT%H%M%SZ"
                                     : "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

uint32_t currentProcessId() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return 0;
#endif
}

std::string sanitizeName(std::string value) {
    for (char &character : value) {
        const bool allowed =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_';
        if (!allowed)
            character = '-';
    }
    while (!value.empty() && value.back() == '-')
        value.pop_back();
    if (value.empty())
        value = "render-test";
    if (value.size() > 32)
        value.resize(32);
    return value;
}

std::string uniqueToken(const std::string &prefix) {
    static std::atomic<uint64_t> sequence{0};
    const uint64_t ticks = static_cast<uint64_t>(
        Clock::now().time_since_epoch().count());
    std::ostringstream output;
    output << sanitizeName(prefix) << '_' << currentProcessId() << '_' <<
        std::hex << ticks << '_' << sequence.fetch_add(1);
    std::string result = output.str();
    if (result.size() > control::kMaxPipeSuffixLength)
        result.resize(control::kMaxPipeSuffixLength);
    return result;
}

std::wstring widenUtf8(const std::string &value) {
#ifdef _WIN32
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            value.data(),
                                            static_cast<int>(value.size()),
                                            nullptr, 0);
    if (length <= 0)
        throw RunnerFailure("invalid_utf8",
                            "Could not convert UTF-8 process argument.", 2);
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            length) != length) {
        throw RunnerFailure("invalid_utf8",
                            "Could not convert UTF-8 process argument.", 2);
    }
    return result;
#else
    return std::wstring(value.begin(), value.end());
#endif
}

void atomicPublish(const std::filesystem::path &temporary,
                   const std::filesystem::path &output) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), output.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("atomic publish failed with Win32 error " +
                                 std::to_string(GetLastError()));
    }
#else
    std::filesystem::rename(temporary, output);
#endif
}

void writeJsonAtomic(const std::filesystem::path &path,
                     const Json &document) {
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("could not create JSON output");
        const std::string payload = document.dump(2);
        output.write(payload.data(),
                     static_cast<std::streamsize>(payload.size()));
        output.put('\n');
        output.close();
        if (!output)
            throw std::runtime_error("could not write JSON output");
        atomicPublish(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

Json readJson(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open JSON file: " +
                                 path.u8string());
    Json result;
    input >> result;
    return result;
}

void copyFileAtomic(const std::filesystem::path &source,
                    const std::filesystem::path &destination) {
    if (destination.has_parent_path())
        std::filesystem::create_directories(destination.parent_path());
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    std::error_code error;
    std::filesystem::copy_file(source, temporary,
                               std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error)
        throw std::runtime_error("could not copy file: " + error.message());
    try {
        atomicPublish(temporary, destination);
    } catch (...) {
        std::filesystem::remove(temporary, error);
        throw;
    }
}

Json imageAnalysisToJson(const ImageAnalysis &analysis) {
    return {{"nonBlackRatio", analysis.nonBlackRatio},
            {"nonWhiteRatio", analysis.nonWhiteRatio},
            {"dominantSolidColorRatio",
             analysis.dominantSolidColorRatio},
            {"dominantColor", analysis.dominantColor}};
}

Json comparisonMetricsToJson(const ImageComparisonMetrics &metrics) {
    return {{"dimensionsMatch", metrics.dimensionsMatch},
            {"mae", metrics.mae},
            {"rmse", metrics.rmse},
            {"maximumError", metrics.maximumError},
            {"maePerChannel", metrics.maePerChannel},
            {"rmsePerChannel", metrics.rmsePerChannel},
            {"maximumErrorPerChannel",
             metrics.maximumErrorPerChannel},
            {"badPixelCount", metrics.badPixelCount},
            {"badPixelRatio", metrics.badPixelRatio}};
}

std::optional<uint64_t> operationTaskId(const Json &result) {
    if (result.contains("taskId") && result["taskId"].is_number_unsigned())
        return result["taskId"].get<uint64_t>();
    if (result.contains("loadTask") && result["loadTask"].is_object() &&
        result["loadTask"].contains("taskId") &&
        result["loadTask"]["taskId"].is_number_unsigned()) {
        return result["loadTask"]["taskId"].get<uint64_t>();
    }
    return std::nullopt;
}

bool containsCapability(const Json &info, const std::string &capability) {
    if (!info.contains("capabilities") ||
        !info["capabilities"].is_array())
        return false;
    return std::any_of(
        info["capabilities"].begin(), info["capabilities"].end(),
        [&](const Json &value) {
            return value.is_string() &&
                   value.get<std::string>() == capability;
        });
}

std::string runtimeErrorMessage(const Json &response) {
    if (!response.contains("error") || !response["error"].is_object())
        return "Runtime Control returned an invalid error response.";
    return response["error"].value("message", "Unknown runtime error.");
}

std::string runtimeErrorCode(const Json &response) {
    if (!response.contains("error") || !response["error"].is_object())
        return "protocol_error";
    return response["error"].value("code", "runtime_error");
}

class RunnerSession {
  public:
    RunnerSession(const RenderTestRunOptions &options,
                  std::filesystem::path resultRoot)
        : options_(options), resultRoot_(std::move(resultRoot)),
          captureRoot_(resultRoot_ / "capture"),
          reportPath_(resultRoot_ / "report.json") {
        report_ = {{"schemaVersion", 1},
                   {"status", "running"},
                   {"startedAtUtc", utcTimestamp()},
                   {"resultRoot", resultRoot_.u8string()},
                   {"steps", Json::array()},
                   {"artifacts",
                    {{"report", reportPath_.u8string()},
                     {"rendererLog",
                      (resultRoot_ / "renderer.log").u8string()},
                     {"rendererStdio",
                      (resultRoot_ / "renderer-stdio.log").u8string()}}}};
    }

    RenderTestRunResult run() {
        RenderTestRunResult result;
        result.resultRoot = resultRoot_;
        result.reportPath = reportPath_;
        const Clock::time_point started = Clock::now();
        try {
            spec_ = loadRenderTestSpec(options_.specPath);
            if (options_.accept && spec_->mode != RenderTestMode::Golden) {
                throw RunnerFailure(
                    "accept_requires_golden",
                    "--accept is only valid for a golden-mode spec.", 2);
            }
            report_["spec"] = renderTestSpecToJson(*spec_);
            execute();
            if (outcomeStatus_.empty())
                outcomeStatus_ = "passed";
        } catch (const RunnerFailure &error) {
            rememberFailure(error.code(), error.what(), error.exitCode());
        } catch (const std::invalid_argument &error) {
            rememberFailure("spec_invalid", error.what(), 2);
        } catch (const Json::exception &error) {
            rememberFailure("invalid_json", error.what(), 2);
        } catch (const std::exception &error) {
            rememberFailure("runner_failed", error.what(), 1);
        }

        cleanupRenderer();
        report_["finishedAtUtc"] = utcTimestamp();
        report_["durationMs"] = elapsedMs(started, Clock::now());
        report_["droppedSteps"] = droppedSteps_;
        if (failure_) {
            report_["status"] = "failed";
            report_["error"] = {{"code", failure_->code},
                                 {"message", failure_->message}};
            result.status = "failed";
            result.code = failure_->code;
            result.message = failure_->message;
            result.exitCode = failure_->exitCode;
        } else {
            report_["status"] = outcomeStatus_;
            report_["error"] = nullptr;
            result.status = outcomeStatus_;
            result.code = outcomeCode_;
            result.message = outcomeMessage_;
            result.exitCode = outcomeStatus_ == "skipped" ? 125 : 0;
        }
        try {
            writeJsonAtomic(reportPath_, report_);
        } catch (const std::exception &error) {
            result.status = "failed";
            result.code = "report_write_failed";
            result.message = error.what();
            result.exitCode = 1;
        }
        return result;
    }

  private:
    struct Failure {
        std::string code;
        std::string message;
        int exitCode = 1;
    };

    void rememberFailure(std::string code, std::string message,
                         int exitCode) {
        if (!failure_)
            failure_ = Failure{std::move(code), std::move(message), exitCode};
    }

    void recordStep(const std::string &method, const Json &response,
                    double durationMs) {
        if (report_["steps"].size() >= kMaximumRecordedSteps) {
            ++droppedSteps_;
            return;
        }
        report_["steps"].push_back({{"method", method},
                                    {"durationMs", durationMs},
                                    {"response", response}});
    }

    Json invoke(const std::string &method,
                Json params = Json::object()) {
        if (!client_)
            throw RunnerFailure("protocol_error",
                                "Runtime Control client is unavailable.");
        const Clock::time_point started = Clock::now();
        Json response;
        try {
            response = client_->invoke(nextRequestId_++, method,
                                       std::move(params));
        } catch (const std::exception &error) {
            if (process_) {
                const std::optional<uint32_t> exit =
                    process_->waitForExit(100);
                if (exit) {
                    report_["runtime"]["unexpectedExitCode"] = *exit;
                    throw RunnerFailure(
                        "renderer_crash",
                        "VulkanLab exited unexpectedly with code " +
                            std::to_string(*exit) + ".");
                }
            }
            throw RunnerFailure("protocol_error", error.what());
        }
        recordStep(method, response, elapsedMs(started, Clock::now()));
        if (!response.is_object() || !response.value("ok", false)) {
            throw RunnerFailure(runtimeErrorCode(response),
                                runtimeErrorMessage(response));
        }
        if (!response.contains("result"))
            throw RunnerFailure("protocol_error",
                                "Runtime response has no result field.");
        return response["result"];
    }

    void execute() {
        if (!std::filesystem::is_regular_file(options_.runtimeExecutable)) {
            throw RunnerFailure("renderer_not_found",
                                "VulkanLab executable was not found.", 2);
        }
        const std::filesystem::path runtime =
            std::filesystem::absolute(options_.runtimeExecutable)
                .lexically_normal();
        const std::filesystem::path workingDirectory = runtime.parent_path();
        std::filesystem::create_directories(captureRoot_);
        const std::string suffix = uniqueToken(spec_->name);
        const auto endpoint = control::makeRuntimeControlEndpoint(suffix);
        report_["runtime"] = {{"executable", runtime.u8string()},
                              {"workingDirectory",
                               workingDirectory.u8string()},
                              {"pipe", endpoint.nameUtf8},
                              {"pipeSuffix", suffix}};

        std::ostringstream viewport;
        viewport << spec_->viewport[0] << 'x' << spec_->viewport[1];
        std::ostringstream fixedDelta;
        fixedDelta << std::setprecision(17) << spec_->fixedDelta;
        ManagedProcessOptions processOptions;
        processOptions.executable = runtime;
        processOptions.workingDirectory = workingDirectory;
        processOptions.outputLog = resultRoot_ / "renderer-stdio.log";
        processOptions.arguments = {
            L"--runtime-control", L"--runtime-control-pipe",
            widenUtf8(suffix), L"--automation", L"--window-size",
            widenUtf8(viewport.str()), L"--fixed-delta",
            widenUtf8(fixedDelta.str()), L"--capture-root",
            captureRoot_.wstring()};
        if (!spec_->includeGui)
            processOptions.arguments.push_back(L"--no-gui");
        if (options_.projectRoot) {
            processOptions.arguments.push_back(L"--project");
            processOptions.arguments.push_back(
                std::filesystem::absolute(*options_.projectRoot)
                    .lexically_normal()
                    .wstring());
        }
        processOptions.environmentOverrides = {
            {L"VKR_LOG_FILE", (resultRoot_ / "renderer.log").wstring()},
            {L"VKR_LOG_NO_COLOR", L"1"},
        };
        try {
            process_ =
                std::make_unique<ManagedProcessWin32>(processOptions);
        } catch (const std::exception &error) {
            throw RunnerFailure("renderer_start_failed", error.what());
        }
        report_["runtime"]["processId"] = process_->processId();
        client_ = std::make_unique<RuntimeControlClientWin32>(endpoint, 250);
        waitForStartup();

        Json info = invoke("system.info");
        report_["runtime"]["info"] = info;
        if (info.value("protocolVersion", uint32_t{0}) !=
            control::kProtocolVersion) {
            throw RunnerFailure("protocol_version_mismatch",
                                "Runtime Control protocol version mismatch.");
        }
        if (info.value("pipe", std::string{}) != endpoint.nameUtf8)
            throw RunnerFailure("protocol_endpoint_mismatch",
                                "Renderer reported the wrong pipe endpoint.");
        for (const std::string capability :
             {"camera_control", "render_status", "capture",
              "render_settings"}) {
            if (!containsCapability(info, capability)) {
                throw RunnerFailure(
                    "runtime_capability_missing",
                    "Renderer does not advertise required capability '" +
                        capability + "'.");
            }
        }

        const Json scenes = invoke("scene.list");
        const Json *sceneEntry = nullptr;
        if (scenes.contains("entries") && scenes["entries"].is_array()) {
            for (const Json &entry : scenes["entries"]) {
                if (entry.is_object() &&
                    entry.value("id", std::string{}) == spec_->sceneId) {
                    sceneEntry = &entry;
                    break;
                }
            }
        }
        if (!sceneEntry)
            throw RunnerFailure("scene_not_found",
                                "Spec sceneId is not present in the Catalog.");
        if (sceneEntry->value("profileId", std::string{}) !=
            spec_->profileId) {
            throw RunnerFailure(
                "scene_profile_mismatch",
                "Catalog profile does not match the render test spec.");
        }
        const uint32_t profileTextureLimit =
            sceneEntry->value("textureLimit",
                              std::numeric_limits<uint32_t>::max());
        if (profileTextureLimit != 0 && profileTextureLimit != 512 &&
            profileTextureLimit != 1024 && profileTextureLimit != 2048) {
            throw RunnerFailure(
                "scene_profile_invalid",
                "Runtime did not report a valid texture limit for the "
                "requested Catalog profile.");
        }
        if (!sceneEntry->value("available", false))
            throw RunnerFailure("scene_unavailable",
                                "Spec scene is unavailable in this project.");
        if (info.value("textureLimit", std::numeric_limits<uint32_t>::max()) !=
            profileTextureLimit) {
            Json profileLoad = invoke(
                "texture_limit.set", {{"value", profileTextureLimit}});
            waitForLoad(profileLoad);
            info = invoke("system.info");
            if (info.value("textureLimit",
                           std::numeric_limits<uint32_t>::max()) !=
                profileTextureLimit) {
                throw RunnerFailure(
                    "scene_profile_apply_failed",
                    "Renderer did not apply the requested Catalog profile.");
            }
        }
        report_["runtime"]["requestedProfile"] = {
            {"id", spec_->profileId},
            {"textureLimit", profileTextureLimit}};
        const std::string sceneName =
            sceneEntry->value("name", std::string{});
        Json load = invoke("scene.load", {{"name", sceneName}});
        waitForLoad(load, "sceneLoad");

        Json environmentLoad = invoke(
            "environment.set",
            {{"name", spec_->environmentId
                          ? *spec_->environmentId
                          : std::string("None")}});
        waitForLoad(environmentLoad, "environmentLoad");

        invoke("shader.set", {{"name", spec_->shader}});
        invoke("camera.set",
               {{"position", spec_->camera.position},
                {"yaw", spec_->camera.yaw},
                {"pitch", spec_->camera.pitch}});
        invoke("render_settings.set",
               {{"shadowsEnabled",
                 spec_->renderSettings.shadowsEnabled},
                {"shadowReceiverBias",
                 spec_->renderSettings.shadowReceiverBias},
                {"shadowConstantBias",
                 spec_->renderSettings.shadowConstantBias},
                {"shadowSlopeBias",
                 spec_->renderSettings.shadowSlopeBias},
                {"exposureEv", spec_->renderSettings.exposureEv},
                {"toneMapper",
                 toneMapperName(spec_->renderSettings.toneMapper)},
                {"iblEnabled", spec_->renderSettings.iblEnabled},
                {"skyboxEnabled", spec_->renderSettings.skyboxEnabled},
                {"environmentIntensity",
                 spec_->renderSettings.environmentIntensity},
                {"environmentRotationRadians",
                 spec_->renderSettings.environmentRotationRadians}});
        report_["runtime"]["environment"] =
            invoke("environment.current");
        info = invoke("system.info");
        report_["runtime"]["infoAfterSetup"] = info;
        if (info.value("shader", std::string{}) != spec_->shader)
            throw RunnerFailure("shader_state_mismatch",
                                "Renderer did not retain the requested shader.");
        waitForStableRender();

        try {
            report_["loadStats"] = invoke("stats.last_load");
        } catch (const RunnerFailure &error) {
            report_["loadStatsError"] =
                {{"code", error.code()}, {"message", error.what()}};
        }

        Json capture = invoke(
            "capture.screenshot",
            {{"path", "raw/actual.png"},
             {"includeGui", spec_->includeGui}});
        capture = waitForCapture(capture);
        processCapture(capture, info);
    }

    void waitForStartup() {
        const Clock::time_point deadline =
            Clock::now() +
            std::chrono::milliseconds(options_.startupTimeoutMs);
        uint64_t attempts = 0;
        std::string lastError;
        while (Clock::now() < deadline) {
            ++attempts;
            if (const auto exit = process_->waitForExit(0)) {
                report_["runtime"]["startupAttempts"] = attempts;
                report_["runtime"]["earlyExitCode"] = *exit;
                throw RunnerFailure(
                    "renderer_start_failed",
                    "VulkanLab exited before Runtime Control became ready.");
            }
            try {
                const Json response =
                    client_->invoke(nextRequestId_++, "system.ping");
                if (response.value("ok", false)) {
                    recordStep("system.ping", response, 0.0);
                    report_["runtime"]["startupAttempts"] = attempts;
                    return;
                }
                lastError = runtimeErrorMessage(response);
            } catch (const std::exception &error) {
                lastError = error.what();
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100));
        }
        report_["runtime"]["startupAttempts"] = attempts;
        report_["runtime"]["lastStartupError"] = lastError;
        throw RunnerFailure("renderer_start_timeout",
                            "Timed out waiting for Runtime Control startup.");
    }

    void waitForLoad(Json status,
                     const char *reportField = "sceneLoad") {
        const std::optional<uint64_t> taskId = operationTaskId(status);
        if (!taskId)
            return;
        const Clock::time_point deadline =
            Clock::now() +
            std::chrono::milliseconds(options_.operationTimeoutMs);
        for (;;) {
            if (status.value("terminal", false)) {
                const std::string state =
                    status.value("state", std::string{});
                if (state == "Failed" || state == "Cancelled") {
                    throw RunnerFailure(
                        "load_failed",
                        "Scene load ended in state " + state + ": " +
                            status.value("error", std::string{}));
                }
                report_[reportField] = status;
                return;
            }
            if (Clock::now() >= deadline)
                throw RunnerFailure("load_timeout",
                                    "Timed out waiting for scene load.");
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100));
            status = invoke("load.status", {{"taskId", *taskId}});
        }
    }

    void waitForStableRender() {
        const Clock::time_point deadline =
            Clock::now() +
            std::chrono::milliseconds(options_.renderTimeoutMs);
        std::optional<uint64_t> generation;
        uint64_t lastPresented = 0;
        uint64_t stableFrames = 0;
        bool sawMinimized = false;
        while (Clock::now() < deadline) {
            const Json status = invoke("render.status");
            const Json scene = status.value("scene", Json(nullptr));
            const bool sceneMatches =
                scene.is_object() &&
                scene.value("id", std::string{}) == spec_->sceneId;
            const uint64_t currentGeneration =
                status.value("sceneGeneration", uint64_t{0});
            const uint64_t presented =
                status.value("presentedFrames", uint64_t{0});
            const bool minimized = status.value("minimized", false);
            sawMinimized = sawMinimized || minimized;
            const Json load = status.value("loadTask", Json(nullptr));
            if (load.is_object() && load.value("terminal", false)) {
                const std::string state =
                    load.value("state", std::string{});
                if (state == "Failed" || state == "Cancelled")
                    throw RunnerFailure("load_failed",
                                        "Scene load failed before rendering.");
            }
            const bool loadReady =
                load.is_null() || load.value("terminal", false);
            const bool ready =
                sceneMatches && status.value("rendering", false) &&
                loadReady &&
                status.value("pendingUpload", uint64_t{0}) == 0 &&
                !minimized &&
                !status.value("swapchainRecreatePending", false);
            bool environmentMatches = true;
            if (spec_->environmentId) {
                const Json environment =
                    status.value("environment", Json::object());
                environmentMatches =
                    environment.value("ready", false) &&
                    environment.value("publishedId", std::string{}) ==
                        *spec_->environmentId;
            }
            const bool fullyReady = ready && environmentMatches;
            if (!generation || *generation != currentGeneration ||
                presented < lastPresented || !fullyReady) {
                stableFrames = 0;
            } else {
                stableFrames += presented - lastPresented;
            }
            generation = currentGeneration;
            lastPresented = presented;
            if (fullyReady && stableFrames >= spec_->stableFrames) {
                report_["renderReady"] = status;
                report_["renderReady"]["stableFrames"] = stableFrames;
                report_["renderReady"]["stableFrameTarget"] =
                    spec_->stableFrames;
                return;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kProtocolPollMs));
        }
        throw RunnerFailure(
            sawMinimized ? "window_not_rendering" : "render_wait_timeout",
            sawMinimized
                ? "The renderer window was minimized and stopped presenting."
                : "Timed out waiting for stable rendered frames.");
    }

    Json waitForCapture(Json status) {
        if (!status.contains("taskId") ||
            !status["taskId"].is_number_unsigned()) {
            throw RunnerFailure("capture_failed",
                                "Capture request returned no task ID.");
        }
        const uint64_t taskId = status["taskId"].get<uint64_t>();
        const Clock::time_point deadline =
            Clock::now() +
            std::chrono::milliseconds(options_.captureTimeoutMs);
        for (;;) {
            if (status.value("terminal", false)) {
                const std::string state =
                    status.value("state", std::string{});
                if (state != "Completed") {
                    std::string message = "Capture ended in state " + state;
                    if (status.contains("error") &&
                        status["error"].is_string())
                        message += ": " + status["error"].get<std::string>();
                    throw RunnerFailure("capture_failed", message);
                }
                return status;
            }
            if (Clock::now() >= deadline)
                throw RunnerFailure("capture_timeout",
                                    "Timed out waiting for screenshot.");
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kProtocolPollMs));
            status = invoke("capture.status", {{"taskId", taskId}});
        }
    }

    void processCapture(const Json &capture, const Json &runtimeInfo) {
        if (!capture.contains("result") || !capture["result"].is_object())
            throw RunnerFailure("capture_failed",
                                "Completed capture has no result object.");
        const Json &captureResult = capture["result"];
        const std::filesystem::path capturePath =
            std::filesystem::u8path(
                captureResult.value("outputPath", std::string{}));
        if (!pathIsWithin(captureRoot_, capturePath) ||
            !std::filesystem::is_regular_file(capturePath)) {
            throw RunnerFailure(
                "capture_path_invalid",
                "Renderer returned a capture outside the isolated root.");
        }
        const std::string captureHash = sha256File(capturePath);
        if (captureHash !=
            captureResult.value("sha256", std::string{})) {
            throw RunnerFailure("capture_hash_mismatch",
                                "Capture file SHA-256 does not match response.");
        }
        const std::filesystem::path actualPath = resultRoot_ / "actual.png";
        copyFileAtomic(capturePath, actualPath);
        report_["artifacts"]["actual"] = actualPath.u8string();
        report_["capture"] = capture;

        const RgbaImage actual = loadRgbaPng(actualPath);
        if (actual.width != spec_->viewport[0] ||
            actual.height != spec_->viewport[1]) {
            report_["image"] = {{"width", actual.width},
                                {"height", actual.height},
                                {"sha256", captureHash}};
            throw RunnerFailure(
                "capture_extent_mismatch",
                "Capture dimensions do not match the requested viewport.");
        }
        const SmokeEvaluation smoke =
            evaluateSmoke(actual, spec_->smokeThresholds);
        report_["image"] = {{"width", actual.width},
                            {"height", actual.height},
                            {"sha256", captureHash},
                            {"smoke",
                             {{"passed", smoke.passed},
                              {"analysis",
                               imageAnalysisToJson(smoke.analysis)},
                              {"failures", smoke.failures}}}};
        if (!smoke.passed)
            throw RunnerFailure("smoke_compare_failed",
                                "Captured image failed smoke invariants.");

        if (spec_->mode == RenderTestMode::Golden)
            processGolden(actual, captureHash, runtimeInfo);
    }

    Json makeBaselineMetadata(const std::string &imageHash,
                              const Json &runtimeInfo) const {
        if (!runtimeInfo.contains("gpu") ||
            !runtimeInfo["gpu"].is_object() ||
            !runtimeInfo.contains("build") ||
            !runtimeInfo["build"].is_object() ||
            !runtimeInfo.contains("shaderInfo") ||
            !runtimeInfo["shaderInfo"].is_object()) {
            throw RunnerFailure(
                "runtime_identity_missing",
                "Runtime did not report GPU/build/shader identity.");
        }
        return {
            {"schemaVersion", 1},
            {"acceptedAtUtc", utcTimestamp()},
            {"spec",
             {{"name", spec_->name},
              {"sceneId", spec_->sceneId},
              {"profileId", spec_->profileId},
              {"shader", spec_->shader},
              {"viewport", spec_->viewport},
              {"fixedDelta", spec_->fixedDelta},
              {"includeGui", spec_->includeGui}}},
            {"gpu", runtimeInfo["gpu"]},
            {"build", runtimeInfo["build"]},
            {"shaderInfo", runtimeInfo["shaderInfo"]},
            {"image",
             {{"sha256", imageHash},
              {"width", spec_->viewport[0]},
              {"height", spec_->viewport[1]}}},
        };
    }

    void validateBaselineMetadata(const Json &metadata,
                                  const std::string &baselineHash) const {
        try {
            if (!metadata.is_object() ||
                metadata.at("schemaVersion").get<uint32_t>() != 1)
                throw std::runtime_error("unsupported metadata schema");
            const Json &identity = metadata.at("spec");
            if (identity.at("name").get<std::string>() != spec_->name ||
                identity.at("sceneId").get<std::string>() !=
                    spec_->sceneId ||
                identity.at("profileId").get<std::string>() !=
                    spec_->profileId ||
                identity.at("shader").get<std::string>() != spec_->shader ||
                identity.at("viewport") != Json(spec_->viewport) ||
                identity.at("fixedDelta").get<double>() !=
                    spec_->fixedDelta ||
                identity.at("includeGui").get<bool>() != spec_->includeGui) {
                throw std::runtime_error("baseline spec identity mismatch");
            }
            const Json &image = metadata.at("image");
            if (image.at("sha256").get<std::string>() != baselineHash ||
                image.at("width").get<uint32_t>() != spec_->viewport[0] ||
                image.at("height").get<uint32_t>() != spec_->viewport[1])
                throw std::runtime_error("baseline image hash mismatch");
            (void)metadata.at("gpu").at("vendorId").get<uint32_t>();
            (void)metadata.at("gpu").at("deviceId").get<uint32_t>();
        } catch (const std::exception &error) {
            throw RunnerFailure("baseline_invalid",
                                std::string("Invalid baseline metadata: ") +
                                    error.what());
        }
    }

    void processGolden(const RgbaImage &actual,
                       const std::string &actualHash,
                       const Json &runtimeInfo) {
        const GoldenSpec &golden = *spec_->golden;
        if (options_.accept) {
            if (std::filesystem::is_regular_file(golden.baselineImage)) {
                const std::filesystem::path previous =
                    resultRoot_ / "previous.png";
                copyFileAtomic(golden.baselineImage, previous);
                report_["artifacts"]["previous"] = previous.u8string();
                try {
                    const RgbaImage old = loadRgbaPng(previous);
                    const GoldenEvaluation comparison = compareImages(
                        actual, old, golden.thresholds);
                    report_["acceptComparison"] = {
                        {"passed", comparison.passed},
                        {"metrics",
                         comparisonMetricsToJson(comparison.metrics)},
                        {"failures", comparison.failures}};
                    if (comparison.metrics.dimensionsMatch) {
                        const std::filesystem::path diff =
                            resultRoot_ / "accept-diff.png";
                        writeRgbaPngAtomic(diff, comparison.diff);
                        report_["artifacts"]["acceptDiff"] = diff.u8string();
                    }
                } catch (const std::exception &error) {
                    report_["acceptComparisonError"] = error.what();
                }
            }
            copyFileAtomic(resultRoot_ / "actual.png",
                           golden.baselineImage);
            const Json metadata =
                makeBaselineMetadata(actualHash, runtimeInfo);
            writeJsonAtomic(golden.baselineMetadata, metadata);
            report_["golden"] = {
                {"status", "accepted"},
                {"baselineImage", golden.baselineImage.u8string()},
                {"baselineMetadata", golden.baselineMetadata.u8string()}};
            outcomeStatus_ = "accepted";
            outcomeCode_ = "baseline_accepted";
            outcomeMessage_ = "Golden baseline was explicitly accepted.";
            return;
        }

        if (!std::filesystem::is_regular_file(golden.baselineImage) ||
            !std::filesystem::is_regular_file(golden.baselineMetadata)) {
            report_["golden"] = {
                {"status", "missing"},
                {"baselineImage", golden.baselineImage.u8string()},
                {"baselineMetadata", golden.baselineMetadata.u8string()}};
            throw RunnerFailure(
                "baseline_missing",
                "Golden baseline is missing; review actual.png before using --accept.");
        }
        const std::string baselineHash = sha256File(golden.baselineImage);
        Json metadata;
        try {
            metadata = readJson(golden.baselineMetadata);
        } catch (const std::exception &error) {
            throw RunnerFailure("baseline_invalid", error.what());
        }
        validateBaselineMetadata(metadata, baselineHash);
        if (!runtimeInfo.contains("gpu") ||
            !runtimeInfo["gpu"].is_object()) {
            throw RunnerFailure("runtime_identity_missing",
                                "Runtime did not report GPU identity.");
        }
        const Json &referenceGpu = metadata["gpu"];
        const Json &currentGpu = runtimeInfo["gpu"];
        const bool sameGpuFamily =
            referenceGpu.value("vendorId", uint32_t{0}) ==
                currentGpu.value("vendorId", uint32_t{0}) &&
            referenceGpu.value("deviceId", uint32_t{0}) ==
                currentGpu.value("deviceId", uint32_t{0});
        if (!sameGpuFamily) {
            report_["golden"] = {
                {"status", "skipped"},
                {"reason", "reference_gpu_mismatch"},
                {"referenceGpu", referenceGpu},
                {"currentGpu", currentGpu}};
            outcomeStatus_ = "skipped";
            outcomeCode_ = "reference_gpu_mismatch";
            outcomeMessage_ =
                "Smoke passed; golden comparison skipped on another GPU family.";
            return;
        }

        const RgbaImage baseline = loadRgbaPng(golden.baselineImage);
        const GoldenEvaluation comparison =
            compareImages(actual, baseline, golden.thresholds);
        report_["golden"] = {
            {"status", comparison.passed ? "passed" : "failed"},
            {"baselineImage", golden.baselineImage.u8string()},
            {"baselineMetadata", golden.baselineMetadata.u8string()},
            {"referenceGpu", referenceGpu},
            {"currentGpu", currentGpu},
            {"driverChanged",
             referenceGpu.value("driverVersion", uint32_t{0}) !=
                 currentGpu.value("driverVersion", uint32_t{0})},
            {"shaderChanged",
             metadata.value("shaderInfo", Json::object()) !=
                 runtimeInfo.value("shaderInfo", Json::object())},
            {"metrics", comparisonMetricsToJson(comparison.metrics)},
            {"failures", comparison.failures}};
        if (!comparison.passed) {
            if (comparison.metrics.dimensionsMatch) {
                const std::filesystem::path diff =
                    resultRoot_ / "diff.png";
                writeRgbaPngAtomic(diff, comparison.diff);
                report_["artifacts"]["diff"] = diff.u8string();
            }
            throw RunnerFailure("golden_compare_failed",
                                "Captured image differs from the golden baseline.");
        }
    }

    void cleanupRenderer() {
        if (!process_)
            return;
        const bool runningBeforeCleanup = process_->running();
        Json cleanup = {{"quitRequested", false},
                        {"forcedTermination", false}};
        if (runningBeforeCleanup && client_) {
            try {
                const Clock::time_point started = Clock::now();
                const Json response =
                    client_->invoke(nextRequestId_++, "app.quit");
                recordStep("app.quit", response,
                           elapsedMs(started, Clock::now()));
                cleanup["quitRequested"] = true;
                cleanup["quitResponse"] = response;
                if (!response.value("ok", false)) {
                    throw std::runtime_error(runtimeErrorMessage(response));
                }
            } catch (const std::exception &error) {
                cleanup["quitError"] = error.what();
                if (!failure_)
                    rememberFailure("quit_failed", error.what(), 1);
            }
        }

        std::optional<uint32_t> exit;
        try {
            exit = process_->waitForExit(options_.quitTimeoutMs);
            if (!exit && process_->running()) {
                process_->terminate(1);
                cleanup["forcedTermination"] = true;
                exit = process_->waitForExit(1000);
                if (!failure_)
                    rememberFailure("quit_timeout",
                                    "Renderer did not exit after app.quit.", 1);
            }
        } catch (const std::exception &error) {
            cleanup["cleanupError"] = error.what();
            if (!failure_)
                rememberFailure("renderer_cleanup_failed", error.what(), 1);
        }
        if (exit)
            cleanup["exitCode"] = *exit;
        if (!failure_ && exit && !runningBeforeCleanup)
            rememberFailure("renderer_crash",
                            "Renderer exited before cleanup could request quit.",
                            1);
        else if (!failure_ && exit && *exit != 0)
            rememberFailure("renderer_crash",
                            "Renderer exited with a non-zero code.", 1);
        report_["cleanup"] = std::move(cleanup);
    }

    const RenderTestRunOptions &options_;
    std::filesystem::path resultRoot_;
    std::filesystem::path captureRoot_;
    std::filesystem::path reportPath_;
    std::optional<RenderTestSpec> spec_;
    Json report_;
    std::unique_ptr<ManagedProcessWin32> process_;
    std::unique_ptr<RuntimeControlClientWin32> client_;
    uint64_t nextRequestId_ = 1;
    uint64_t droppedSteps_ = 0;
    std::optional<Failure> failure_;
    std::string outcomeStatus_;
    std::string outcomeCode_;
    std::string outcomeMessage_;
};

} // namespace

RenderTestRunResult runRenderTest(const RenderTestRunOptions &options) {
    if (options.specPath.empty())
        throw std::invalid_argument("render test spec path is required");
    if (options.outputRoot.empty())
        throw std::invalid_argument("render test output root is required");
    if (options.startupTimeoutMs == 0 || options.operationTimeoutMs == 0 ||
        options.renderTimeoutMs == 0 || options.captureTimeoutMs == 0 ||
        options.quitTimeoutMs == 0) {
        throw std::invalid_argument("render test timeouts must be non-zero");
    }
    const std::filesystem::path outputRoot =
        std::filesystem::absolute(options.outputRoot).lexically_normal();
    std::filesystem::create_directories(outputRoot);
    const std::string baseName = sanitizeName(
        options.specPath.stem().u8string());
    const std::filesystem::path resultRoot =
        outputRoot /
        (baseName + "-" + utcTimestamp(true) + "-" +
         std::to_string(currentProcessId()) + "-" +
         uniqueToken("run"));
    std::filesystem::create_directories(resultRoot);
    return RunnerSession(options, resultRoot).run();
}

} // namespace vkr::render_test
