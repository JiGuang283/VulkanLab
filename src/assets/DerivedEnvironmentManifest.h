#pragma once

#include "DerivedTextureManifest.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

enum class EnvironmentMapKind {
    Radiance,
    Irradiance,
    PrefilteredSpecular,
    BrdfLut,
};

const char *environmentMapKindName(EnvironmentMapKind kind);
std::optional<EnvironmentMapKind>
environmentMapKindFromName(const std::string &name);

struct DerivedEnvironmentImage {
    EnvironmentMapKind kind = EnvironmentMapKind::Radiance;
    std::string format;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    std::string cacheKey;
    std::string blob;
    uint64_t bytes = 0;
};

struct DerivedEnvironmentManifest {
    static constexpr uint32_t kSchemaVersion = 1;

    uint32_t schemaVersion = kSchemaVersion;
    std::string projectId;
    std::string environmentId;
    std::string profileId;
    std::string sourcePath;
    std::string sourceSha256;
    std::string algorithmVersion = "ibl-cpu-v1";
    std::string coordinateSystem = "right-handed-z-up";
    uint32_t diffuseSamples = 1024;
    uint32_t specularSamples = 512;
    uint32_t brdfSamples = 1024;
    DerivedFileStamp source;
    std::vector<DerivedEnvironmentImage> images;

    const DerivedEnvironmentImage *find(EnvironmentMapKind kind) const;
};

std::filesystem::path derivedEnvironmentManifestPath(
    const std::filesystem::path &cacheRoot,
    const std::string &environmentId, const std::string &profileId);

bool loadDerivedEnvironmentManifest(
    const std::filesystem::path &path,
    DerivedEnvironmentManifest &manifest, std::string &error);
bool saveDerivedEnvironmentManifest(
    const std::filesystem::path &path,
    const DerivedEnvironmentManifest &manifest, std::string &error);

} // namespace vkr
