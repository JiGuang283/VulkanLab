#include "TextureCacheBuilder.h"

#include "assets/DerivedAssetPaths.h"
#include "assets/SceneCatalog.h"

#include "assets/DerivedTextureManifest.h"

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
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace vkr::assettool {
namespace {

constexpr const char *kEncoderSettings =
    "ktx-4.4.2|uastc|quality=2|rdo=1|zstd=9|mips=lanczos4|tf=semantic";

struct ScopedHandle {
    HANDLE value = nullptr;
    ~ScopedHandle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
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
            const ULONG chunk = static_cast<ULONG>(
                std::min<size_t>(size - offset, ULONG_MAX));
            status = BCryptHashData(hash,
                                    const_cast<PUCHAR>(data + offset), chunk, 0);
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
    static constexpr std::array<uint8_t, 8> signature{
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
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

std::filesystem::path makeTemporaryPath(
    const std::filesystem::path &directory, const std::string &suffix) {
    static std::atomic<uint64_t> counter{0};
    return directory /
           (".vulkanlab-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(counter.fetch_add(1)) + suffix);
}

std::wstring quoteWindowsArgument(const std::wstring &argument) {
    if (argument.empty())
        return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

DWORD runProcess(const std::filesystem::path &executable,
                 const std::vector<std::wstring> &arguments) {
    std::wstring commandLine = quoteWindowsArgument(executable.wstring());
    for (const std::wstring &argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr,
                        nullptr, TRUE, 0, nullptr, nullptr, &startup,
                        &process)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not start ktx tool");
    }
    ScopedHandle processHandle{process.hProcess};
    ScopedHandle threadHandle{process.hThread};
    if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "waiting for ktx tool failed");
    }
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "reading ktx tool exit code failed");
    }
    return exitCode;
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
            throw std::runtime_error("image bufferView references invalid buffer");
        const tg3_buffer &buffer = model->buffers[view.buffer];
        if (view.byte_offset + view.byte_length > buffer.data.count)
            throw std::runtime_error("image bufferView is out of range");
        const uint8_t *begin = buffer.data.data + view.byte_offset;
        result.bytes.assign(begin, begin + view.byte_length);
    } else if (image.image.data && image.image.count > 0) {
        result.bytes.assign(image.image.data, image.image.data + image.image.count);
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
        result.externalPath = std::filesystem::absolute(result.externalPath)
                                  .lexically_normal();
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
        result.stamp.path = genericPath(relativeError ? result.externalPath
                                                      : relative);
    } else {
        result.stamp = fileStamp(scenePath, sourceHash);
        result.stamp.path = scenePath.filename().generic_string();
    }
    return result;
}

std::filesystem::path writeTemporaryPng(
    const ImageSource &source, const std::filesystem::path &directory) {
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

void printParseErrors(const tinygltf3::ErrorStack &errors) {
    for (uint32_t i = 0; i < errors.count(); ++i) {
        const tg3_error_entry *entry = errors.entry(i);
        if (entry && entry->message)
            std::cerr << "  " << entry->message << '\n';
    }
}

} // namespace

int buildTextureCache(const TextureCacheBuildOptions &options) {
    if (options.projectId.empty() || options.sceneId.empty() ||
        options.profileId.empty())
        throw std::invalid_argument(
            "projectId, sceneId, and profileId are required");
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

    const std::vector<BuildTask> tasks = collectTasks(model.get());
    DerivedTextureManifest manifest;
    manifest.projectId = options.projectId;
    manifest.sceneId = options.sceneId;
    manifest.profileId = options.profileId;
    manifest.scenePath = genericPath(sceneIdentity);
    manifest.textureLimit = options.textureLimit;
    const std::vector<uint8_t> sceneBytes = readFile(scene);
    manifest.scene = fileStamp(scene, sha256(sceneBytes));
    manifest.scene.path = scene.filename().generic_string();
    manifest.entries.reserve(tasks.size());

    uint64_t encodedCount = 0;
    uint64_t reusedCount = 0;
    std::cout << "Building texture cache for " << scene.string() << '\n'
              << "  profile: "
              << (options.textureLimit == 0
                      ? std::string("full")
                      : std::to_string(options.textureLimit))
              << "\n  textures: " << tasks.size() << "\n  ktx tool: "
              << ktxTool.string() << '\n';

    for (size_t taskIndex = 0; taskIndex < tasks.size(); ++taskIndex) {
        const BuildTask &task = tasks[taskIndex];
        ImageSource source = loadImageSource(model.get(), task.imageIndex, scene);
        const auto [outputWidth, outputHeight] = limitedExtent(
            source.width, source.height, options.textureLimit);
        const std::string keyMaterial =
            std::string(kEncoderSettings) + "|source=" + source.stamp.sha256 +
            "|semantic=" + textureSemanticName(task.semantic) + "|size=" +
            std::to_string(outputWidth) + "x" +
            std::to_string(outputHeight) + "|wrap=" +
            derivedMipmapWrapName(task.wrap);
        const std::string cacheKey = sha256(keyMaterial);
        const std::filesystem::path blob =
            blobDirectory / (cacheKey + ".ktx2");

        bool reuse = false;
        if (!options.force && std::filesystem::is_regular_file(blob)) {
            try {
                validateKtx2(blob, outputWidth, outputHeight);
                reuse = true;
            } catch (const std::exception &exception) {
                std::cerr << "  invalid cached blob, rebuilding: "
                          << exception.what() << '\n';
            }
        }

        if (!reuse) {
            ScopedTemporaryFile temporaryInput;
            const std::filesystem::path input =
                source.png && !source.externalPath.empty()
                    ? source.externalPath
                    : (temporaryInput.path =
                           writeTemporaryPng(source, blobDirectory));
            ScopedTemporaryFile temporaryOutput{
                makeTemporaryPath(blobDirectory, ".output.ktx2")};

            std::vector<std::wstring> arguments{
                L"create",
                L"--format",
                task.semantic == TextureSemantic::SrgbColor
                    ? L"R8G8B8A8_SRGB"
                    : L"R8G8B8A8_UNORM",
                L"--assign-tf",
                task.semantic == TextureSemantic::SrgbColor ? L"srgb"
                                                            : L"linear",
                L"--encode",
                // KTX 4.4.2 names the 4x4 UASTC encoder "uastc".
                L"uastc",
                L"--uastc-quality",
                L"2",
                L"--uastc-rdo",
                L"--uastc-rdo-l",
                task.semantic == TextureSemantic::Normal ? L"0.5" : L"1.0",
                L"--zstd",
                L"9",
                L"--generate-mipmap",
                L"--mipmap-filter",
                L"lanczos4",
                L"--mipmap-wrap",
                std::wstring(ktxWrapName(task.wrap),
                             ktxWrapName(task.wrap) +
                                 std::char_traits<char>::length(
                                     ktxWrapName(task.wrap))),
                L"--width",
                std::to_wstring(outputWidth),
                L"--height",
                std::to_wstring(outputHeight)};
            if (task.semantic == TextureSemantic::Normal)
                arguments.push_back(L"--normalize");
            arguments.push_back(input.wstring());
            arguments.push_back(temporaryOutput.path.wstring());

            std::cout << "  [" << (taskIndex + 1) << '/' << tasks.size()
                      << "] image " << task.imageIndex << " "
                      << textureSemanticName(task.semantic) << " "
                      << source.width << 'x' << source.height << " -> "
                      << outputWidth << 'x' << outputHeight << '\n';
            const DWORD exitCode = runProcess(ktxTool, arguments);
            if (exitCode != 0) {
                throw std::runtime_error("ktx create failed for image " +
                                         std::to_string(task.imageIndex) +
                                         " with exit code " +
                                         std::to_string(exitCode));
            }
            validateKtx2(temporaryOutput.path, outputWidth, outputHeight);
            atomicReplace(temporaryOutput.path, blob);
            temporaryOutput.path.clear();
            ++encodedCount;
        } else {
            ++reusedCount;
        }

        DerivedTextureEntry entry;
        entry.imageIndex = task.imageIndex;
        entry.semantic = task.semantic;
        entry.mipWrap = task.wrap;
        entry.width = outputWidth;
        entry.height = outputHeight;
        entry.cacheKey = cacheKey;
        entry.blob = genericPath(std::filesystem::relative(blob, cacheRoot));
        entry.source = std::move(source.stamp);
        manifest.entries.push_back(std::move(entry));
    }

    const std::filesystem::path manifestPath =
        derivedManifestPath(cacheRoot, options.sceneId, options.profileId);
    std::string saveError;
    if (!saveDerivedTextureManifest(manifestPath, manifest, saveError))
        throw std::runtime_error("could not publish manifest: " + saveError);

    std::cout << "Texture cache complete\n"
              << "  encoded: " << encodedCount << "\n"
              << "  reused: " << reusedCount << "\n"
              << "  manifest: " << manifestPath.string() << '\n';
    return 0;
}

int migrateTextureCache(const TextureCacheMigrationOptions &options) {
    const std::filesystem::path projectRoot =
        std::filesystem::absolute(options.projectRoot).lexically_normal();
    const SceneCatalog catalog = SceneCatalog::load(
        projectRoot / "assets/catalog.json", projectRoot);
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
                throw std::runtime_error("could not publish migrated manifest: " +
                                         error);
            ++migrated;
            std::cout << "Migrated " << scene.id << '/' << profile.id
                      << " -> " << targetManifest.string() << '\n';
        }
    }
    std::cout << "Texture cache migration complete\n"
              << "  manifests: " << migrated << "\n"
              << "  copied blobs: " << copiedBlobs << "\n"
              << "  unavailable scenes: " << skipped << '\n';
    return 0;
}

} // namespace vkr::assettool
