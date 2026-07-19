#include "TextureCacheBuilder.h"
#include "ProcessRunner.h"
#include "TextureCachePipeline.h"

#include "assets/DerivedAssetPaths.h"
#include "assets/SceneCatalog.h"

#include "assets/DerivedTextureManifest.h"

#include <json.hpp>
#include <ktx.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <tiny_gltf_v3.h>

#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vkr::assettool {
namespace {

struct EncoderPreset {
    const char *name = "development";
    const char *settings = nullptr;
    const wchar_t *quality = L"2";
    const wchar_t *zstd = L"9";
    bool rdo = true;
};

const EncoderPreset &encoderPreset(const std::string &name) {
    static const EncoderPreset development{
        "development",
        "ktx-4.4.2|uastc|quality=2|rdo=1|zstd=9|mips=lanczos4|tf=semantic",
        L"2", L"9", true};
    static const EncoderPreset production{
        "production",
        "ktx-4.4.2|uastc|quality=4|rdo=1|zstd=18|mips=lanczos4|tf=semantic",
        L"4", L"18", true};
    if (name == development.name)
        return development;
    if (name == production.name)
        return production;
    throw std::invalid_argument(
        "texture preset must be development or production");
};

struct ScopedTemporaryFile {
    std::filesystem::path path;
    ~ScopedTemporaryFile() {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};

struct ImageSource {
    std::vector<uint8_t> bytes;
    std::filesystem::path externalPath;
    DerivedFileStamp stamp;
    uint32_t width = 0;
    uint32_t height = 0;
    bool png = false;
};

struct BuildTask {
    int32_t imageIndex = -1;
    TextureSemantic semantic = TextureSemantic::LinearData;
    DerivedMipmapWrap wrap = DerivedMipmapWrap::Clamp;
};

struct ScannedTask {
    BuildTask sourceTask;
    DerivedFileStamp sourceStamp;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint64_t estimatedMemoryBytes = 0;
    std::string cacheKey;
    std::filesystem::path blob;
    bool reused = false;
    size_t ownerIndex = SIZE_MAX;
};

std::string toString(tg3_str value) {
    return value.data && value.len ? std::string(value.data, value.len)
                                   : std::string{};
}

std::string genericPath(const std::filesystem::path &path) {
    return path.lexically_normal().generic_string();
}

std::vector<uint8_t> readFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("could not open file: " + path.string());
    const std::streamoff end = input.tellg();
    if (end < 0)
        throw std::runtime_error("could not determine file size: " +
                                 path.string());
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty() &&
        !input.read(reinterpret_cast<char *>(bytes.data()), end)) {
        throw std::runtime_error("could not read file: " + path.string());
    }
    return bytes;
}

std::string hexString(const uint8_t *bytes, size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i)
        output << std::setw(2) << static_cast<unsigned>(bytes[i]);
    return output.str();
}

std::string sha256(const uint8_t *data, size_t size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<uint8_t> object;
    std::vector<uint8_t> digest;

    const auto cleanup = [&] {
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
    };

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        cleanup();
        throw std::runtime_error("BCryptOpenAlgorithmProvider(SHA-256) failed");
    }

    DWORD objectLength = 0;
    DWORD digestLength = 0;
    DWORD resultLength = 0;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&objectLength),
                               sizeof(objectLength), &resultLength, 0);
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                   reinterpret_cast<PUCHAR>(&digestLength),
                                   sizeof(digestLength), &resultLength, 0);
    }
    if (!BCRYPT_SUCCESS(status)) {
        cleanup();
        throw std::runtime_error("BCryptGetProperty(SHA-256) failed");
    }

    object.resize(objectLength);
    digest.resize(digestLength);
    status = BCryptCreateHash(algorithm, &hash, object.data(), objectLength,
                              nullptr, 0, 0);
    if (BCRYPT_SUCCESS(status) && size != 0) {
        size_t offset = 0;
        while (offset < size && BCRYPT_SUCCESS(status)) {
            const ULONG chunk =
                static_cast<ULONG>(std::min<size_t>(size - offset, ULONG_MAX));
            status = BCryptHashData(hash, const_cast<PUCHAR>(data + offset),
                                    chunk, 0);
            offset += chunk;
        }
    }
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptFinishHash(hash, digest.data(), digestLength, 0);
    }
    if (!BCRYPT_SUCCESS(status)) {
        cleanup();
        throw std::runtime_error("BCrypt SHA-256 calculation failed");
    }
    cleanup();
    return hexString(digest.data(), digest.size());
}

std::string sha256(const std::vector<uint8_t> &bytes) {
    return sha256(bytes.data(), bytes.size());
}

std::string sha256(const std::string &text) {
    return sha256(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

bool hasPngSignature(const std::vector<uint8_t> &bytes) {
    static constexpr std::array<uint8_t, 8> signature{0x89, 'P',  'N',  'G',
                                                      0x0d, 0x0a, 0x1a, 0x0a};
    return bytes.size() >= signature.size() &&
           std::equal(signature.begin(), signature.end(), bytes.begin());
}

std::vector<uint8_t> decodeBase64(const std::string &input) {
    static const std::array<int8_t, 256> makeTable = [] {
        std::array<int8_t, 256> result{};
        result.fill(-1);
        for (int i = 0; i < 26; ++i) {
            result[static_cast<size_t>('A' + i)] = static_cast<int8_t>(i);
            result[static_cast<size_t>('a' + i)] = static_cast<int8_t>(26 + i);
        }
        for (int i = 0; i < 10; ++i)
            result[static_cast<size_t>('0' + i)] = static_cast<int8_t>(52 + i);
        result[static_cast<size_t>('+')] = 62;
        result[static_cast<size_t>('/')] = 63;
        return result;
    }();

    std::vector<uint8_t> output;
    uint32_t accumulator = 0;
    int bits = 0;
    for (unsigned char c : input) {
        if (c == '=')
            break;
        if (std::isspace(c))
            continue;
        const int value = makeTable[c];
        if (value < 0)
            throw std::runtime_error("invalid base64 image URI");
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(
                static_cast<uint8_t>((accumulator >> bits) & 0xffu));
        }
    }
    return output;
}

std::vector<uint8_t> decodeDataUri(const std::string &uri) {
    const size_t comma = uri.find(',');
    if (comma == std::string::npos)
        throw std::runtime_error("malformed image data URI");
    const std::string metadata = uri.substr(0, comma);
    if (metadata.find(";base64") == std::string::npos)
        throw std::runtime_error("non-base64 image data URI is unsupported");
    return decodeBase64(uri.substr(comma + 1));
}

bool isRemoteUri(const std::string &uri) {
    return uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0;
}

DerivedMipmapWrap samplerWrap(const tg3_model *model,
                              const tg3_texture &texture) {
    int32_t wrapS = TG3_TEXTURE_WRAP_REPEAT;
    int32_t wrapT = TG3_TEXTURE_WRAP_REPEAT;
    if (texture.sampler >= 0 &&
        texture.sampler < static_cast<int32_t>(model->samplers_count)) {
        const tg3_sampler &sampler = model->samplers[texture.sampler];
        wrapS = sampler.wrap_s;
        wrapT = sampler.wrap_t;
    }
    if (wrapS == TG3_TEXTURE_WRAP_MIRRORED_REPEAT &&
        wrapT == TG3_TEXTURE_WRAP_MIRRORED_REPEAT)
        return DerivedMipmapWrap::Reflect;
    if (wrapS == TG3_TEXTURE_WRAP_REPEAT && wrapT == TG3_TEXTURE_WRAP_REPEAT)
        return DerivedMipmapWrap::Repeat;
    return DerivedMipmapWrap::Clamp;
}

const char *ktxWrapName(DerivedMipmapWrap wrap) {
    switch (wrap) {
    case DerivedMipmapWrap::Repeat:
        return "wrap";
    case DerivedMipmapWrap::Reflect:
        return "reflect";
    case DerivedMipmapWrap::Clamp:
    default:
        return "clamp";
    }
}

std::pair<uint32_t, uint32_t> limitedExtent(uint32_t width, uint32_t height,
                                            uint32_t limit) {
    if (limit == 0 || std::max(width, height) <= limit)
        return {width, height};
    const double scale = static_cast<double>(limit) /
                         static_cast<double>(std::max(width, height));
    return {std::max(1u, static_cast<uint32_t>(std::lround(width * scale))),
            std::max(1u, static_cast<uint32_t>(std::lround(height * scale)))};
}

std::filesystem::path makeTemporaryPath(const std::filesystem::path &directory,
                                        const std::string &suffix) {
    static std::atomic<uint64_t> counter{0};
    return directory / (".vulkanlab-" + std::to_string(GetCurrentProcessId()) +
                        "-" + std::to_string(counter.fetch_add(1)) + suffix);
}

std::filesystem::path findKtxTool(const std::filesystem::path &requested) {
    if (!requested.empty()) {
        const std::filesystem::path absolute =
            std::filesystem::absolute(requested);
        if (!std::filesystem::exists(absolute))
            throw std::runtime_error("ktx tool not found: " +
                                     absolute.string());
        return absolute;
    }

    std::array<wchar_t, 32768> modulePath{};
    const DWORD moduleLength = GetModuleFileNameW(
        nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (moduleLength > 0 && moduleLength < modulePath.size()) {
        const std::filesystem::path sibling =
            std::filesystem::path(modulePath.data()).parent_path() / L"ktx.exe";
        if (std::filesystem::exists(sibling))
            return sibling;
    }

    std::array<wchar_t, 32768> resolved{};
    const DWORD length = SearchPathW(nullptr, L"ktx.exe", nullptr,
                                     static_cast<DWORD>(resolved.size()),
                                     resolved.data(), nullptr);
    if (length > 0 && length < resolved.size())
        return std::filesystem::path(resolved.data());

    throw std::runtime_error(
        "ktx.exe was not found beside VulkanLabAssetTool or on PATH; "
        "provide --ktx-tool <path>");
}

void validateKtx2(const std::filesystem::path &path, uint32_t expectedWidth,
                  uint32_t expectedHeight) {
    ktxTexture *texture = nullptr;
    const KTX_error_code result = ktxTexture_CreateFromNamedFile(
        path.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &texture);
    if (result != KTX_SUCCESS || !texture)
        throw std::runtime_error("generated KTX2 is invalid: " + path.string() +
                                 " (" + ktxErrorString(result) + ")");
    const bool valid = texture->classId == ktxTexture2_c &&
                       texture->baseWidth == expectedWidth &&
                       texture->baseHeight == expectedHeight &&
                       texture->numLevels != 0;
    ktxTexture_Destroy(texture);
    if (!valid) {
        throw std::runtime_error(
            "generated KTX2 has unexpected type or dimensions: " +
            path.string());
    }
}

void atomicReplace(const std::filesystem::path &source,
                   const std::filesystem::path &destination) {
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not publish KTX2 blob");
    }
}

ImageSource loadImageSource(const tg3_model *model, int32_t imageIndex,
                            const std::filesystem::path &scenePath) {
    if (imageIndex < 0 ||
        imageIndex >= static_cast<int32_t>(model->images_count))
        throw std::runtime_error("invalid glTF image index");

    const tg3_image &image = model->images[imageIndex];
    ImageSource result;
    const std::filesystem::path sceneDirectory = scenePath.parent_path();
    const std::string uri = toString(image.uri);

    if (image.buffer_view >= 0 &&
        image.buffer_view < static_cast<int32_t>(model->buffer_views_count)) {
        const tg3_buffer_view &view = model->buffer_views[image.buffer_view];
        if (view.buffer < 0 ||
            view.buffer >= static_cast<int32_t>(model->buffers_count))
            throw std::runtime_error(
                "image bufferView references invalid buffer");
        const tg3_buffer &buffer = model->buffers[view.buffer];
        if (view.byte_offset + view.byte_length > buffer.data.count)
            throw std::runtime_error("image bufferView is out of range");
        const uint8_t *begin = buffer.data.data + view.byte_offset;
        result.bytes.assign(begin, begin + view.byte_length);
    } else if (image.image.data && image.image.count > 0) {
        result.bytes.assign(image.image.data,
                            image.image.data + image.image.count);
    } else if (uri.rfind("data:", 0) == 0) {
        result.bytes = decodeDataUri(uri);
    } else {
        if (uri.empty())
            throw std::runtime_error("image has no URI or embedded data");
        if (isRemoteUri(uri))
            throw std::runtime_error("remote image URI is unsupported: " + uri);
        result.externalPath = std::filesystem::path(uri).is_absolute()
                                  ? std::filesystem::path(uri)
                                  : sceneDirectory / std::filesystem::path(uri);
        result.externalPath =
            std::filesystem::absolute(result.externalPath).lexically_normal();
        result.bytes = readFile(result.externalPath);
    }

    int width = 0;
    int height = 0;
    int components = 0;
    if (result.bytes.empty() ||
        !stbi_info_from_memory(result.bytes.data(),
                               static_cast<int>(result.bytes.size()), &width,
                               &height, &components) ||
        width <= 0 || height <= 0) {
        throw std::runtime_error("could not inspect glTF image[" +
                                 std::to_string(imageIndex) + "]");
    }
    result.width = static_cast<uint32_t>(width);
    result.height = static_cast<uint32_t>(height);
    result.png = hasPngSignature(result.bytes);

    const std::string sourceHash = sha256(result.bytes);
    if (!result.externalPath.empty()) {
        result.stamp = fileStamp(result.externalPath, sourceHash);
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(
            result.externalPath, sceneDirectory, relativeError);
        result.stamp.path =
            genericPath(relativeError ? result.externalPath : relative);
    } else {
        result.stamp = fileStamp(scenePath, sourceHash);
        result.stamp.path = scenePath.filename().generic_string();
    }
    return result;
}

std::filesystem::path
writeTemporaryPng(const ImageSource &source,
                  const std::filesystem::path &directory) {
    int width = 0;
    int height = 0;
    int components = 0;
    stbi_uc *pixels = stbi_load_from_memory(
        source.bytes.data(), static_cast<int>(source.bytes.size()), &width,
        &height, &components, STBI_rgb_alpha);
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> owner(
        pixels, &stbi_image_free);
    if (!pixels)
        throw std::runtime_error("could not decode image for temporary PNG");
    const std::filesystem::path path =
        makeTemporaryPath(directory, ".input.png");
    if (!stbi_write_png(path.string().c_str(), width, height, 4, pixels,
                        width * 4)) {
        throw std::runtime_error("could not write temporary PNG: " +
                                 path.string());
    }
    return path;
}

std::vector<BuildTask> collectTasks(const tg3_model *model) {
    std::map<std::tuple<int32_t, TextureSemantic, DerivedMipmapWrap>, BuildTask>
        unique;
    const auto addTexture = [&](int32_t textureIndex,
                                TextureSemantic semantic) {
        if (textureIndex < 0 ||
            textureIndex >= static_cast<int32_t>(model->textures_count))
            return;
        const tg3_texture &texture = model->textures[textureIndex];
        if (texture.source < 0 ||
            texture.source >= static_cast<int32_t>(model->images_count))
            return;
        const DerivedMipmapWrap wrap = samplerWrap(model, texture);
        const auto key = std::make_tuple(texture.source, semantic, wrap);
        unique.emplace(key, BuildTask{texture.source, semantic, wrap});
    };

    for (uint32_t i = 0; i < model->materials_count; ++i) {
        const tg3_material &material = model->materials[i];
        addTexture(material.pbr_metallic_roughness.base_color_texture.index,
                   TextureSemantic::SrgbColor);
        addTexture(material.emissive_texture.index, TextureSemantic::SrgbColor);
        addTexture(material.normal_texture.index, TextureSemantic::Normal);
        addTexture(
            material.pbr_metallic_roughness.metallic_roughness_texture.index,
            TextureSemantic::LinearData);
        addTexture(material.occlusion_texture.index,
                   TextureSemantic::LinearData);
    }

    std::vector<BuildTask> tasks;
    tasks.reserve(unique.size());
    for (const auto &item : unique)
        tasks.push_back(item.second);
    return tasks;
}

uint64_t estimateTaskMemory(const ImageSource &source, uint32_t outputWidth,
                            uint32_t outputHeight) {
    constexpr uint64_t minimum = 64ull * 1024ull * 1024ull;
    const uint64_t sourcePixels =
        static_cast<uint64_t>(source.width) * source.height;
    const uint64_t outputPixels =
        static_cast<uint64_t>(outputWidth) * outputHeight;
    // KTX may retain decoded source, resize, mip and encoder working buffers.
    return minimum + sourcePixels * 16ull + outputPixels * 8ull +
           static_cast<uint64_t>(source.bytes.size());
}

std::vector<std::wstring>
makeKtxArguments(const ScannedTask &task, const EncoderPreset &preset,
                 const std::filesystem::path &input,
                 const std::filesystem::path &output) {
    std::vector<std::wstring> arguments{
        L"create",
        L"--format",
        task.sourceTask.semantic == TextureSemantic::SrgbColor
            ? L"R8G8B8A8_SRGB"
            : L"R8G8B8A8_UNORM",
        L"--assign-tf",
        task.sourceTask.semantic == TextureSemantic::SrgbColor ? L"srgb"
                                                               : L"linear",
        L"--encode",
        L"uastc",
        L"--uastc-quality",
        preset.quality};
    if (preset.rdo) {
        arguments.push_back(L"--uastc-rdo");
        arguments.push_back(L"--uastc-rdo-l");
        arguments.push_back(task.sourceTask.semantic == TextureSemantic::Normal
                                ? L"0.5"
                                : L"1.0");
    }
    arguments.insert(arguments.end(),
                     {L"--zstd", preset.zstd, L"--generate-mipmap",
                      L"--mipmap-filter", L"lanczos4", L"--mipmap-wrap",
                      std::wstring(ktxWrapName(task.sourceTask.wrap),
                                   ktxWrapName(task.sourceTask.wrap) +
                                       std::char_traits<char>::length(
                                           ktxWrapName(task.sourceTask.wrap))),
                      L"--width", std::to_wstring(task.outputWidth),
                      L"--height", std::to_wstring(task.outputHeight)});
    if (task.sourceTask.semantic == TextureSemantic::Normal)
        arguments.push_back(L"--normalize");
    arguments.push_back(input.wstring());
    arguments.push_back(output.wstring());
    return arguments;
}

class ProgressReporter {
  public:
    ProgressReporter(bool ndjson, std::string sceneId, std::string profileId,
                     size_t total, uint32_t workers, uint64_t budgetMiB,
                     std::chrono::steady_clock::time_point begin)
        : ndjson_(ndjson), sceneId_(std::move(sceneId)),
          profileId_(std::move(profileId)), total_(total), workers_(workers),
          budgetMiB_(budgetMiB), begin_(begin),
          taskId_(std::to_string(GetCurrentProcessId()) + "-" +
                  std::to_string(std::chrono::steady_clock::now()
                                     .time_since_epoch()
                                     .count())) {}

    void started(size_t reused) {
        std::lock_guard lock(mutex_);
        reused_ = reused;
        if (ndjson_) {
            emitJson({{"event", "started"},
                      {"protocolVersion", 1},
                      {"taskId", taskId_},
                      {"scene", sceneId_},
                      {"profile", profileId_},
                      {"total", total_},
                      {"reused", reused_},
                      {"workers", workers_},
                      {"memoryBudgetMiB", budgetMiB_}});
        }
    }

    void schedulerEvent(const char *event, const TextureBuildWorkItem &item,
                        const TextureBuildWorkResult *result) {
        std::lock_guard lock(mutex_);
        if (result) {
            if (result->success)
                ++encoded_;
            else
                ++failed_;
        }
        if (ndjson_) {
            nlohmann::json value{{"event", event},
                                 {"taskId", taskId_},
                                 {"index", item.index},
                                 {"image", item.imageIndex},
                                 {"semantic", item.semantic},
                                 {"estimatedBytes", item.estimatedMemoryBytes}};
            if (result) {
                value["durationMs"] = result->durationMs;
                value["exitCode"] = result->exitCode;
                if (!result->error.empty())
                    value["message"] = result->error;
            }
            emitJson(value);
            if (result) {
                emitJson({{"event", "progress"},
                          {"taskId", taskId_},
                          {"completed", encoded_ + reused_ + failed_},
                          {"reused", reused_},
                          {"encoded", encoded_},
                          {"failed", failed_}});
            }
        } else if (std::string(event) == "artifact_started") {
            std::cout << "  [" << (item.index + 1) << '/' << total_
                      << "] image " << item.imageIndex << ' ' << item.semantic
                      << '\n';
        }
    }

    void finished(const char *event, const std::filesystem::path &manifest = {},
                  const TextureBuildScheduleStats &schedule = {}) {
        std::lock_guard lock(mutex_);
        const double durationMs = elapsedMs();
        if (ndjson_) {
            nlohmann::json value{
                {"event", event},
                {"taskId", taskId_},
                {"scene", sceneId_},
                {"profile", profileId_},
                {"encoded", encoded_},
                {"reused", reused_},
                {"failed", failed_},
                {"durationMs", durationMs},
                {"peakWorkers", schedule.peakWorkers},
                {"peakReservedBytes", schedule.peakReservedBytes}};
            if (!manifest.empty())
                value["manifest"] = manifest.generic_string();
            emitJson(value);
        }
    }

    void publishing(const std::filesystem::path &manifest) {
        std::lock_guard lock(mutex_);
        if (ndjson_) {
            emitJson({{"event", "publishing"},
                      {"taskId", taskId_},
                      {"manifest", manifest.generic_string()}});
        }
    }

    size_t encoded() const { return encoded_; }
    size_t reused() const { return reused_; }
    double elapsedMs() const {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - begin_)
            .count();
    }

  private:
    void emitJson(const nlohmann::json &value) {
        std::cout << value.dump() << '\n';
        std::cout.flush();
    }

    bool ndjson_ = false;
    std::string sceneId_;
    std::string profileId_;
    size_t total_ = 0;
    uint32_t workers_ = 0;
    uint64_t budgetMiB_ = 0;
    std::string taskId_;
    std::chrono::steady_clock::time_point begin_;
    mutable std::mutex mutex_;
    size_t encoded_ = 0;
    size_t reused_ = 0;
    size_t failed_ = 0;
};

std::vector<ScannedTask>
scanTextureBuildPlan(const tg3_model *model,
                     const std::vector<BuildTask> &tasks,
                     const std::filesystem::path &scene, uint32_t textureLimit,
                     const std::filesystem::path &blobDirectory,
                     const EncoderPreset &preset, bool force) {
    std::vector<ScannedTask> result;
    result.reserve(tasks.size());
    for (const BuildTask &task : tasks) {
        ImageSource source = loadImageSource(model, task.imageIndex, scene);
        const auto [outputWidth, outputHeight] =
            limitedExtent(source.width, source.height, textureLimit);
        const std::string keyMaterial =
            std::string(preset.settings) + "|source=" + source.stamp.sha256 +
            "|semantic=" + textureSemanticName(task.semantic) +
            "|size=" + std::to_string(outputWidth) + "x" +
            std::to_string(outputHeight) +
            "|wrap=" + derivedMipmapWrapName(task.wrap);

        ScannedTask scanned;
        scanned.sourceTask = task;
        scanned.sourceStamp = std::move(source.stamp);
        scanned.sourceWidth = source.width;
        scanned.sourceHeight = source.height;
        scanned.outputWidth = outputWidth;
        scanned.outputHeight = outputHeight;
        scanned.estimatedMemoryBytes =
            estimateTaskMemory(source, outputWidth, outputHeight);
        scanned.cacheKey = sha256(keyMaterial);
        scanned.blob = blobDirectory / (scanned.cacheKey + ".ktx2");
        if (!force && std::filesystem::is_regular_file(scanned.blob)) {
            try {
                validateKtx2(scanned.blob, outputWidth, outputHeight);
                scanned.reused = true;
            } catch (const std::exception &exception) {
                std::cerr << "  invalid cached blob, rebuilding: "
                          << exception.what() << '\n';
            }
        }
        result.push_back(std::move(scanned));
    }
    return result;
}

void appendManifestEntries(DerivedTextureManifest &manifest,
                           const std::vector<ScannedTask> &tasks,
                           const std::filesystem::path &cacheRoot) {
    manifest.entries.reserve(tasks.size());
    for (const ScannedTask &task : tasks) {
        DerivedTextureEntry entry;
        entry.imageIndex = task.sourceTask.imageIndex;
        entry.semantic = task.sourceTask.semantic;
        entry.mipWrap = task.sourceTask.wrap;
        entry.width = task.outputWidth;
        entry.height = task.outputHeight;
        entry.cacheKey = task.cacheKey;
        entry.blob =
            genericPath(std::filesystem::relative(task.blob, cacheRoot));
        entry.source = task.sourceStamp;
        manifest.entries.push_back(std::move(entry));
    }
}

void printParseErrors(const tinygltf3::ErrorStack &errors) {
    for (uint32_t i = 0; i < errors.count(); ++i) {
        const tg3_error_entry *entry = errors.entry(i);
        if (entry && entry->message)
            std::cerr << "  " << entry->message << '\n';
    }
}

} // namespace

int buildTextureCache(const TextureCacheBuildOptions &options) {
    Win32JobProcessRunner processRunner;
    return buildTextureCache(options, processRunner);
}

int buildTextureCache(const TextureCacheBuildOptions &options,
                      IProcessRunner &processRunner) {
    const auto buildBegin = std::chrono::steady_clock::now();
    if (options.projectId.empty() || options.sceneId.empty() ||
        options.profileId.empty())
        throw std::invalid_argument(
            "projectId, sceneId, and profileId are required");
    const EncoderPreset &preset = encoderPreset(options.qualityPreset);
    const uint32_t defaultWorkers = std::max(
        1u,
        std::min(4u, std::max(1u, std::thread::hardware_concurrency() / 2)));
    const uint32_t maxWorkers =
        options.maxWorkers == 0 ? defaultWorkers : options.maxWorkers;
    if (maxWorkers == 0 || maxWorkers > 64)
        throw std::invalid_argument("workers must be between 1 and 64");
    if (options.memoryBudgetMiB == 0)
        throw std::invalid_argument("memory budget must be greater than zero");

    std::atomic_bool localCancellation{false};
    std::atomic_bool &cancelRequested =
        options.cancelRequested ? *options.cancelRequested : localCancellation;
    const std::filesystem::path sceneIdentity =
        (options.sceneProjectPath.empty() ? options.scene
                                          : options.sceneProjectPath)
            .lexically_normal();
    const std::filesystem::path scene =
        std::filesystem::absolute(options.scene).lexically_normal();
    if (!std::filesystem::is_regular_file(scene))
        throw std::runtime_error("scene not found: " + scene.string());

    const std::filesystem::path cacheRoot =
        std::filesystem::absolute(options.cacheRoot).lexically_normal();
    const std::filesystem::path blobDirectory = cacheRoot / "blobs";
    std::filesystem::create_directories(blobDirectory);
    const std::filesystem::path ktxTool = findKtxTool(options.ktxTool);

    tinygltf3::Model model;
    tinygltf3::ErrorStack errors;
    tg3_parse_options parseOptions;
    tg3_parse_options_init(&parseOptions);
    parseOptions.images_as_is = 1;
    const tg3_error_code parseResult = tinygltf3::parse_file(
        model, errors, scene.string().c_str(), &parseOptions);
    if (parseResult != TG3_OK || errors.has_error()) {
        std::cerr << "Failed to parse glTF scene:\n";
        printParseErrors(errors);
        throw std::runtime_error("glTF parse failed: " + scene.string());
    }

    const std::vector<BuildTask> sourceTasks = collectTasks(model.get());
    const std::vector<ScannedTask> tasks = scanTextureBuildPlan(
        model.get(), sourceTasks, scene, options.textureLimit, blobDirectory,
        preset, options.force);
    DerivedTextureManifest manifest;
    manifest.projectId = options.projectId;
    manifest.sceneId = options.sceneId;
    manifest.profileId = options.profileId;
    manifest.scenePath = genericPath(sceneIdentity);
    manifest.qualityPreset = preset.name;
    manifest.encoderSettings = preset.settings;
    manifest.textureLimit = options.textureLimit;
    const std::vector<uint8_t> sceneBytes = readFile(scene);
    manifest.scene = fileStamp(scene, sha256(sceneBytes));
    manifest.scene.path = scene.filename().generic_string();
    size_t reusedCount = 0;
    std::vector<TextureBuildWorkItem> workItems;
    workItems.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i].reused) {
            ++reusedCount;
            continue;
        }
        workItems.push_back({i, tasks[i].sourceTask.imageIndex,
                             textureSemanticName(tasks[i].sourceTask.semantic),
                             tasks[i].estimatedMemoryBytes});
    }

    ProgressReporter reporter(options.progressNdjson, options.sceneId,
                              options.profileId, tasks.size(), maxWorkers,
                              options.memoryBudgetMiB, buildBegin);
    if (!options.progressNdjson) {
        std::cout << "Building texture cache for " << scene.string() << '\n'
                  << "  profile: " << options.profileId << " (" << preset.name
                  << ")\n  textures: " << tasks.size()
                  << "\n  workers: " << maxWorkers
                  << "\n  memory budget: " << options.memoryBudgetMiB
                  << " MiB\n  ktx tool: " << ktxTool.string() << '\n';
    }
    reporter.started(reusedCount);

    std::mutex childOutputMutex;
    const TextureBuildSchedulerOptions schedulerOptions{
        maxWorkers, options.memoryBudgetMiB * 1024ull * 1024ull};
    TextureBuildScheduleStats scheduleStats;
    const auto results = executeTextureBuildPlan(
        workItems, schedulerOptions, cancelRequested,
        [&](const TextureBuildWorkItem &work, const std::atomic_bool &cancel) {
            const auto begin = std::chrono::steady_clock::now();
            TextureBuildWorkResult result;
            result.index = work.index;
            const ScannedTask &task = tasks[work.index];
            if (cancel.load()) {
                result.cancelled = true;
                result.error = "cancelled before encoding";
                return result;
            }
            try {
                ImageSource source = loadImageSource(
                    model.get(), task.sourceTask.imageIndex, scene);
                if (source.stamp.sha256 != task.sourceStamp.sha256)
                    throw std::runtime_error(
                        "source image changed while import was running");

                ScopedTemporaryFile temporaryInput;
                const std::filesystem::path input =
                    source.png && !source.externalPath.empty()
                        ? source.externalPath
                        : (temporaryInput.path =
                               writeTemporaryPng(source, blobDirectory));
                ScopedTemporaryFile temporaryOutput{
                    makeTemporaryPath(blobDirectory, ".output.ktx2")};
                const ProcessResult process = processRunner.run(
                    {ktxTool, makeKtxArguments(task, preset, input,
                                               temporaryOutput.path)},
                    cancel);
                if (!process.output.empty()) {
                    std::lock_guard outputLock(childOutputMutex);
                    std::cerr << process.output;
                    if (process.output.back() != '\n')
                        std::cerr << '\n';
                }
                result.exitCode = process.exitCode;
                result.cancelled = process.cancelled || cancel.load();
                if (result.cancelled) {
                    result.error = "texture encoding cancelled";
                } else if (process.exitCode != 0) {
                    result.error =
                        "ktx create failed for image " +
                        std::to_string(task.sourceTask.imageIndex) + " (" +
                        textureSemanticName(task.sourceTask.semantic) +
                        ") with exit code " + std::to_string(process.exitCode);
                } else {
                    validateKtx2(temporaryOutput.path, task.outputWidth,
                                 task.outputHeight);
                    if (cancel.load()) {
                        result.cancelled = true;
                        result.error = "texture encoding cancelled";
                    } else {
                        atomicReplace(temporaryOutput.path, task.blob);
                        temporaryOutput.path.clear();
                        result.success = true;
                    }
                }
            } catch (const std::exception &exception) {
                result.cancelled = cancel.load();
                result.error =
                    "image " + std::to_string(task.sourceTask.imageIndex) +
                    " (" + textureSemanticName(task.sourceTask.semantic) +
                    "), command exit code " + std::to_string(result.exitCode) +
                    ": " + exception.what();
            }
            result.durationMs = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - begin)
                                    .count();
            return result;
        },
        [&](const char *event, const TextureBuildWorkItem &item,
            const TextureBuildWorkResult *result) {
            reporter.schedulerEvent(event, item, result);
        },
        &scheduleStats);

    const TextureBuildWorkResult *failure = nullptr;
    for (const TextureBuildWorkResult &result : results) {
        if (!result.success && !result.cancelled) {
            failure = &result;
            break;
        }
    }
    if (cancelRequested.load()) {
        processRunner.cancelAll();
        if (failure) {
            reporter.finished("failed", {}, scheduleStats);
            throw std::runtime_error(failure->error);
        }
        reporter.finished("cancelled", {}, scheduleStats);
        return 130;
    }

    appendManifestEntries(manifest, tasks, cacheRoot);

    const std::filesystem::path manifestPath =
        derivedManifestPath(cacheRoot, options.sceneId, options.profileId);
    reporter.publishing(manifestPath);
    std::string saveError;
    if (!saveDerivedTextureManifest(manifestPath, manifest, saveError))
        throw std::runtime_error("could not publish manifest: " + saveError);

    reporter.finished("completed", manifestPath, scheduleStats);
    if (!options.progressNdjson) {
        std::cout << "Texture cache complete\n"
                  << "  encoded: " << reporter.encoded() << "\n"
                  << "  reused: " << reporter.reused() << "\n"
                  << "  duration: " << std::fixed << std::setprecision(2)
                  << reporter.elapsedMs() << " ms\n"
                  << "  peak workers: " << scheduleStats.peakWorkers << "\n"
                  << "  peak reserved: " << std::setprecision(2)
                  << (static_cast<double>(scheduleStats.peakReservedBytes) /
                      (1024.0 * 1024.0))
                  << " MiB\n"
                  << "  manifest: " << manifestPath.string() << '\n';
    }
    return 0;
}

int migrateTextureCache(const TextureCacheMigrationOptions &options) {
    const std::filesystem::path projectRoot =
        std::filesystem::absolute(options.projectRoot).lexically_normal();
    const SceneCatalog catalog =
        SceneCatalog::load(projectRoot / "assets/catalog.json", projectRoot);
    const std::filesystem::path legacyRoot =
        std::filesystem::absolute(options.legacyCacheRoot).lexically_normal();
    const std::filesystem::path targetRoot =
        std::filesystem::absolute(options.cacheRoot).lexically_normal();

    uint64_t migrated = 0;
    uint64_t copiedBlobs = 0;
    uint64_t skipped = 0;
    for (const CatalogScene &scene : catalog.scenes) {
        if (scene.type != "gltf")
            continue;
        const std::filesystem::path source = projectRoot / scene.source;
        if (!std::filesystem::is_regular_file(source)) {
            ++skipped;
            continue;
        }
        for (const auto &profilePair : catalog.importProfiles) {
            const ImportProfile &profile = profilePair.second;
            const std::filesystem::path legacyManifestPath =
                derivedManifestPath(legacyRoot, scene.source,
                                    profile.textureLimit);
            if (!std::filesystem::is_regular_file(legacyManifestPath))
                continue;

            DerivedTextureManifest manifest;
            std::string error;
            if (!loadDerivedTextureManifest(legacyManifestPath, manifest,
                                            error))
                throw std::runtime_error("could not read legacy manifest '" +
                                         legacyManifestPath.string() +
                                         "': " + error);
            if (manifest.schemaVersion !=
                DerivedTextureManifest::kLegacySchemaVersion)
                continue;
            manifest.schemaVersion = DerivedTextureManifest::kSchemaVersion;
            manifest.projectId = catalog.projectId;
            manifest.sceneId = scene.id;
            manifest.profileId = profile.id;
            manifest.scenePath = scene.source.generic_string();

            for (const DerivedTextureEntry &entry : manifest.entries) {
                const std::filesystem::path oldBlob = legacyRoot / entry.blob;
                const std::filesystem::path newBlob = targetRoot / entry.blob;
                if (!std::filesystem::is_regular_file(oldBlob))
                    throw std::runtime_error("legacy blob is missing: " +
                                             oldBlob.string());
                if (!std::filesystem::is_regular_file(newBlob)) {
                    std::filesystem::create_directories(newBlob.parent_path());
                    std::filesystem::copy_file(
                        oldBlob, newBlob,
                        std::filesystem::copy_options::skip_existing);
                    ++copiedBlobs;
                }
            }

            const std::filesystem::path targetManifest =
                derivedManifestPath(targetRoot, scene.id, profile.id);
            if (!saveDerivedTextureManifest(targetManifest, manifest, error))
                throw std::runtime_error(
                    "could not publish migrated manifest: " + error);
            ++migrated;
            std::cout << "Migrated " << scene.id << '/' << profile.id << " -> "
                      << targetManifest.string() << '\n';
        }
    }
    std::cout << "Texture cache migration complete\n"
              << "  manifests: " << migrated << "\n"
              << "  copied blobs: " << copiedBlobs << "\n"
              << "  unavailable scenes: " << skipped << '\n';
    return 0;
}

} // namespace vkr::assettool
