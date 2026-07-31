#include "DerivedTextureManifest.h"

#include <json.hpp>

#include <atomic>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace vkr {

namespace {

using Json = nlohmann::json;

std::string genericPath(const std::filesystem::path &path) {
    return path.lexically_normal().generic_string();
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

} // namespace

const char *textureSemanticName(TextureSemantic semantic) {
    switch (semantic) {
    case TextureSemantic::SrgbColor:
        return "srgb-color";
    case TextureSemantic::Normal:
        return "normal";
    case TextureSemantic::LinearData:
    default:
        return "linear-data";
    }
}

const char *derivedMipmapWrapName(DerivedMipmapWrap wrap) {
    switch (wrap) {
    case DerivedMipmapWrap::Repeat:
        return "repeat";
    case DerivedMipmapWrap::Reflect:
        return "reflect";
    case DerivedMipmapWrap::Clamp:
    default:
        return "clamp";
    }
}

const char *textureEncoderName(TextureEncoder encoder) {
    return encoder == TextureEncoder::Bc7 ? "bc7" : "uastc";
}

const char *derivedTexturePayloadKindName(DerivedTexturePayloadKind kind) {
    return kind == DerivedTexturePayloadKind::NativeBc7 ? "native-bc7"
                                                        : "basis-uastc";
}

std::optional<TextureSemantic>
textureSemanticFromName(const std::string &name) {
    if (name == "srgb-color")
        return TextureSemantic::SrgbColor;
    if (name == "linear-data")
        return TextureSemantic::LinearData;
    if (name == "normal")
        return TextureSemantic::Normal;
    return std::nullopt;
}

std::optional<DerivedMipmapWrap>
derivedMipmapWrapFromName(const std::string &name) {
    if (name == "clamp")
        return DerivedMipmapWrap::Clamp;
    if (name == "repeat")
        return DerivedMipmapWrap::Repeat;
    if (name == "reflect")
        return DerivedMipmapWrap::Reflect;
    return std::nullopt;
}

std::optional<TextureEncoder> textureEncoderFromName(const std::string &name) {
    if (name == "uastc")
        return TextureEncoder::Uastc;
    if (name == "bc7")
        return TextureEncoder::Bc7;
    return std::nullopt;
}

std::optional<DerivedTexturePayloadKind>
derivedTexturePayloadKindFromName(const std::string &name) {
    if (name == "basis-uastc")
        return DerivedTexturePayloadKind::BasisUastc;
    if (name == "native-bc7")
        return DerivedTexturePayloadKind::NativeBc7;
    return std::nullopt;
}

const DerivedTextureEntry *
DerivedTextureManifest::find(int imageIndex, TextureSemantic semantic,
                             DerivedMipmapWrap wrap) const {
    for (const DerivedTextureEntry &entry : entries) {
        if (entry.imageIndex == imageIndex && entry.semantic == semantic &&
            entry.mipWrap == wrap) {
            return &entry;
        }
    }
    return nullptr;
}

std::string normalizedSceneKey(const std::filesystem::path &scenePath) {
    const std::string value = genericPath(scenePath);
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : value) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<unsigned char>(c - 'A' + 'a');
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::filesystem::path derivedManifestPath(
    const std::filesystem::path &cacheRoot,
    const std::filesystem::path &scenePath, uint32_t textureLimit) {
    const std::string profile =
        textureLimit == 0 ? "full" : std::to_string(textureLimit);
    return cacheRoot / "manifests" / normalizedSceneKey(scenePath) /
           (profile + ".json");
}

std::filesystem::path derivedManifestPath(
    const std::filesystem::path &cacheRoot, const std::string &sceneId,
    const std::string &profileId) {
    return cacheRoot / "manifests" / sceneId / (profileId + ".json");
}

DerivedFileStamp fileStamp(const std::filesystem::path &path,
                          const std::string &sha256) {
    DerivedFileStamp stamp;
    stamp.path = genericPath(path);
    std::error_code error;
    stamp.size = std::filesystem::file_size(path, error);
    if (error)
        stamp.size = 0;
    error.clear();
    const auto time = std::filesystem::last_write_time(path, error);
    stamp.writeTime = error ? 0 : time.time_since_epoch().count();
    stamp.sha256 = sha256;
    return stamp;
}

bool fileStampMatches(const DerivedFileStamp &stamp,
                      const std::filesystem::path &sceneDirectory) {
    if (stamp.path.empty())
        return false;
    std::filesystem::path path(stamp.path);
    if (!path.is_absolute())
        path = sceneDirectory / path;
    const DerivedFileStamp current = fileStamp(path);
    return current.size == stamp.size && current.writeTime == stamp.writeTime;
}

bool loadDerivedTextureManifest(const std::filesystem::path &path,
                                DerivedTextureManifest &manifest,
                                std::string &error) {
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
        manifest.projectId = root.value("projectId", std::string{});
        manifest.sceneId = root.value("sceneId", std::string{});
        manifest.profileId = root.value("profileId", std::string{});
        manifest.scenePath = root.value("scenePath", std::string{});
        manifest.qualityPreset =
            root.value("qualityPreset", std::string("development"));
        manifest.encoderSettings = root.value("encoderSettings", std::string{});
        manifest.textureLimit = root.value("textureLimit", 0u);
        manifest.scene = stampFromJson(root.at("scene"));
        if (manifest.schemaVersion != DerivedTextureManifest::kSchemaVersion &&
            manifest.schemaVersion !=
                DerivedTextureManifest::kUastcSchemaVersion &&
            manifest.schemaVersion !=
                DerivedTextureManifest::kLegacySchemaVersion) {
            error = "unsupported manifest schema";
            return false;
        }
        if (manifest.schemaVersion >=
                DerivedTextureManifest::kUastcSchemaVersion &&
            (manifest.projectId.empty() || manifest.sceneId.empty() ||
             manifest.profileId.empty())) {
            error = "manifest identity is incomplete";
            return false;
        }
        if (manifest.schemaVersion == DerivedTextureManifest::kSchemaVersion) {
            const Json &encoder = root.at("encoder");
            const auto parsedEncoder = textureEncoderFromName(
                encoder.value("codec", std::string{}));
            if (!parsedEncoder) {
                error = "manifest contains an unknown texture encoder";
                return false;
            }
            manifest.textureEncoder = *parsedEncoder;
            manifest.encoderName =
                encoder.value("name", std::string{});
            manifest.encoderVersion =
                encoder.value("version", std::string{});
        } else {
            manifest.textureEncoder = TextureEncoder::Uastc;
            manifest.encoderName = "ktx create";
            manifest.encoderVersion = "4.4.2";
        }
        for (const Json &item : root.at("entries")) {
            const auto semantic = textureSemanticFromName(
                item.value("semantic", std::string{}));
            const auto wrap = derivedMipmapWrapFromName(
                item.value("mipWrap", std::string{}));
            if (!semantic || !wrap) {
                error = "manifest contains an unknown texture semantic";
                return false;
            }
            DerivedTextureEntry entry;
            entry.imageIndex = item.value("imageIndex", -1);
            entry.semantic = *semantic;
            entry.mipWrap = *wrap;
            entry.width = item.value("width", 0u);
            entry.height = item.value("height", 0u);
            if (manifest.schemaVersion ==
                DerivedTextureManifest::kSchemaVersion) {
                const auto payload = derivedTexturePayloadKindFromName(
                    item.value("payloadKind", std::string{}));
                if (!payload) {
                    error = "manifest contains an unknown texture payload";
                    return false;
                }
                entry.payloadKind = *payload;
                entry.vkFormat = item.value("vkFormat", 0u);
                entry.mipLevels = item.value("mipLevels", 0u);
                entry.payloadBytes = item.value("payloadBytes", uint64_t{0});
                entry.blobBytes = item.value("blobBytes", uint64_t{0});
                entry.supercompression =
                    item.value("supercompression", std::string("none"));
            } else {
                entry.payloadKind =
                    DerivedTexturePayloadKind::BasisUastc;
                entry.supercompression = "zstd";
            }
            entry.cacheKey = item.value("cacheKey", std::string{});
            entry.blob = item.value("blob", std::string{});
            entry.source = stampFromJson(item.at("source"));
            manifest.entries.push_back(std::move(entry));
        }
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

bool saveDerivedTextureManifest(const std::filesystem::path &path,
                                const DerivedTextureManifest &manifest,
                                std::string &error) {
    std::filesystem::path temporary;
    try {
        Json root{{"schemaVersion", manifest.schemaVersion},
                  {"projectId", manifest.projectId},
                  {"sceneId", manifest.sceneId},
                  {"profileId", manifest.profileId},
                  {"scenePath", manifest.scenePath},
                  {"qualityPreset", manifest.qualityPreset},
                  {"encoderSettings", manifest.encoderSettings},
                  {"textureLimit", manifest.textureLimit},
                  {"scene", stampToJson(manifest.scene)},
                  {"encoder",
                    {{"name", manifest.encoderName},
                     {"version", manifest.encoderVersion},
                     {"codec", textureEncoderName(manifest.textureEncoder)},
                     {"preset", manifest.qualityPreset},
                     {"settings", manifest.encoderSettings}}},
                  {"entries", Json::array()}};
        for (const DerivedTextureEntry &entry : manifest.entries) {
            root["entries"].push_back(
                {{"imageIndex", entry.imageIndex},
                 {"semantic", textureSemanticName(entry.semantic)},
                 {"mipWrap", derivedMipmapWrapName(entry.mipWrap)},
                 {"width", entry.width},
                 {"height", entry.height},
                 {"payloadKind",
                  derivedTexturePayloadKindName(entry.payloadKind)},
                 {"vkFormat", entry.vkFormat},
                 {"mipLevels", entry.mipLevels},
                 {"payloadBytes", entry.payloadBytes},
                 {"blobBytes", entry.blobBytes},
                 {"supercompression", entry.supercompression},
                 {"cacheKey", entry.cacheKey},
                 {"blob", entry.blob},
                 {"source", stampToJson(entry.source)}});
        }
        std::filesystem::create_directories(path.parent_path());
        static std::atomic<uint64_t> temporaryCounter{0};
        temporary = path.string() + ".tmp-" +
                    std::to_string(temporaryCounter.fetch_add(1));
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error("could not create manifest");
            output << root.dump(2) << '\n';
            output.flush();
            if (!output)
                throw std::runtime_error("could not flush manifest");
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD moveError = GetLastError();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw std::system_error(static_cast<int>(moveError),
                                    std::system_category(),
                                    "could not publish manifest");
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
