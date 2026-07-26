#include "EnvironmentCacheBuilder.h"

#include "assets/ArtifactStatus.h"
#include "assets/AssetImportManager.h"
#include "assets/DerivedEnvironmentCache.h"
#include "assets/DerivedEnvironmentManifest.h"
#include "assets/EnvironmentLoadManager.h"
#include "diagnostics/CaptureTypes.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

static_assert((vkr::EnvironmentLoadManager::kTaskIdMask &
               vkr::kCaptureTaskIdBase) == 0);
static_assert((vkr::EnvironmentLoadManager::kTaskIdMask &
               vkr::AssetImportManager::kTaskIdMask) == 0);
static_assert((vkr::kCaptureTaskIdBase &
               vkr::AssetImportManager::kTaskIdMask) == 0);

void requireEnvironment(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("vulkan-lab-environment-" +
                 std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};

void writeTinyHdr(const std::filesystem::path &path, uint8_t red) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("could not create HDR fixture");
    output << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n";
    for (uint32_t pixel = 0; pixel < 8; ++pixel) {
        const char rgbe[4] = {
            static_cast<char>(red),
            static_cast<char>(64 + pixel),
            static_cast<char>(32),
            static_cast<char>(129)};
        output.write(rgbe, sizeof(rgbe));
    }
}

void testCubeDirectionsAndHammersley() {
    static const glm::vec3 expected[] = {
        {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},  {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
    };
    for (uint32_t face = 0; face < 6; ++face) {
        const glm::vec3 direction =
            vkr::assettool::environmentCubeDirection(face, 0.0f, 0.0f);
        requireEnvironment(
            glm::length(direction - expected[face]) < 1.0e-6f,
            "cubemap face direction convention changed");
    }
    for (uint32_t index = 0; index < 16; ++index) {
        const glm::vec2 first =
            vkr::assettool::environmentHammersley(index, 16);
        const glm::vec2 second =
            vkr::assettool::environmentHammersley(index, 16);
        requireEnvironment(first == second && std::isfinite(first.x) &&
                               std::isfinite(first.y) && first.x >= 0.0f &&
                               first.x < 1.0f && first.y >= 0.0f &&
                               first.y < 1.0f,
                           "Hammersley sequence is not deterministic");
    }
}

vkr::assettool::EnvironmentCacheBuildOptions makeOptions(
    const TemporaryDirectory &temporary,
    const std::filesystem::path &source) {
    vkr::assettool::EnvironmentCacheBuildOptions options;
    options.source = source;
    options.sourceProjectPath = "assets/environments/test.hdr";
    options.cacheRoot = temporary.path() / "cache";
    options.projectId = "environment-tests";
    options.environmentId = "test-environment";
    options.profile.id = "tiny_ibl";
    options.profile.radianceSize = 4;
    options.profile.irradianceSize = 2;
    options.profile.prefilteredSize = 4;
    options.profile.brdfLutSize = 4;
    options.profile.diffuseSamples = 8;
    options.profile.specularSamples = 8;
    options.profile.brdfSamples = 8;
    options.maxWorkers = 2;
    return options;
}

void testEnvironmentBuildAndCache() {
    TemporaryDirectory temporary;
    const std::filesystem::path source = temporary.path() / "test.hdr";
    writeTinyHdr(source, 128);
    auto options = makeOptions(temporary, source);

    const auto first =
        vkr::assettool::buildEnvironmentCache(options);
    requireEnvironment(first.generatedBlobs == 4 &&
                           first.reusedBlobs == 0 &&
                           std::filesystem::is_regular_file(
                               first.manifestPath),
                       "initial environment bake did not publish four blobs");

    vkr::DerivedEnvironmentManifest manifest;
    std::string error;
    requireEnvironment(vkr::loadDerivedEnvironmentManifest(
                           first.manifestPath, manifest, error) &&
                           manifest.images.size() == 4,
                       "environment manifest could not be loaded");
    const auto second =
        vkr::assettool::buildEnvironmentCache(options);
    requireEnvironment(second.generatedBlobs == 0 &&
                           second.reusedBlobs == 4,
                       "second environment bake did not reuse blobs");

    vkr::DerivedEnvironmentCache cache(
        options.cacheRoot, source, options.projectId,
        options.environmentId, "Test Environment", options.profile.id,
        true);
    const vkr::PreparedEnvironmentData prepared = cache.load();
    requireEnvironment(
        prepared.radiance.arrayLayers == 6 &&
            prepared.radiance.mipLevels == 3 &&
            prepared.radiance.subresources.size() == 18 &&
            prepared.irradiance.subresources.size() == 6 &&
            prepared.prefilteredSpecular.subresources.size() == 18 &&
            prepared.brdfLut.arrayLayers == 1 &&
            prepared.brdfLut.subresources.size() == 1,
        "KTX2 environment face/mip layout changed");
    for (const auto &subresource : prepared.radiance.subresources) {
        requireEnvironment(
            subresource.size > 0 &&
                subresource.offset + subresource.size <=
                    prepared.radiance.bytes.size(),
            "radiance KTX2 subresource range is invalid");
    }

    std::atomic_bool cancelled{true};
    options.force = true;
    options.cancelRequested = &cancelled;
    bool cancellationObserved = false;
    try {
        (void)vkr::assettool::buildEnvironmentCache(options);
    } catch (const std::exception &) {
        cancellationObserved = true;
    }
    requireEnvironment(cancellationObserved,
                       "environment bake ignored cancellation");
    requireEnvironment(vkr::loadDerivedEnvironmentManifest(
                           first.manifestPath, manifest, error),
                       "cancelled bake replaced the valid manifest");

    options.force = false;
    options.cancelRequested = nullptr;
    writeTinyHdr(source, 160);
    const auto changed =
        vkr::assettool::buildEnvironmentCache(options);
    requireEnvironment(changed.generatedBlobs == 4,
                       "source content change did not invalidate cache keys");

    requireEnvironment(vkr::loadDerivedEnvironmentManifest(
                           changed.manifestPath, manifest, error),
                       "changed environment manifest could not be loaded");
    const std::filesystem::path corruptBlob =
        options.cacheRoot / manifest.images.front().blob;
    {
        std::ofstream output(corruptBlob,
                             std::ios::binary | std::ios::trunc);
        output << "not a KTX2";
    }
    const vkr::ArtifactStatus status =
        vkr::inspectEnvironmentArtifacts(
            {options.cacheRoot, source, options.projectId,
             options.environmentId, options.profile.id});
    requireEnvironment(status.state == vkr::ArtifactState::Invalid,
                       "corrupt environment blob was accepted");
}

} // namespace

void runEnvironmentCacheTests() {
    testCubeDirectionsAndHammersley();
    testEnvironmentBuildAndCache();
}
