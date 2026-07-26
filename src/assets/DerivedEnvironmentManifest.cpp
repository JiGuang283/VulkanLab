#include "DerivedEnvironmentManifest.h"

#include <json.hpp>

#include <atomic>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace vkr {
namespace {

using Json = nlohmann::json;

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

} // namespace

const char *environmentMapKindName(EnvironmentMapKind kind) {
    switch (kind) {
    case EnvironmentMapKind::Radiance:
        return "radiance";
    case EnvironmentMapKind::Irradiance:
        return "irradiance";
    case EnvironmentMapKind::PrefilteredSpecular:
        return "prefiltered-specular";
    case EnvironmentMapKind::BrdfLut:
        return "brdf-lut";
    }
    return "radiance";
}

std::optional<EnvironmentMapKind>
environmentMapKindFromName(const std::string &name) {
    if (name == "radiance")
        return EnvironmentMapKind::Radiance;
    if (name == "irradiance")
        return EnvironmentMapKind::Irradiance;
    if (name == "prefiltered-specular")
        return EnvironmentMapKind::PrefilteredSpecular;
    if (name == "brdf-lut")
        return EnvironmentMapKind::BrdfLut;
    return std::nullopt;
}

const DerivedEnvironmentImage *
DerivedEnvironmentManifest::find(EnvironmentMapKind kind) const {
    for (const DerivedEnvironmentImage &image : images) {
        if (image.kind == kind)
            return &image;
    }
    return nullptr;
}

std::filesystem::path derivedEnvironmentManifestPath(
    const std::filesystem::path &cacheRoot,
    const std::string &environmentId, const std::string &profileId) {
    return cacheRoot / "manifests" / "environments" / environmentId /
           (profileId + ".json");
}

bool loadDerivedEnvironmentManifest(
    const std::filesystem::path &path,
    DerivedEnvironmentManifest &manifest, std::string &error) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "manifest not found";
            return false;
        }
        Json root;
        input >> root;
        manifest = {};
        manifest.schemaVersion = root.value("schemaVersion", 0u);
        if (manifest.schemaVersion !=
            DerivedEnvironmentManifest::kSchemaVersion) {
            error = "unsupported environment manifest schema";
            return false;
        }
        manifest.projectId = root.value("projectId", std::string{});
        manifest.environmentId =
            root.value("environmentId", std::string{});
        manifest.profileId = root.value("profileId", std::string{});
        manifest.sourcePath = root.value("sourcePath", std::string{});
        manifest.sourceSha256 =
            root.value("sourceSha256", std::string{});
        manifest.algorithmVersion =
            root.value("algorithmVersion", std::string{});
        manifest.coordinateSystem =
            root.value("coordinateSystem", std::string{});
        manifest.diffuseSamples = root.value("diffuseSamples", 0u);
        manifest.specularSamples = root.value("specularSamples", 0u);
        manifest.brdfSamples = root.value("brdfSamples", 0u);
        manifest.source = stampFromJson(root.at("source"));
        if (manifest.projectId.empty() || manifest.environmentId.empty() ||
            manifest.profileId.empty() || manifest.sourceSha256.empty() ||
            manifest.algorithmVersion.empty()) {
            error = "environment manifest identity is incomplete";
            return false;
        }
        for (const Json &item : root.at("images")) {
            const auto kind = environmentMapKindFromName(
                item.value("kind", std::string{}));
            if (!kind) {
                error = "environment manifest has an unknown image kind";
                return false;
            }
            DerivedEnvironmentImage image;
            image.kind = *kind;
            image.format = item.value("format", std::string{});
            image.width = item.value("width", 0u);
            image.height = item.value("height", 0u);
            image.mipLevels = item.value("mipLevels", 0u);
            image.arrayLayers = item.value("arrayLayers", 0u);
            image.cacheKey = item.value("cacheKey", std::string{});
            image.blob = item.value("blob", std::string{});
            image.bytes = item.value("bytes", uint64_t{0});
            if (image.format.empty() || image.width == 0 ||
                image.height == 0 || image.mipLevels == 0 ||
                image.arrayLayers == 0 || image.cacheKey.empty() ||
                image.blob.empty()) {
                error = "environment manifest image is incomplete";
                return false;
            }
            manifest.images.push_back(std::move(image));
        }
        if (manifest.images.size() != 4 ||
            !manifest.find(EnvironmentMapKind::Radiance) ||
            !manifest.find(EnvironmentMapKind::Irradiance) ||
            !manifest.find(EnvironmentMapKind::PrefilteredSpecular) ||
            !manifest.find(EnvironmentMapKind::BrdfLut)) {
            error = "environment manifest must contain four image kinds";
            return false;
        }
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

bool saveDerivedEnvironmentManifest(
    const std::filesystem::path &path,
    const DerivedEnvironmentManifest &manifest, std::string &error) {
    std::filesystem::path temporary;
    try {
        Json root{{"schemaVersion", manifest.schemaVersion},
                  {"projectId", manifest.projectId},
                  {"environmentId", manifest.environmentId},
                  {"profileId", manifest.profileId},
                  {"sourcePath", manifest.sourcePath},
                  {"sourceSha256", manifest.sourceSha256},
                  {"algorithmVersion", manifest.algorithmVersion},
                  {"coordinateSystem", manifest.coordinateSystem},
                  {"diffuseSamples", manifest.diffuseSamples},
                  {"specularSamples", manifest.specularSamples},
                  {"brdfSamples", manifest.brdfSamples},
                  {"source", stampToJson(manifest.source)},
                  {"images", Json::array()}};
        for (const DerivedEnvironmentImage &image : manifest.images) {
            root["images"].push_back(
                {{"kind", environmentMapKindName(image.kind)},
                 {"format", image.format},
                 {"width", image.width},
                 {"height", image.height},
                 {"mipLevels", image.mipLevels},
                 {"arrayLayers", image.arrayLayers},
                 {"cacheKey", image.cacheKey},
                 {"blob", image.blob},
                 {"bytes", image.bytes}});
        }
        std::filesystem::create_directories(path.parent_path());
        static std::atomic<uint64_t> temporaryCounter{0};
        temporary = path.string() + ".tmp-" +
                    std::to_string(temporaryCounter.fetch_add(1));
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error(
                    "could not create environment manifest");
            output << root.dump(2) << '\n';
            output.flush();
            if (!output)
                throw std::runtime_error(
                    "could not flush environment manifest");
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING |
                             MOVEFILE_WRITE_THROUGH)) {
            const DWORD moveError = GetLastError();
            throw std::system_error(
                static_cast<int>(moveError), std::system_category(),
                "could not publish environment manifest");
        }
#else
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::rename(temporary, path);
#endif
        temporary.clear();
        return true;
    } catch (const std::exception &exception) {
        if (!temporary.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        error = exception.what();
        return false;
    }
}

} // namespace vkr
