#include "GltfValidator.h"

#include "assets/ContentHash.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

namespace vkr::assettool {
namespace {

using Json = nlohmann::json;

std::filesystem::path currentExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return std::filesystem::current_path();
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path findOnPath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = SearchPathW(
        nullptr, L"gltf_validator.exe", nullptr,
        static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

bool containsExpectedVersion(const std::string &output) {
    return output.find(std::string("version ") + kGltfValidatorVersion) !=
           std::string::npos;
}

std::string extensionNote(const std::string &extension,
                          GltfExtensionSupport support) {
    if (extension == "KHR_materials_transmission")
        return "Factor-only approximate transmission; texture and physical "
               "refraction are not supported.";
    if (extension == "KHR_materials_volume")
        return "Volume fields are parsed but are not used by shading.";
    if (support == GltfExtensionSupport::Unsupported)
        return "The renderer ignores this extension.";
    return {};
}

std::vector<GltfExtensionDiagnostic>
extensionDiagnostics(const ModelImportPreflight &preflight) {
    std::set<std::string> required(preflight.extensionsRequired.begin(),
                                   preflight.extensionsRequired.end());
    std::set<std::string> all(preflight.extensionsUsed.begin(),
                              preflight.extensionsUsed.end());
    all.insert(required.begin(), required.end());
    std::vector<GltfExtensionDiagnostic> diagnostics;
    diagnostics.reserve(all.size());
    for (const std::string &name : all) {
        const GltfExtensionSupport support = gltfExtensionSupport(name);
        diagnostics.push_back(
            {name, support, required.count(name) != 0,
             extensionNote(name, support)});
    }
    return diagnostics;
}

DerivedFileStamp sourceStamp(const std::filesystem::path &source,
                             const std::string &sha256) {
    DerivedFileStamp stamp = fileStamp(source, sha256);
    stamp.path = source.filename().generic_string();
    return stamp;
}

std::vector<DerivedFileStamp>
dependencyStamps(const ModelImportPreflight &preflight) {
    std::vector<DerivedFileStamp> stamps;
    stamps.reserve(preflight.dependencies.size());
    for (const auto &dependency : preflight.dependencies) {
        DerivedFileStamp stamp = fileStamp(dependency.sourcePath);
        stamp.path = dependency.relativePath.generic_string();
        stamps.push_back(std::move(stamp));
    }
    return stamps;
}

void finalizeIdentity(AssetValidationReport &report) {
    report.inputFingerprint = assetValidationInputFingerprint(
        report.sourceSha256, report.source, report.dependencies);
    report.reportKey = report.validatorVersion + "/" +
                       report.inputFingerprint + ".json";
}

AssetValidationReport baseReport(const ModelImportPreflight &preflight) {
    AssetValidationReport report;
    report.validatorVersion = kGltfValidatorVersion;
    report.sourceSha256 = sha256File(preflight.sourcePath);
    report.source = sourceStamp(preflight.sourcePath, report.sourceSha256);
    report.dependencies = dependencyStamps(preflight);
    report.extensions = extensionDiagnostics(preflight);
    finalizeIdentity(report);
    return report;
}

void parseValidatorReport(const Json &root, AssetValidationReport &report) {
    const std::string version =
        root.value("validatorVersion", std::string{});
    if (version != kGltfValidatorVersion) {
        throw std::runtime_error(
            "Validator report version mismatch: expected " +
            std::string(kGltfValidatorVersion) + ", got " + version);
    }
    if (!root.contains("issues") || !root.at("issues").is_object())
        throw std::runtime_error("Validator report has no issues object");
    const Json &issues = root.at("issues");
    report.errorCount = issues.value("numErrors", uint64_t{0});
    report.warningCount = issues.value("numWarnings", uint64_t{0});
    report.infoCount = issues.value("numInfos", uint64_t{0});
    report.hintCount = issues.value("numHints", uint64_t{0});
    report.truncated = issues.value("truncated", false);
    if (issues.contains("messages") && issues.at("messages").is_array()) {
        for (const Json &message : issues.at("messages")) {
            report.issues.push_back(
                {message.value("code", std::string{}),
                 message.value("message", std::string{}),
                 message.value("pointer", std::string{}),
                 message.value("severity", 0u)});
        }
    }
    if (root.contains("info") && root.at("info").is_object()) {
        const Json &info = root.at("info");
        report.statistics.assetVersion =
            info.value("version", std::string{});
        report.statistics.generator =
            info.value("generator", std::string{});
        report.statistics.animationCount =
            info.value("animationCount", uint64_t{0});
        report.statistics.materialCount =
            info.value("materialCount", uint64_t{0});
        report.statistics.drawCallCount =
            info.value("drawCallCount", uint64_t{0});
        report.statistics.totalVertexCount =
            info.value("totalVertexCount", uint64_t{0});
        report.statistics.totalTriangleCount =
            info.value("totalTriangleCount", uint64_t{0});
        report.statistics.maxUvs = info.value("maxUVs", uint64_t{0});
        report.statistics.maxInfluences =
            info.value("maxInfluences", uint64_t{0});
        report.statistics.maxAttributes =
            info.value("maxAttributes", uint64_t{0});
        report.statistics.hasMorphTargets =
            info.value("hasMorphTargets", false);
        report.statistics.hasSkins = info.value("hasSkins", false);
        report.statistics.hasTextures = info.value("hasTextures", false);
        report.statistics.hasDefaultScene =
            info.value("hasDefaultScene", false);
    }
    report.state = report.errorCount != 0
                       ? AssetValidationState::Invalid
                       : (report.warningCount != 0
                              ? AssetValidationState::Warnings
                              : AssetValidationState::Valid);
}

void saveReportOrThrow(const std::filesystem::path &cacheRoot,
                       AssetValidationReport &report) {
    std::string error;
    if (!saveAssetValidationReport(cacheRoot, report, error))
        throw std::runtime_error("Could not save validation report: " +
                                 error);
}

} // namespace

std::filesystem::path locateGltfValidator(
    const std::filesystem::path &explicitPath,
    IProcessRunner &processRunner, std::string &reason) {
    std::vector<std::filesystem::path> candidates;
    if (!explicitPath.empty()) {
        candidates.push_back(
            std::filesystem::absolute(explicitPath).lexically_normal());
    } else {
        candidates.push_back(currentExecutableDirectory() /
                             "gltf_validator.exe");
        const std::filesystem::path onPath = findOnPath();
        if (!onPath.empty())
            candidates.push_back(onPath);
    }

    std::set<std::filesystem::path> checked;
    for (const auto &candidate : candidates) {
        const std::filesystem::path normalized =
            candidate.lexically_normal();
        if (!checked.insert(normalized).second ||
            !std::filesystem::is_regular_file(normalized))
            continue;
        std::atomic_bool cancelRequested{false};
        ProcessRequest probe;
        probe.executable = normalized;
        probe.arguments = {L"--help"};
        probe.timeoutMs = 10000;
        probe.maxStdoutBytes = 64 * 1024;
        probe.maxStderrBytes = 256 * 1024;
        const ProcessResult result =
            processRunner.run(probe, cancelRequested);
        if (!result.timedOut && containsExpectedVersion(result.output)) {
            reason.clear();
            return normalized;
        }
        reason = std::string("glTF Validator version mismatch; expected ") +
                 kGltfValidatorVersion;
        if (!explicitPath.empty())
            return {};
    }
    if (reason.empty()) {
        reason = !explicitPath.empty()
                     ? "glTF Validator was not found at the explicit "
                       "--gltf-validator path"
                     : "glTF Validator " +
                           std::string(kGltfValidatorVersion) +
                           " was not found beside VulkanLabAssetTool.exe or "
                           "on PATH";
    }
    return {};
}

GltfValidationResult validateGltf(
    const GltfValidationOptions &options,
    const std::atomic_bool &cancelRequested,
    IProcessRunner &processRunner) {
    GltfValidationResult result;
    result.preflight = ModelImportService::preflight(options.sourcePath);
    result.report = baseReport(result.preflight);
    result.reportPath = assetValidationReportPath(
        options.cacheRoot, result.report.validatorVersion,
        result.report.inputFingerprint);

    if (!options.force &&
        std::filesystem::is_regular_file(result.reportPath)) {
        AssetValidationReport cached;
        std::string error;
        if (loadAssetValidationReport(result.reportPath, cached, error) &&
            (cached.state == AssetValidationState::Valid ||
             cached.state == AssetValidationState::Warnings ||
             cached.state == AssetValidationState::Invalid) &&
            validationReportMatchesSource(cached,
                                          result.preflight.sourcePath,
                                          error)) {
            if (!options.requireExecutable) {
                result.report = std::move(cached);
                result.reused = true;
                return result;
            }
        }
    }

    if (cancelRequested.load())
        throw std::runtime_error("glTF validation cancelled");

    std::string locatorReason;
    const std::filesystem::path validator = locateGltfValidator(
        options.validatorPath, processRunner, locatorReason);
    if (validator.empty()) {
        result.report.state = AssetValidationState::Unavailable;
        result.report.failureReason = locatorReason;
        saveReportOrThrow(options.cacheRoot, result.report);
        result.reportPath = assetValidationReportPath(
            options.cacheRoot, result.report.validatorVersion,
            result.report.inputFingerprint);
        return result;
    }

    if (!options.force && options.requireExecutable &&
        std::filesystem::is_regular_file(result.reportPath)) {
        AssetValidationReport cached;
        std::string error;
        if (loadAssetValidationReport(result.reportPath, cached, error) &&
            (cached.state == AssetValidationState::Valid ||
             cached.state == AssetValidationState::Warnings ||
             cached.state == AssetValidationState::Invalid) &&
            validationReportMatchesSource(cached,
                                          result.preflight.sourcePath,
                                          error)) {
            result.report = std::move(cached);
            result.reused = true;
            return result;
        }
    }

    ProcessRequest request;
    request.executable = validator;
    request.arguments = {L"--stdout", L"--validate-resources",
                         L"--no-write-timestamp", L"--no-absolute-path",
                         L"--threads", L"1",
                         result.preflight.sourcePath.wstring()};
    request.timeoutMs = 300000;
    request.maxStdoutBytes = 16 * 1024 * 1024;
    request.maxStderrBytes = 1024 * 1024;
    const ProcessResult process =
        processRunner.run(request, cancelRequested);
    if (process.cancelled)
        throw std::runtime_error("glTF validation cancelled");
    if (process.timedOut) {
        result.report.state = AssetValidationState::Failed;
        result.report.failureReason = "glTF Validator timed out after 300s";
    } else if (process.stdoutTruncated) {
        result.report.state = AssetValidationState::Failed;
        result.report.failureReason =
            "glTF Validator report exceeded 16 MiB";
    } else if (process.stderrTruncated) {
        result.report.state = AssetValidationState::Failed;
        result.report.failureReason =
            "glTF Validator diagnostics exceeded 1 MiB";
    } else {
        try {
            const Json root = Json::parse(process.stdoutText);
            parseValidatorReport(root, result.report);
            if (process.exitCode != 0 && result.report.errorCount == 0) {
                result.report.state = AssetValidationState::Failed;
                result.report.failureReason =
                    "glTF Validator exited with code " +
                    std::to_string(process.exitCode);
            }
        } catch (const std::exception &exception) {
            result.report.state = AssetValidationState::Failed;
            result.report.failureReason =
                std::string("Could not parse glTF Validator report: ") +
                exception.what();
        }
    }
    saveReportOrThrow(options.cacheRoot, result.report);
    result.reportPath = assetValidationReportPath(
        options.cacheRoot, result.report.validatorVersion,
        result.report.inputFingerprint);
    return result;
}

} // namespace vkr::assettool
