#include "assets/ContentHash.h"
#include "assets/ProjectContext.h"
#include "assets/RuntimePackage.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

void requirePackage(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryPackage {
  public:
    TemporaryPackage() {
        root = std::filesystem::temp_directory_path() /
               ("vulkan_lab_package_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        std::filesystem::create_directories(root / "assets");
    }
    ~TemporaryPackage() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

void writePackageFile(const std::filesystem::path &path,
                      const std::string &contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

vkr::RuntimePackageFile packageFile(const std::filesystem::path &root,
                                    const std::string &relative) {
    const auto path = root / relative;
    return {relative, std::filesystem::file_size(path),
            vkr::sha256File(path)};
}

void testRuntimePackageRoundTrip() {
    TemporaryPackage temporary;
    writePackageFile(temporary.root / "assets/catalog.json", "catalog");
    writePackageFile(temporary.root / "VulkanLab.exe", "executable");

    vkr::RuntimePackageManifest manifest;
    manifest.platform = "windows-x64";
    manifest.projectId = "package-test";
    manifest.profileId = "desktop-512";
    manifest.files = {packageFile(temporary.root, "VulkanLab.exe"),
                      packageFile(temporary.root, "assets/catalog.json")};
    std::sort(manifest.files.begin(), manifest.files.end(),
              [](const auto &left, const auto &right) {
                  return left.path < right.path;
              });
    std::string error;
    requirePackage(vkr::saveRuntimePackageManifest(
                       temporary.root / "package_manifest.json", manifest,
                       error),
                   "could not save runtime package manifest");
    vkr::RuntimePackageManifest loaded;
    requirePackage(vkr::loadRuntimePackageManifest(
                       temporary.root / "package_manifest.json", loaded,
                       error),
                   "could not load runtime package manifest");
    const auto verified = vkr::verifyRuntimePackage(temporary.root, loaded);
    requirePackage(verified.fileCount == 2 && verified.totalBytes > 0,
                   "runtime package verification returned wrong totals");
    requirePackage(vkr::findRuntimePackageRoot(
                       temporary.root / "VulkanLab.exe") == temporary.root,
                   "runtime package was not discovered beside executable");
    const vkr::ProjectContext context = vkr::ProjectContextResolver::resolve(
        std::nullopt, temporary.root / "VulkanLab.exe");
    requirePackage(context.cookedPackage && !context.catalogWritable &&
                       context.projectRoot ==
                           std::filesystem::weakly_canonical(temporary.root) &&
                       context.cacheRoot ==
                           context.projectRoot / "runtime_assets" &&
                       context.packageProfileId == "desktop-512",
                   "package ProjectContext was not configured correctly");
    bool overrideRejected = false;
    try {
        (void)vkr::ProjectContextResolver::resolve(
            temporary.root, temporary.root / "VulkanLab.exe");
    } catch (const std::exception &) {
        overrideRejected = true;
    }
    requirePackage(overrideRejected,
                   "--project override was accepted for a package");

    writePackageFile(temporary.root / "assets/catalog.json", "tampered");
    bool rejected = false;
    try {
        (void)vkr::verifyRuntimePackage(temporary.root, loaded);
    } catch (const std::exception &) {
        rejected = true;
    }
    requirePackage(rejected, "tampered package file was accepted");
}

void testUnsafeRuntimePackagePath() {
    TemporaryPackage temporary;
    writePackageFile(temporary.root / "assets/catalog.json", "catalog");
    writePackageFile(temporary.root / "VulkanLab.exe", "executable");
    vkr::RuntimePackageManifest manifest;
    manifest.platform = "windows-x64";
    manifest.projectId = "package-test";
    manifest.profileId = "desktop-512";
    manifest.files = {{"../outside", 1, std::string(64, '0')},
                      packageFile(temporary.root, "VulkanLab.exe"),
                      packageFile(temporary.root, "assets/catalog.json")};
    std::sort(manifest.files.begin(), manifest.files.end(),
              [](const auto &left, const auto &right) {
                  return left.path < right.path;
              });
    std::string error;
    requirePackage(!vkr::saveRuntimePackageManifest(
                       temporary.root / "package_manifest.json", manifest,
                       error),
                   "unsafe package path was accepted");
}

void testRuntimePackageRequiresExecutable() {
    TemporaryPackage temporary;
    writePackageFile(temporary.root / "assets/catalog.json", "catalog");
    vkr::RuntimePackageManifest manifest;
    manifest.platform = "windows-x64";
    manifest.projectId = "package-test";
    manifest.profileId = "desktop-512";
    manifest.files = {packageFile(temporary.root, "assets/catalog.json")};
    std::string error;
    requirePackage(!vkr::saveRuntimePackageManifest(
                       temporary.root / "package_manifest.json", manifest,
                       error),
                   "package without executable was accepted");
}

} // namespace

void runRuntimePackageTests() {
    testRuntimePackageRoundTrip();
    testUnsafeRuntimePackagePath();
    testRuntimePackageRequiresExecutable();
}
