#include "assets/ArtifactCachePruner.h"
#include "assets/DerivedTextureManifest.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

void requirePrune(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryPruneCache {
  public:
    TemporaryPruneCache() {
        root = std::filesystem::temp_directory_path() /
               ("vulkan_lab_prune_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        std::filesystem::create_directories(root / "blobs");
    }
    ~TemporaryPruneCache() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

void writePruneKtx(const std::filesystem::path &path) {
    static constexpr std::array<uint8_t, 12> identifier{
        0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
        0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(identifier.data()),
                 identifier.size());
}

void testSafePrune() {
    TemporaryPruneCache temporary;
    const auto referenced = temporary.root / "blobs/referenced.ktx2";
    const auto oldOrphan = temporary.root / "blobs/old.ktx2";
    const auto recentOrphan = temporary.root / "blobs/recent.ktx2";
    writePruneKtx(referenced);
    writePruneKtx(oldOrphan);
    writePruneKtx(recentOrphan);
    std::filesystem::last_write_time(
        oldOrphan, std::filesystem::last_write_time(oldOrphan) -
                       std::chrono::hours(24 * 10));

    vkr::DerivedTextureManifest manifest;
    manifest.projectId = "prune";
    manifest.sceneId = "scene";
    manifest.profileId = "profile";
    manifest.textureLimit = 512;
    vkr::DerivedTextureEntry entry;
    entry.imageIndex = 0;
    entry.blob = "blobs/referenced.ktx2";
    entry.source.path = "source.png";
    manifest.entries.push_back(entry);
    std::string error;
    requirePrune(vkr::saveDerivedTextureManifest(
                     vkr::derivedManifestPath(temporary.root, "scene",
                                              "profile"),
                     manifest, error),
                 "could not publish prune test manifest");

    const vkr::ArtifactPruneReport dryRun =
        vkr::pruneArtifactCache({temporary.root, 7, false});
    requirePrune(dryRun.candidates.size() == 1 &&
                     dryRun.candidates[0].path == oldOrphan,
                 "dry-run did not select only the old orphan");
    requirePrune(std::filesystem::is_regular_file(oldOrphan),
                 "dry-run removed an orphan blob");
    requirePrune(dryRun.protectedBlobs == 1 &&
                     dryRun.deferredBlobFiles == 1,
                 "prune protection or retention accounting is wrong");

    const vkr::ArtifactPruneReport executed =
        vkr::pruneArtifactCache({temporary.root, 7, true});
    requirePrune(executed.deletedBlobFiles == 1 &&
                     !std::filesystem::exists(oldOrphan),
                 "execute did not remove the old orphan");
    requirePrune(std::filesystem::is_regular_file(referenced) &&
                     std::filesystem::is_regular_file(recentOrphan),
                 "execute removed a protected or recent blob");
}

void testCorruptManifestBlocksPrune() {
    TemporaryPruneCache temporary;
    const auto orphan = temporary.root / "blobs/orphan.ktx2";
    writePruneKtx(orphan);
    std::filesystem::create_directories(temporary.root / "manifests/bad");
    std::ofstream(temporary.root / "manifests/bad/profile.json")
        << "{broken";
    bool rejected = false;
    try {
        (void)vkr::pruneArtifactCache({temporary.root, 0, true});
    } catch (const std::exception &) {
        rejected = true;
    }
    requirePrune(rejected && std::filesystem::is_regular_file(orphan),
                 "corrupt manifest did not fail closed during prune");
}

} // namespace

void runArtifactCachePrunerTests() {
    testSafePrune();
    testCorruptManifestBlocksPrune();
}
