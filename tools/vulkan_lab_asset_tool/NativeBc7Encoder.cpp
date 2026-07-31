#include "NativeBc7Encoder.h"

#include <DirectXTex.h>
#include <ktx.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace vkr::assettool {
namespace {

class KtxOwner {
  public:
    ~KtxOwner() {
        if (texture_)
            ktxTexture_Destroy(ktxTexture(texture_));
    }

    ktxTexture2 **put() { return &texture_; }
    ktxTexture2 *get() const { return texture_; }

  private:
    ktxTexture2 *texture_ = nullptr;
};

std::string hresultString(HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase
           << static_cast<uint32_t>(result);
    return stream.str();
}

void throwKtxError(const char *operation, KTX_error_code result) {
    if (result != KTX_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 ktxErrorString(result));
    }
}

} // namespace

void encodeNativeBc7Ktx2(const std::filesystem::path &rgbaKtx2,
                         const std::filesystem::path &outputKtx2,
                         TextureSemantic semantic, bool exhaustive,
                         const std::atomic_bool &cancelRequested) {
    KtxOwner sourceOwner;
    KTX_error_code result = ktxTexture2_CreateFromNamedFile(
        rgbaKtx2.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        sourceOwner.put());
    throwKtxError("could not read preprocessed KTX2", result);

    ktxTexture2 *source2 = sourceOwner.get();
    ktxTexture *source = ktxTexture(source2);
    const uint32_t expectedSourceFormat =
        semantic == TextureSemantic::SrgbColor
            ? VK_FORMAT_R8G8B8A8_SRGB
            : VK_FORMAT_R8G8B8A8_UNORM;
    if (!source || source->numDimensions != 2 || source->numLayers != 1 ||
        source->numFaces != 1 || source->numLevels == 0 ||
        source2->vkFormat != expectedSourceFormat ||
        ktxTexture2_NeedsTranscoding(source2)) {
        throw std::runtime_error(
            "preprocessed KTX2 is not an RGBA8 2D mip chain");
    }

    std::vector<DirectX::Image> sourceImages;
    sourceImages.reserve(source->numLevels);
    uint8_t *sourceData = ktxTexture_GetData(source);
    for (uint32_t level = 0; level < source->numLevels; ++level) {
        ktx_size_t offset = 0;
        throwKtxError("could not query preprocessed mip offset",
                      ktxTexture_GetImageOffset(source, level, 0, 0, &offset));
        const size_t width = std::max<size_t>(1, source->baseWidth >> level);
        const size_t height = std::max<size_t>(1, source->baseHeight >> level);
        const size_t expectedBytes = width * height * 4;
        if (ktxTexture_GetImageSize(source, level) != expectedBytes)
            throw std::runtime_error("preprocessed RGBA8 mip size mismatch");
        sourceImages.push_back(
            {width, height,
             semantic == TextureSemantic::SrgbColor
                 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                 : DXGI_FORMAT_R8G8B8A8_UNORM,
             width * 4, expectedBytes, sourceData + offset});
    }

    DirectX::TexMetadata metadata{};
    metadata.width = source->baseWidth;
    metadata.height = source->baseHeight;
    metadata.depth = 1;
    metadata.arraySize = 1;
    metadata.mipLevels = source->numLevels;
    metadata.format = sourceImages.front().format;
    metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;

    DirectX::CompressOptions options{};
    options.flags = exhaustive
                        ? DirectX::TEX_COMPRESS_BC7_USE_3SUBSETS
                        : DirectX::TEX_COMPRESS_BC7_QUICK;
    options.threshold = DirectX::TEX_THRESHOLD_DEFAULT;
    options.alphaWeight = DirectX::TEX_ALPHA_WEIGHT_DEFAULT;
    const DXGI_FORMAT targetDxgiFormat =
        semantic == TextureSemantic::SrgbColor
            ? DXGI_FORMAT_BC7_UNORM_SRGB
            : DXGI_FORMAT_BC7_UNORM;

    DirectX::ScratchImage compressed;
    const HRESULT compressionResult = DirectX::CompressEx(
        sourceImages.data(), sourceImages.size(), metadata, targetDxgiFormat,
        options, compressed,
        [&cancelRequested](size_t, size_t) {
            return !cancelRequested.load();
        });
    if (FAILED(compressionResult)) {
        if (cancelRequested.load())
            throw std::runtime_error("BC7 compression cancelled");
        throw std::runtime_error("DirectXTex BC7 compression failed (" +
                                 hresultString(compressionResult) + ")");
    }
    if (cancelRequested.load())
        throw std::runtime_error("BC7 compression cancelled");

    ktxTextureCreateInfo createInfo{};
    createInfo.vkFormat = semantic == TextureSemantic::SrgbColor
                              ? VK_FORMAT_BC7_SRGB_BLOCK
                              : VK_FORMAT_BC7_UNORM_BLOCK;
    createInfo.baseWidth = source->baseWidth;
    createInfo.baseHeight = source->baseHeight;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = source->numLevels;
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    KtxOwner outputOwner;
    result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE,
                                outputOwner.put());
    throwKtxError("could not create native BC7 KTX2", result);

    for (uint32_t level = 0; level < source->numLevels; ++level) {
        const DirectX::Image *image = compressed.GetImage(level, 0, 0);
        if (!image || !image->pixels || image->format != targetDxgiFormat)
            throw std::runtime_error("DirectXTex returned an invalid BC7 mip");
        result = ktxTexture_SetImageFromMemory(
            ktxTexture(outputOwner.get()), level, 0, 0, image->pixels,
            image->slicePitch);
        throwKtxError("could not store BC7 mip in KTX2", result);
    }

    result = ktxTexture_WriteToNamedFile(ktxTexture(outputOwner.get()),
                                         outputKtx2.string().c_str());
    throwKtxError("could not write native BC7 KTX2", result);
}

} // namespace vkr::assettool
