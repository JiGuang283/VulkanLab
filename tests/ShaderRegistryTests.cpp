#include "render/ShaderRegistry.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void requireRegistry(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class ShaderRegistryFixture {
  public:
    ShaderRegistryFixture() {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        root = std::filesystem::temp_directory_path() /
               ("vulkan_lab_shader_registry_" + std::to_string(suffix));
        std::filesystem::create_directories(root / "main");
        std::ofstream(root / "main/test.vert.spv", std::ios::binary) << "SPV";
        std::ofstream(root / "main/test.frag.spv", std::ios::binary) << "SPV";
        std::ofstream(root / "main/test.comp.spv", std::ios::binary) << "SPV";
    }

    ~ShaderRegistryFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void write(const std::string &json) const {
        std::ofstream(root / "manifest.json", std::ios::binary) << json;
    }

    std::filesystem::path manifest() const { return root / "manifest.json"; }

    std::filesystem::path root;
};

std::string validManifest(std::string variants = R"(
    [
      {
        "id":"second",
        "displayName":"Second",
        "program":"forward.test",
        "category":"debug",
        "toneMapping":"pass-through",
        "default":false,
        "order":20
      },
      {
        "id":"first",
        "displayName":"First",
        "program":"forward.test",
        "category":"pbr",
        "toneMapping":"configurable",
        "default":true,
        "order":10
      }
    ])") {
    return R"({
      "schemaVersion":1,
      "programs":[
        {
          "id":"forward.test",
          "contract":"main-forward",
          "vertex":"main/test.vert",
          "fragment":"main/test.frag"
        }
      ],
      "variants":)" +
           variants + "}";
}

bool loadFailsWith(const ShaderRegistryFixture &fixture,
                   const std::string &needle) {
    try {
        (void)vkr::ShaderRegistry::load(fixture.manifest());
    } catch (const std::exception &error) {
        return std::string(error.what()).find(needle) != std::string::npos;
    }
    return false;
}

void testRegistryLoadsAndSortsVariants() {
    ShaderRegistryFixture fixture;
    fixture.write(validManifest());
    const vkr::ShaderRegistry registry =
        vkr::ShaderRegistry::load(fixture.manifest());
    requireRegistry(registry.variants().size() == 2,
                    "shader variants were not loaded");
    requireRegistry(registry.variants()[0].id == "first" &&
                        registry.variants()[1].id == "second",
                    "shader variants were not sorted by order");
    requireRegistry(registry.defaultVariant().id == "first",
                    "default shader variant was not selected");
    requireRegistry(registry.findVariant("first") != nullptr &&
                        registry.findVariant("FIRST") != nullptr &&
                        registry.findVariant("second") != nullptr,
                    "shader ID/display-name lookup failed");
    requireRegistry(
        registry.defaultVariant().toneMapping ==
            vkr::ShaderToneMappingPolicy::Configurable,
        "shader tone mapping policy was not parsed");
    requireRegistry(registry.spirvPaths().size() == 2,
                    "shader SPIR-V paths were not deduplicated");
}

void testProjectManifestPreservesPublicVariants() {
    const std::filesystem::path shaderRoot =
        std::filesystem::absolute(VKL_TEST_SHADER_ROOT).lexically_normal();
    const vkr::ShaderRegistry registry =
        vkr::ShaderRegistry::load(shaderRoot / "manifest.json");
    static constexpr std::array<std::string_view, 14> expectedIds = {
        "legacy-forward",
        "pbr-lite-forward",
        "pbr-lite-normal-mapped",
        "debug-base-color",
        "debug-normal",
        "debug-roughness",
        "debug-metallic",
        "debug-occlusion",
        "debug-emissive",
        "debug-alpha",
        "debug-transmission",
        "debug-shadow",
        "debug-ibl-diffuse",
        "debug-ibl-specular",
    };
    requireRegistry(registry.variants().size() == expectedIds.size(),
                    "project shader variant count changed");
    for (size_t index = 0; index < expectedIds.size(); ++index) {
        requireRegistry(registry.variants()[index].id == expectedIds[index],
                        "project shader variant order changed");
    }
    requireRegistry(registry.defaultVariant().id ==
                        "pbr-lite-normal-mapped",
                    "project default shader variant changed");
    requireRegistry(
        registry.defaultVariant().toneMapping ==
            vkr::ShaderToneMappingPolicy::Configurable,
        "project default shader must use configurable tone mapping");
    requireRegistry(registry.findProgram("shadow.opaque") != nullptr &&
                        registry.findProgram("shadow.mask") != nullptr &&
                        registry.findProgram("postprocess.tonemap") != nullptr &&
                        registry.findProgram(
                            "postprocess.taa-resolve") != nullptr &&
                        registry.findProgram(
                            "postprocess.bloom-downsample") != nullptr &&
                        registry.findProgram(
                            "postprocess.bloom-upsample") != nullptr &&
                        registry.findProgram("skybox") != nullptr &&
                        registry.findProgram(
                            "atmosphere.transmittance") != nullptr &&
                        registry.findProgram("atmosphere.sky") != nullptr,
                    "required internal shader program is missing");
    requireRegistry(registry.defaultVariant().supportsBloom,
                    "project default shader must support Bloom");
    requireRegistry(registry.defaultVariant().supportsAtmosphere,
                    "project default shader must support Atmosphere");
    requireRegistry(registry.defaultVariant().supportsScreenSpace,
                    "project default shader must support screen-space effects");
}

void testRegistryRejectsDuplicateDisplayName() {
    ShaderRegistryFixture fixture;
    fixture.write(validManifest(R"([
      {
        "id":"one","displayName":"Same","program":"forward.test",
        "category":"debug","toneMapping":"pass-through",
        "default":true,"order":0
      },
      {
        "id":"two","displayName":"same","program":"forward.test",
        "category":"debug","toneMapping":"pass-through",
        "default":false,"order":1
      }
    ])"));
    requireRegistry(loadFailsWith(fixture, "duplicate display name"),
                    "duplicate shader display names were accepted");
}

void testRegistryRejectsMissingDefault() {
    ShaderRegistryFixture fixture;
    fixture.write(validManifest(R"([
      {
        "id":"one","displayName":"One","program":"forward.test",
        "category":"debug","toneMapping":"pass-through",
        "default":false,"order":0
      }
    ])"));
    requireRegistry(loadFailsWith(fixture, "exactly one default"),
                    "manifest without a default variant was accepted");
}

void testRegistryRejectsUnknownProgram() {
    ShaderRegistryFixture fixture;
    fixture.write(validManifest(R"([
      {
        "id":"one","displayName":"One","program":"missing",
        "category":"debug","toneMapping":"pass-through",
        "default":true,"order":0
      }
    ])"));
    requireRegistry(loadFailsWith(fixture, "unknown program ID"),
                    "variant with an unknown program was accepted");
}

void testRegistryRejectsEscapingAndMissingPaths() {
    ShaderRegistryFixture fixture;
    std::string manifest = validManifest();
    const size_t path = manifest.find("main/test.vert");
    manifest.replace(path, std::string("main/test.vert").size(),
                     "../test.vert");
    fixture.write(manifest);
    requireRegistry(loadFailsWith(fixture, "escapes"),
                    "escaping shader source path was accepted");

    manifest = validManifest();
    const size_t fragment = manifest.find("main/test.frag");
    manifest.replace(fragment, std::string("main/test.frag").size(),
                     "main/missing.frag");
    fixture.write(manifest);
    requireRegistry(loadFailsWith(fixture, "SPIR-V file is missing"),
                    "missing shader SPIR-V was accepted");
}

void testRegistryRejectsUnknownPolicyAndStageConflict() {
    ShaderRegistryFixture fixture;
    std::string manifest = validManifest();
    const size_t policy = manifest.find("configurable");
    manifest.replace(policy, std::string("configurable").size(), "unknown");
    fixture.write(manifest);
    requireRegistry(loadFailsWith(fixture, "unknown tone mapping policy"),
                    "unknown tone mapping policy was accepted");

    fixture.write(R"({
      "schemaVersion":1,
      "programs":[
        {
          "id":"invalid.compute",
          "contract":"compute",
          "vertex":"main/test.vert",
          "compute":"main/test.comp"
        }
      ],
      "variants":[
        {
          "id":"invalid","displayName":"Invalid",
          "program":"invalid.compute","category":"debug",
          "toneMapping":"pass-through","default":true,"order":0
        }
      ]
    })");
    requireRegistry(loadFailsWith(fixture, "cannot contain graphics"),
                    "mixed compute and graphics stages were accepted");
}

} // namespace

void runShaderRegistryTests() {
    testRegistryLoadsAndSortsVariants();
    testProjectManifestPreservesPublicVariants();
    testRegistryRejectsDuplicateDisplayName();
    testRegistryRejectsMissingDefault();
    testRegistryRejectsUnknownProgram();
    testRegistryRejectsEscapingAndMissingPaths();
    testRegistryRejectsUnknownPolicyAndStageConflict();
}
