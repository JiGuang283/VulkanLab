#include "render/shader/ShaderRegistry.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace vkr {

namespace {

using Json = nlohmann::json;

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}

bool isStableId(std::string_view value) {
    if (value.empty())
        return false;
    for (char c : value) {
        const bool valid = (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                           c == '-';
        if (!valid)
            return false;
    }
    return value.front() != '.' && value.front() != '_' &&
           value.front() != '-';
}

std::runtime_error fieldError(const std::string &field,
                              const std::string &message) {
    return std::runtime_error("Invalid shader manifest field '" + field +
                              "': " + message);
}

void rejectUnknownFields(
    const Json &object, const std::string &field,
    std::initializer_list<std::string_view> allowedFields) {
    if (!object.is_object())
        throw fieldError(field, "expected an object");
    for (auto it = object.begin(); it != object.end(); ++it) {
        const bool allowed =
            std::find(allowedFields.begin(), allowedFields.end(), it.key()) !=
            allowedFields.end();
        if (!allowed)
            throw fieldError(field + "." + it.key(), "unknown field");
    }
}

std::string requiredString(const Json &object, const char *name,
                           const std::string &field) {
    const std::string value = object.at(name).get<std::string>();
    if (value.empty())
        throw fieldError(field + "." + name, "cannot be empty");
    return value;
}

std::string optionalString(const Json &object, const char *name,
                           const std::string &field) {
    if (!object.contains(name))
        return {};
    return requiredString(object, name, field);
}

ShaderProgramContract parseContract(const std::string &value,
                                    const std::string &field) {
    if (value == "main-forward")
        return ShaderProgramContract::MainForward;
    if (value == "shadow-depth")
        return ShaderProgramContract::ShadowDepth;
    if (value == "punctual-shadow-depth")
        return ShaderProgramContract::PunctualShadowDepth;
    if (value == "surface-prepass")
        return ShaderProgramContract::SurfacePrepass;
    if (value == "fullscreen")
        return ShaderProgramContract::Fullscreen;
    if (value == "compute")
        return ShaderProgramContract::Compute;
    throw fieldError(field, "unknown contract '" + value + "'");
}

ShaderToneMappingPolicy parseToneMapping(const std::string &value,
                                         const std::string &field) {
    if (value == "pass-through")
        return ShaderToneMappingPolicy::PassThrough;
    if (value == "configurable")
        return ShaderToneMappingPolicy::Configurable;
    throw fieldError(field, "unknown tone mapping policy '" + value + "'");
}

std::string validateSourcePath(const std::string &value,
                               const std::string &expectedExtension,
                               const std::string &field) {
    const std::filesystem::path source =
        std::filesystem::u8path(value).lexically_normal();
    if (source.empty() || source.is_absolute())
        throw fieldError(field, "must be a relative path");
    for (const auto &component : source) {
        if (component == "..")
            throw fieldError(field, "path escapes the shader directory");
    }
    if (source.generic_string() != value)
        throw fieldError(field, "path must be normalized with '/' separators");
    if (source.extension() != expectedExtension)
        throw fieldError(field, "expected a " + expectedExtension + " file");
    return source.generic_string();
}

std::string resolveSpirvPath(const std::filesystem::path &shaderRoot,
                             const std::string &sourcePath,
                             const std::string &field,
                             std::string_view suffix = ".spv") {
    if (sourcePath.empty())
        return {};
    const std::filesystem::path result =
        shaderRoot / std::filesystem::u8path(sourcePath + std::string(suffix));
    if (!std::filesystem::is_regular_file(result))
        throw fieldError(field, "SPIR-V file is missing: " + result.string());
    return std::filesystem::absolute(result).lexically_normal().string();
}

void validateProgramStages(const ShaderProgram &program,
                           const std::string &field) {
    const bool hasVertex = !program.vertexSourcePath.empty();
    const bool hasFragment = !program.fragmentSourcePath.empty();
    const bool hasCompute = !program.computeSourcePath.empty();
    if (hasCompute) {
        if (hasVertex || hasFragment ||
            program.contract != ShaderProgramContract::Compute) {
            throw fieldError(
                field,
                "compute programs cannot contain graphics shader stages");
        }
        return;
    }
    if (!hasVertex)
        throw fieldError(field, "graphics programs require a vertex stage");
    switch (program.contract) {
    case ShaderProgramContract::MainForward:
    case ShaderProgramContract::SurfacePrepass:
    case ShaderProgramContract::Fullscreen:
        if (!hasFragment)
            throw fieldError(field,
                             "this graphics contract requires a fragment stage");
        break;
    case ShaderProgramContract::ShadowDepth:
    case ShaderProgramContract::PunctualShadowDepth:
        break;
    case ShaderProgramContract::Compute:
        throw fieldError(field, "compute contract requires a compute stage");
    }
}

} // namespace

ShaderRegistry
ShaderRegistry::load(const std::filesystem::path &manifestPath) {
    try {
        std::ifstream input(manifestPath, std::ios::binary);
        if (!input)
            throw std::runtime_error("could not open file");
        Json root;
        input >> root;
        rejectUnknownFields(root, "root",
                            {"schemaVersion", "programs", "variants"});

        ShaderRegistry registry;
        registry.manifestPath_ =
            std::filesystem::absolute(manifestPath).lexically_normal();
        const std::filesystem::path shaderRoot =
            registry.manifestPath_.parent_path();

        const uint32_t schemaVersion =
            root.at("schemaVersion").get<uint32_t>();
        if (schemaVersion != kSchemaVersion)
            throw fieldError("schemaVersion", "unsupported schema");

        const Json &programs = root.at("programs");
        if (!programs.is_array() || programs.empty())
            throw fieldError("programs", "expected a non-empty array");
        std::unordered_map<std::string, size_t> programIndices;
        for (size_t index = 0; index < programs.size(); ++index) {
            const Json &item = programs[index];
            const std::string field =
                "programs[" + std::to_string(index) + "]";
            rejectUnknownFields(
                item, field,
                {"id", "contract", "vertex", "fragment", "compute",
                 "sceneLights", "atmosphere", "screenSpace", "ddgi",
                 "materialTextures", "lightingMrt", "surfaceMrt",
                 "targetEnv"});

            ShaderProgram program;
            program.id = requiredString(item, "id", field);
            if (!isStableId(program.id))
                throw fieldError(field + ".id", "invalid stable ID");
            if (!programIndices.emplace(program.id, registry.programs_.size())
                     .second) {
                throw fieldError(field + ".id", "duplicate program ID");
            }
            program.contract = parseContract(
                requiredString(item, "contract", field), field + ".contract");
            program.usesSceneLights = item.value("sceneLights", false);
            program.usesAtmosphere = item.value("atmosphere", false);
            program.usesScreenSpace = item.value("screenSpace", false);
            program.usesDdgi = item.value("ddgi", false);
            program.usesMaterialTextures =
                item.value("materialTextures", false);
            program.usesLightingMrt = item.value("lightingMrt", false);
            program.usesSurfaceMrt = item.value("surfaceMrt", false);

            const std::string targetEnv =
                optionalString(item, "targetEnv", field);
            if (!targetEnv.empty() && targetEnv != "vulkan1.0" &&
                targetEnv != "vulkan1.2") {
                throw fieldError(field + ".targetEnv",
                                 "expected vulkan1.0 or vulkan1.2");
            }

            const std::string vertex = optionalString(item, "vertex", field);
            const std::string fragment =
                optionalString(item, "fragment", field);
            const std::string compute = optionalString(item, "compute", field);
            if (!vertex.empty()) {
                program.vertexSourcePath =
                    validateSourcePath(vertex, ".vert", field + ".vertex");
            }
            if (!fragment.empty()) {
                program.fragmentSourcePath = validateSourcePath(
                    fragment, ".frag", field + ".fragment");
            }
            if (!compute.empty()) {
                program.computeSourcePath =
                    validateSourcePath(compute, ".comp", field + ".compute");
            }
            validateProgramStages(program, field);
            if (program.usesSceneLights &&
                program.contract != ShaderProgramContract::MainForward) {
                throw fieldError(field + ".sceneLights",
                                 "only main-forward programs may consume "
                                 "scene lights");
            }
            if (program.usesAtmosphere &&
                (program.contract == ShaderProgramContract::ShadowDepth ||
                 program.contract ==
                     ShaderProgramContract::PunctualShadowDepth)) {
                throw fieldError(field + ".atmosphere",
                                 "shadow-depth programs cannot consume "
                                 "atmosphere resources");
            }
            if (program.usesScreenSpace &&
                program.contract != ShaderProgramContract::MainForward) {
                throw fieldError(field + ".screenSpace",
                                 "only main-forward programs may consume "
                                 "screen-space lighting resources");
            }
            if (program.usesLightingMrt &&
                program.contract != ShaderProgramContract::MainForward) {
                throw fieldError(field + ".lightingMrt",
                                 "only main-forward programs may expose "
                                 "lighting MRT outputs");
            }
            if (program.usesSurfaceMrt &&
                program.contract != ShaderProgramContract::SurfacePrepass) {
                throw fieldError(field + ".surfaceMrt",
                                 "only surface-prepass programs may expose "
                                 "reduced surface outputs");
            }
            if (program.usesDdgi &&
                program.contract != ShaderProgramContract::MainForward) {
                throw fieldError(field + ".ddgi",
                                 "only main-forward programs may consume "
                                 "DDGI resources");
            }
            program.vertSpvPath = resolveSpirvPath(
                shaderRoot, program.vertexSourcePath, field + ".vertex");
            program.fragSpvPath = resolveSpirvPath(
                shaderRoot, program.fragmentSourcePath, field + ".fragment");
            if (program.usesMaterialTextures) {
                if (program.fragmentSourcePath.empty()) {
                    throw fieldError(field + ".materialTextures",
                                     "requires a fragment stage");
                }
                program.bindlessFragSpvPath = resolveSpirvPath(
                    shaderRoot, program.fragmentSourcePath,
                    field + ".fragment", ".bindless.spv");
            }
            if (program.usesLightingMrt) {
                program.colorOnlyFragSpvPath = resolveSpirvPath(
                    shaderRoot, program.fragmentSourcePath,
                    field + ".fragment", ".mrt1.spv");
                program.specularFragSpvPath = resolveSpirvPath(
                    shaderRoot, program.fragmentSourcePath,
                    field + ".fragment", ".mrt2.spv");
                if (program.usesMaterialTextures) {
                    program.bindlessColorOnlyFragSpvPath = resolveSpirvPath(
                        shaderRoot, program.fragmentSourcePath,
                        field + ".fragment", ".mrt1.bindless.spv");
                    program.bindlessSpecularFragSpvPath = resolveSpirvPath(
                        shaderRoot, program.fragmentSourcePath,
                        field + ".fragment", ".mrt2.bindless.spv");
                }
            }
            if (program.usesSurfaceMrt) {
                for (uint32_t attachmentCount = 0; attachmentCount < 3;
                     ++attachmentCount) {
                    const std::string suffix =
                        ".mrt" + std::to_string(attachmentCount) + ".spv";
                    program.reducedSurfaceFragSpvPaths[attachmentCount] =
                        resolveSpirvPath(shaderRoot,
                                         program.fragmentSourcePath,
                                         field + ".fragment", suffix);
                    if (program.usesMaterialTextures) {
                        const std::string bindlessSuffix =
                            ".mrt" + std::to_string(attachmentCount) +
                            ".bindless.spv";
                        program.bindlessReducedSurfaceFragSpvPaths
                            [attachmentCount] = resolveSpirvPath(
                                shaderRoot, program.fragmentSourcePath,
                                field + ".fragment", bindlessSuffix);
                    }
                }
            }
            program.computeSpvPath = resolveSpirvPath(
                shaderRoot, program.computeSourcePath, field + ".compute");
            registry.programs_.push_back(std::move(program));
        }

        const Json &variants = root.at("variants");
        if (!variants.is_array() || variants.empty())
            throw fieldError("variants", "expected a non-empty array");
        std::unordered_set<std::string> variantIds;
        std::unordered_set<std::string> displayNames;
        size_t defaultCount = 0;
        std::string defaultId;
        for (size_t index = 0; index < variants.size(); ++index) {
            const Json &item = variants[index];
            const std::string field =
                "variants[" + std::to_string(index) + "]";
            rejectUnknownFields(item, field,
                                {"id", "displayName", "program", "category",
                                 "toneMapping", "bloom", "default", "order"});

            ShaderVariant variant;
            variant.id = requiredString(item, "id", field);
            if (!isStableId(variant.id))
                throw fieldError(field + ".id", "invalid stable ID");
            if (!variantIds.insert(variant.id).second)
                throw fieldError(field + ".id", "duplicate variant ID");
            variant.displayName = requiredString(item, "displayName", field);
            if (!displayNames.insert(asciiLower(variant.displayName)).second) {
                throw fieldError(field + ".displayName",
                                 "duplicate display name");
            }
            variant.programId = requiredString(item, "program", field);
            const auto programIt = programIndices.find(variant.programId);
            if (programIt == programIndices.end())
                throw fieldError(field + ".program", "unknown program ID");
            const ShaderProgram &program =
                registry.programs_[programIt->second];
            if (program.contract != ShaderProgramContract::MainForward) {
                throw fieldError(field + ".program",
                                 "variants require a main-forward program");
            }
            variant.category = requiredString(item, "category", field);
            if (!isStableId(variant.category))
                throw fieldError(field + ".category", "invalid category ID");
            variant.toneMapping = parseToneMapping(
                requiredString(item, "toneMapping", field),
                field + ".toneMapping");
            variant.supportsBloom = item.value("bloom", false);
            variant.supportsAtmosphere = program.usesAtmosphere;
            variant.supportsScreenSpace = program.usesScreenSpace;
            variant.supportsDdgi = program.usesDdgi;
            variant.isDefault = item.at("default").get<bool>();
            variant.order = item.at("order").get<int32_t>();
            variant.vertSpvPath = program.vertSpvPath;
            variant.fragSpvPath = program.fragSpvPath;
            variant.bindlessFragSpvPath = program.bindlessFragSpvPath;
            variant.colorOnlyFragSpvPath = program.colorOnlyFragSpvPath;
            variant.bindlessColorOnlyFragSpvPath =
                program.bindlessColorOnlyFragSpvPath;
            variant.specularFragSpvPath = program.specularFragSpvPath;
            variant.bindlessSpecularFragSpvPath =
                program.bindlessSpecularFragSpvPath;
            variant.usesLightingMrt = program.usesLightingMrt;
            if (variant.isDefault) {
                ++defaultCount;
                defaultId = variant.id;
            }
            registry.variants_.push_back(std::move(variant));
        }
        if (defaultCount != 1)
            throw fieldError("variants", "expected exactly one default variant");

        std::sort(registry.variants_.begin(), registry.variants_.end(),
                  [](const ShaderVariant &left, const ShaderVariant &right) {
                      if (left.order != right.order)
                          return left.order < right.order;
                      return left.id < right.id;
                  });
        const auto defaultIt =
            std::find_if(registry.variants_.begin(), registry.variants_.end(),
                         [&defaultId](const ShaderVariant &variant) {
                             return variant.id == defaultId;
                         });
        registry.defaultVariantIndex_ =
            static_cast<size_t>(defaultIt - registry.variants_.begin());
        return registry;
    } catch (const std::exception &exception) {
        throw std::runtime_error("Could not load shader manifest '" +
                                 manifestPath.string() + "': " +
                                 exception.what());
    }
}

const ShaderVariant &ShaderRegistry::defaultVariant() const {
    if (variants_.empty() || defaultVariantIndex_ >= variants_.size())
        throw std::logic_error("shader registry has no default variant");
    return variants_[defaultVariantIndex_];
}

const ShaderVariant *
ShaderRegistry::findVariant(std::string_view idOrDisplayName) const {
    const auto byId =
        std::find_if(variants_.begin(), variants_.end(),
                     [idOrDisplayName](const ShaderVariant &variant) {
                         return variant.id == idOrDisplayName;
                     });
    if (byId != variants_.end())
        return &*byId;
    const std::string lowered = asciiLower(std::string(idOrDisplayName));
    const auto byName =
        std::find_if(variants_.begin(), variants_.end(),
                     [&lowered](const ShaderVariant &variant) {
                         return asciiLower(variant.displayName) == lowered;
                     });
    return byName == variants_.end() ? nullptr : &*byName;
}

const ShaderProgram *ShaderRegistry::findProgram(std::string_view id) const {
    const auto found =
        std::find_if(programs_.begin(), programs_.end(),
                     [id](const ShaderProgram &program) {
                         return program.id == id;
                     });
    return found == programs_.end() ? nullptr : &*found;
}

const ShaderProgram &ShaderRegistry::program(std::string_view id) const {
    const ShaderProgram *found = findProgram(id);
    if (!found)
        throw std::out_of_range("unknown shader program: " + std::string(id));
    return *found;
}

std::vector<std::filesystem::path> ShaderRegistry::spirvPaths() const {
    std::set<std::filesystem::path> unique;
    for (const ShaderProgram &program : programs_) {
        for (const std::string *path : {&program.vertSpvPath,
                                        &program.fragSpvPath,
                                        &program.bindlessFragSpvPath,
                                        &program.colorOnlyFragSpvPath,
                                        &program.bindlessColorOnlyFragSpvPath,
                                        &program.specularFragSpvPath,
                                        &program.bindlessSpecularFragSpvPath,
                                        &program.computeSpvPath}) {
            if (!path->empty())
                unique.insert(std::filesystem::path(*path));
        }
        for (const auto *paths : {&program.reducedSurfaceFragSpvPaths,
                                  &program.bindlessReducedSurfaceFragSpvPaths}) {
            for (const std::string &path : *paths) {
                if (!path.empty())
                    unique.insert(std::filesystem::path(path));
            }
        }
    }
    return {unique.begin(), unique.end()};
}

bool ShaderRegistry::supportsBindlessMaterials() const {
    return std::all_of(
        programs_.begin(), programs_.end(), [](const ShaderProgram &program) {
            return !program.usesMaterialTextures ||
                   !program.bindlessFragSpvPath.empty();
        });
}

const char *shaderProgramContractName(ShaderProgramContract contract) {
    switch (contract) {
    case ShaderProgramContract::MainForward:
        return "main-forward";
    case ShaderProgramContract::ShadowDepth:
        return "shadow-depth";
    case ShaderProgramContract::PunctualShadowDepth:
        return "punctual-shadow-depth";
    case ShaderProgramContract::SurfacePrepass:
        return "surface-prepass";
    case ShaderProgramContract::Fullscreen:
        return "fullscreen";
    case ShaderProgramContract::Compute:
        return "compute";
    }
    return "unknown";
}

const char *
shaderToneMappingPolicyName(ShaderToneMappingPolicy policy) {
    switch (policy) {
    case ShaderToneMappingPolicy::PassThrough:
        return "pass-through";
    case ShaderToneMappingPolicy::Configurable:
        return "configurable";
    }
    return "unknown";
}

} // namespace vkr
