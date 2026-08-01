#include "AssetValidation.h"

#include "CacheMutationLock.h"
#include "ContentHash.h"
#include "SceneCatalog.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <stdexcept>

namespace vkr {
namespace {

using Json = nlohmann::json;

constexpr const char *kValidationIndexName = "validation/index.json";

int64_t unixTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Json stampToJson(const DerivedFileStamp &stamp) {
    return {{"path", stamp.path},
            {"size", stamp.size},
            {"writeTime", stamp.writeTime},
            {"sha256", stamp.sha256}};
}

DerivedFileStamp stampFromJson(const Json &json) {
    DerivedFileStamp stamp;
    stamp.path = json.value("path", std::string{});
    stamp.size = json.value("size", uint64_t{0});
    stamp.writeTime = json.value("writeTime", int64_t{0});
    stamp.sha256 = json.value("sha256", std::string{});
    return stamp;
}

Json issueToJson(const AssetValidationIssue &issue) {
    return {{"code", issue.code},
            {"message", issue.message},
            {"pointer", issue.pointer},
            {"severity", issue.severity}};
}

AssetValidationIssue issueFromJson(const Json &json) {
    AssetValidationIssue issue;
    issue.code = json.value("code", std::string{});
    issue.message = json.value("message", std::string{});
    issue.pointer = json.value("pointer", std::string{});
    issue.severity = json.value("severity", 0u);
    return issue;
}

Json extensionToJson(const GltfExtensionDiagnostic &extension) {
    return {{"name", extension.name},
            {"support", gltfExtensionSupportName(extension.support)},
            {"required", extension.required},
            {"note", extension.note}};
}

GltfExtensionDiagnostic extensionFromJson(const Json &json) {
    GltfExtensionDiagnostic extension;
    extension.name = json.value("name", std::string{});
    const std::string support =
        json.value("support", std::string("Unsupported"));
    if (support == "Supported")
        extension.support = GltfExtensionSupport::Supported;
    else if (support == "Partial")
        extension.support = GltfExtensionSupport::Partial;
    else
        extension.support = GltfExtensionSupport::Unsupported;
    extension.required = json.value("required", false);
    extension.note = json.value("note", std::string{});
    return extension;
}

Json statisticsToJson(const AssetValidationStatistics &statistics) {
    return {{"assetVersion", statistics.assetVersion},
            {"generator", statistics.generator},
            {"animationCount", statistics.animationCount},
            {"materialCount", statistics.materialCount},
            {"drawCallCount", statistics.drawCallCount},
            {"totalVertexCount", statistics.totalVertexCount},
            {"totalTriangleCount", statistics.totalTriangleCount},
            {"maxUvs", statistics.maxUvs},
            {"maxInfluences", statistics.maxInfluences},
            {"maxAttributes", statistics.maxAttributes},
            {"hasMorphTargets", statistics.hasMorphTargets},
            {"hasSkins", statistics.hasSkins},
            {"hasTextures", statistics.hasTextures},
            {"hasDefaultScene", statistics.hasDefaultScene}};
}

AssetValidationStatistics statisticsFromJson(const Json &json) {
    AssetValidationStatistics statistics;
    statistics.assetVersion = json.value("assetVersion", std::string{});
    statistics.generator = json.value("generator", std::string{});
    statistics.animationCount = json.value("animationCount", uint64_t{0});
    statistics.materialCount = json.value("materialCount", uint64_t{0});
    statistics.drawCallCount = json.value("drawCallCount", uint64_t{0});
    statistics.totalVertexCount =
        json.value("totalVertexCount", uint64_t{0});
    statistics.totalTriangleCount =
        json.value("totalTriangleCount", uint64_t{0});
    statistics.maxUvs = json.value("maxUvs", uint64_t{0});
    statistics.maxInfluences = json.value("maxInfluences", uint64_t{0});
    statistics.maxAttributes = json.value("maxAttributes", uint64_t{0});
    statistics.hasMorphTargets = json.value("hasMorphTargets", false);
    statistics.hasSkins = json.value("hasSkins", false);
    statistics.hasTextures = json.value("hasTextures", false);
    statistics.hasDefaultScene = json.value("hasDefaultScene", false);
    return statistics;
}

Json reportToJson(const AssetValidationReport &report) {
    Json dependencies = Json::array();
    for (const auto &dependency : report.dependencies)
        dependencies.push_back(stampToJson(dependency));
    Json issues = Json::array();
    for (const auto &issue : report.issues)
        issues.push_back(issueToJson(issue));
    Json extensions = Json::array();
    for (const auto &extension : report.extensions)
        extensions.push_back(extensionToJson(extension));
    return {{"schemaVersion", report.schemaVersion},
            {"state", assetValidationStateName(report.state)},
            {"validatorName", report.validatorName},
            {"validatorVersion", report.validatorVersion},
            {"reportKey", report.reportKey},
            {"inputFingerprint", report.inputFingerprint},
            {"sourceSha256", report.sourceSha256},
            {"source", stampToJson(report.source)},
            {"dependencies", std::move(dependencies)},
            {"counts",
             {{"errors", report.errorCount},
              {"warnings", report.warningCount},
              {"infos", report.infoCount},
              {"hints", report.hintCount}}},
            {"truncated", report.truncated},
            {"issues", std::move(issues)},
            {"extensions", std::move(extensions)},
            {"statistics", statisticsToJson(report.statistics)},
            {"failureReason", report.failureReason},
            {"generatedUnixMs", report.generatedUnixMs}};
}

bool safeReportKey(const std::string &key) {
    if (key.empty() || key.front() == '/' || key.front() == '\\' ||
        key.find("..") != std::string::npos ||
        key.find(':') != std::string::npos)
        return false;
    return std::all_of(key.begin(), key.end(), [](unsigned char value) {
        return std::isalnum(value) || value == '-' || value == '_' ||
               value == '.' || value == '/';
    });
}

std::filesystem::path reportPathFromKey(
    const std::filesystem::path &cacheRoot, const std::string &reportKey) {
    if (!safeReportKey(reportKey))
        throw std::runtime_error("Invalid validation report key");
    return cacheRoot / "validation/reports" /
           std::filesystem::path(reportKey);
}

void atomicReplace(const std::filesystem::path &temporary,
                   const std::filesystem::path &destination) {
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error(
            "Could not atomically replace validation metadata (error " +
            std::to_string(GetLastError()) + ")");
    }
}

Json loadIndex(const std::filesystem::path &path) {
    if (!std::filesystem::is_regular_file(path))
        return {{"schemaVersion", 1}, {"scenes", Json::object()}};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not read validation index");
    Json root;
    input >> root;
    if (!root.is_object() || root.value("schemaVersion", 0u) != 1 ||
        !root.contains("scenes") || !root.at("scenes").is_object()) {
        throw std::runtime_error("Validation index is invalid");
    }
    return root;
}

void saveIndex(const std::filesystem::path &path, const Json &root) {
    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path temporary =
        path.string() + ".tmp-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetCurrentThreadId());
    try {
        std::ofstream output(temporary,
                             std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Could not create validation index");
        output << root.dump(2) << '\n';
        output.flush();
        if (!output)
            throw std::runtime_error("Could not flush validation index");
        output.close();
        atomicReplace(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::filesystem::path dependencyPath(
    const std::filesystem::path &sourcePath,
    const DerivedFileStamp &dependency) {
    std::filesystem::path path =
        std::filesystem::u8path(dependency.path);
    if (path.is_absolute())
        return path;
    return sourcePath.parent_path() / path;
}

bool stampMatchesPath(const DerivedFileStamp &expected,
                      const std::filesystem::path &path) {
    const DerivedFileStamp actual = fileStamp(path);
    return expected.size == actual.size &&
           expected.writeTime == actual.writeTime;
}

} // namespace

const char *assetValidationStateName(AssetValidationState state) {
    switch (state) {
    case AssetValidationState::NotChecked:
        return "NotChecked";
    case AssetValidationState::Valid:
        return "Valid";
    case AssetValidationState::Warnings:
        return "Warnings";
    case AssetValidationState::Invalid:
        return "Invalid";
    case AssetValidationState::Stale:
        return "Stale";
    case AssetValidationState::Unavailable:
        return "Unavailable";
    case AssetValidationState::Failed:
        return "Failed";
    case AssetValidationState::NotApplicable:
        return "NotApplicable";
    }
    return "NotChecked";
}

std::optional<AssetValidationState>
assetValidationStateFromName(const std::string &name) {
    for (AssetValidationState state :
         {AssetValidationState::NotChecked, AssetValidationState::Valid,
          AssetValidationState::Warnings, AssetValidationState::Invalid,
          AssetValidationState::Stale, AssetValidationState::Unavailable,
          AssetValidationState::Failed,
          AssetValidationState::NotApplicable}) {
        if (name == assetValidationStateName(state))
            return state;
    }
    return std::nullopt;
}

const char *gltfExtensionSupportName(GltfExtensionSupport support) {
    switch (support) {
    case GltfExtensionSupport::Supported:
        return "Supported";
    case GltfExtensionSupport::Partial:
        return "Partial";
    case GltfExtensionSupport::Unsupported:
        return "Unsupported";
    }
    return "Unsupported";
}

GltfExtensionSupport gltfExtensionSupport(const std::string &extension) {
    if (extension == "KHR_lights_punctual" ||
        extension == "KHR_materials_emissive_strength")
        return GltfExtensionSupport::Supported;
    if (extension == "KHR_materials_transmission" ||
        extension == "KHR_materials_volume")
        return GltfExtensionSupport::Partial;
    return GltfExtensionSupport::Unsupported;
}

std::string assetValidationInputFingerprint(
    const std::string &sourceSha256, const DerivedFileStamp &source,
    const std::vector<DerivedFileStamp> &dependencies) {
    std::vector<DerivedFileStamp> sorted = dependencies;
    std::sort(sorted.begin(), sorted.end(),
              [](const DerivedFileStamp &left,
                 const DerivedFileStamp &right) {
                  return left.path < right.path;
              });
    std::string canonical =
        "asset-validation-v1\n" + sourceSha256 + "\n" + source.path + "\n" +
        std::to_string(source.size) + "\n" +
        std::to_string(source.writeTime) + "\n";
    for (const auto &dependency : sorted) {
        canonical += dependency.path + "\n" +
                     std::to_string(dependency.size) + "\n" +
                     std::to_string(dependency.writeTime) + "\n";
    }
    return sha256String(canonical);
}

std::filesystem::path assetValidationReportPath(
    const std::filesystem::path &cacheRoot,
    const std::string &validatorVersion,
    const std::string &inputFingerprint) {
    return cacheRoot / "validation/reports" / validatorVersion /
           (inputFingerprint + ".json");
}

bool saveAssetValidationReport(const std::filesystem::path &cacheRoot,
                               AssetValidationReport &report,
                               std::string &error) {
    try {
        if (report.validatorVersion.empty() ||
            report.inputFingerprint.empty())
            throw std::runtime_error(
                "Validation report identity is incomplete");
        report.schemaVersion = AssetValidationReport::kSchemaVersion;
        report.reportKey = report.validatorVersion + "/" +
                           report.inputFingerprint + ".json";
        report.generatedUnixMs =
            report.generatedUnixMs == 0 ? unixTimeMs()
                                        : report.generatedUnixMs;
        const std::filesystem::path destination =
            reportPathFromKey(cacheRoot, report.reportKey);
        CacheMutationLock lock(cacheRoot);
        std::filesystem::create_directories(destination.parent_path());
        const std::filesystem::path temporary =
            destination.string() + ".tmp-" +
            std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetCurrentThreadId());
        try {
            std::ofstream output(temporary,
                                 std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error(
                    "Could not create validation report");
            output << reportToJson(report).dump(2) << '\n';
            output.flush();
            if (!output)
                throw std::runtime_error(
                    "Could not flush validation report");
            output.close();
            atomicReplace(temporary, destination);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

bool loadAssetValidationReport(const std::filesystem::path &path,
                               AssetValidationReport &report,
                               std::string &error) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "validation report not found";
            return false;
        }
        Json root;
        input >> root;
        report = {};
        report.schemaVersion = root.value("schemaVersion", 0u);
        if (report.schemaVersion != AssetValidationReport::kSchemaVersion)
            throw std::runtime_error(
                "Unsupported validation report schema");
        const auto state = assetValidationStateFromName(
            root.value("state", std::string{}));
        if (!state)
            throw std::runtime_error("Invalid validation report state");
        report.state = *state;
        report.validatorName =
            root.value("validatorName", std::string{});
        report.validatorVersion =
            root.value("validatorVersion", std::string{});
        report.reportKey = root.value("reportKey", std::string{});
        report.inputFingerprint =
            root.value("inputFingerprint", std::string{});
        report.sourceSha256 =
            root.value("sourceSha256", std::string{});
        report.source = stampFromJson(root.at("source"));
        for (const Json &dependency : root.at("dependencies"))
            report.dependencies.push_back(stampFromJson(dependency));
        const Json &counts = root.at("counts");
        report.errorCount = counts.value("errors", uint64_t{0});
        report.warningCount = counts.value("warnings", uint64_t{0});
        report.infoCount = counts.value("infos", uint64_t{0});
        report.hintCount = counts.value("hints", uint64_t{0});
        report.truncated = root.value("truncated", false);
        for (const Json &issue : root.at("issues"))
            report.issues.push_back(issueFromJson(issue));
        for (const Json &extension : root.at("extensions"))
            report.extensions.push_back(extensionFromJson(extension));
        report.statistics = statisticsFromJson(root.at("statistics"));
        report.failureReason =
            root.value("failureReason", std::string{});
        report.generatedUnixMs = root.value("generatedUnixMs", int64_t{0});
        if (report.validatorVersion.empty() || report.reportKey.empty() ||
            report.inputFingerprint.empty() ||
            report.reportKey != report.validatorVersion + "/" +
                                    report.inputFingerprint + ".json") {
            throw std::runtime_error(
                "Validation report identity is invalid");
        }
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

SceneValidationReceipt
sceneValidationReceipt(const AssetValidationReport &report) {
    return {report.reportKey, report.inputFingerprint, report.state};
}

bool validationReportMatchesSource(const AssetValidationReport &report,
                                   const std::filesystem::path &sourcePath,
                                   std::string &reason) {
    if (!std::filesystem::is_regular_file(sourcePath)) {
        reason = "scene source is missing";
        return false;
    }
    if (!stampMatchesPath(report.source, sourcePath)) {
        reason = "scene source stamp changed";
        return false;
    }
    for (const auto &dependency : report.dependencies) {
        const std::filesystem::path path =
            dependencyPath(sourcePath, dependency);
        if (!std::filesystem::is_regular_file(path)) {
            reason = "scene dependency is missing: " + dependency.path;
            return false;
        }
        if (!stampMatchesPath(dependency, path)) {
            reason = "scene dependency stamp changed: " + dependency.path;
            return false;
        }
    }
    reason.clear();
    return true;
}

AssetValidationQuery queryValidationReceipt(
    const std::filesystem::path &cacheRoot,
    const SceneValidationReceipt &receipt,
    const std::filesystem::path &sourcePath) {
    AssetValidationQuery query;
    if (receipt.reportKey.empty() || receipt.inputFingerprint.empty()) {
        query.reason = "validation receipt is missing";
        return query;
    }
    try {
        query.reportPath = reportPathFromKey(cacheRoot, receipt.reportKey);
    } catch (const std::exception &exception) {
        query.state = AssetValidationState::Failed;
        query.reason = exception.what();
        return query;
    }
    AssetValidationReport report;
    std::string error;
    if (!loadAssetValidationReport(query.reportPath, report, error)) {
        query.state = AssetValidationState::Failed;
        query.reason = error;
        return query;
    }
    if (report.inputFingerprint != receipt.inputFingerprint ||
        report.state != receipt.state) {
        query.state = AssetValidationState::Failed;
        query.reason = "validation receipt does not match report";
        return query;
    }
    if (!validationReportMatchesSource(report, sourcePath, error)) {
        query.state = AssetValidationState::Stale;
        query.reason = error;
        query.report = std::move(report);
        return query;
    }
    query.state = report.state;
    query.report = std::move(report);
    return query;
}

SceneValidationReceipt bindSceneValidation(
    const std::filesystem::path &cacheRoot,
    const std::filesystem::path &projectRoot, const std::string &sceneId,
    const std::filesystem::path &projectRelativeSource,
    const SceneValidationReceipt &receipt) {
    if (!isStableAssetId(sceneId))
        throw std::runtime_error("Invalid scene ID for validation binding");
    if (projectRelativeSource.empty() || projectRelativeSource.is_absolute())
        throw std::runtime_error(
            "Validation binding source must be project-relative");
    const std::filesystem::path sourcePath =
        projectRoot / projectRelativeSource;
    AssetValidationQuery query =
        queryValidationReceipt(cacheRoot, receipt, sourcePath);
    if (query.state == AssetValidationState::Stale && query.report) {
        // Copying into the project may change file timestamps without changing
        // the bytes that were validated. Rebase the report onto the published
        // files after the staging preflight has succeeded.
        AssetValidationReport rebased = *query.report;
        if (rebased.sourceSha256 != sha256File(sourcePath))
            throw std::runtime_error(
                "Published scene bytes differ from validated source");
        rebased.source = fileStamp(sourcePath, rebased.sourceSha256);
        rebased.source.path = sourcePath.filename().generic_string();
        for (auto &dependency : rebased.dependencies) {
            const std::filesystem::path path =
                sourcePath.parent_path() /
                std::filesystem::u8path(dependency.path);
            if (!std::filesystem::is_regular_file(path))
                throw std::runtime_error(
                    "Published validation dependency is missing: " +
                    dependency.path);
            dependency = fileStamp(path);
            dependency.path = std::filesystem::relative(
                                  path, sourcePath.parent_path())
                                  .generic_string();
        }
        rebased.inputFingerprint = assetValidationInputFingerprint(
            rebased.sourceSha256, rebased.source, rebased.dependencies);
        std::string error;
        if (!saveAssetValidationReport(cacheRoot, rebased, error))
            throw std::runtime_error(error);
        query.state = rebased.state;
        query.report = std::move(rebased);
        query.reportPath = reportPathFromKey(
            cacheRoot, query.report->reportKey);
    }
    if (!query.report || query.state == AssetValidationState::NotChecked ||
        query.state == AssetValidationState::Stale ||
        query.state == AssetValidationState::NotApplicable) {
        throw std::runtime_error(
            "Cannot bind scene validation in state " +
            std::string(assetValidationStateName(query.state)) +
            (query.reason.empty() ? std::string{} : ": " + query.reason));
    }

    CacheMutationLock lock(cacheRoot);
    const std::filesystem::path indexPath = cacheRoot / kValidationIndexName;
    Json root = loadIndex(indexPath);
    root["scenes"][sceneId] =
        {{"source", projectRelativeSource.generic_string()},
         {"reportKey", query.report->reportKey},
         {"inputFingerprint", query.report->inputFingerprint},
         {"state", assetValidationStateName(query.report->state)}};
    saveIndex(indexPath, root);
    return sceneValidationReceipt(*query.report);
}

AssetValidationQuery querySceneValidation(
    const std::filesystem::path &cacheRoot,
    const std::filesystem::path &projectRoot, const std::string &sceneId) {
    AssetValidationQuery query;
    try {
        const Json root = loadIndex(cacheRoot / kValidationIndexName);
        const Json &scenes = root.at("scenes");
        if (!scenes.contains(sceneId)) {
            query.reason = "scene has not been validated";
            return query;
        }
        const Json &entry = scenes.at(sceneId);
        SceneValidationReceipt receipt;
        receipt.reportKey = entry.value("reportKey", std::string{});
        receipt.inputFingerprint =
            entry.value("inputFingerprint", std::string{});
        receipt.state = assetValidationStateFromName(
                            entry.value("state", std::string{}))
                            .value_or(AssetValidationState::Failed);
        const std::filesystem::path relativeSource =
            std::filesystem::u8path(entry.value("source", std::string{}));
        if (relativeSource.empty() || relativeSource.is_absolute()) {
            query.state = AssetValidationState::Failed;
            query.reason = "validation index source is invalid";
            return query;
        }
        return queryValidationReceipt(cacheRoot, receipt,
                                      projectRoot / relativeSource);
    } catch (const std::exception &exception) {
        query.state = AssetValidationState::Failed;
        query.reason = exception.what();
        return query;
    }
}

void removeSceneValidationBinding(
    const std::filesystem::path &cacheRoot, const std::string &sceneId) {
    if (cacheRoot.empty() || sceneId.empty())
        return;
    CacheMutationLock lock(cacheRoot);
    const std::filesystem::path indexPath = cacheRoot / kValidationIndexName;
    Json root = loadIndex(indexPath);
    if (root["scenes"].erase(sceneId) != 0)
        saveIndex(indexPath, root);
}

std::vector<std::filesystem::path> referencedAssetValidationReports(
    const std::filesystem::path &cacheRoot) {
    std::vector<std::filesystem::path> reports;
    const Json root = loadIndex(cacheRoot / kValidationIndexName);
    for (const auto &entry : root.at("scenes")) {
        const std::string reportKey =
            entry.value("reportKey", std::string{});
        if (reportKey.empty())
            throw std::runtime_error(
                "Validation index contains an empty report key");
        reports.push_back(
            reportPathFromKey(cacheRoot, reportKey).lexically_normal());
    }
    return reports;
}

} // namespace vkr
