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
    if (value == "gbuffer")
        return ShaderProgramContract::GBuffer;
    if (value == "deferred-lighting")
        return ShaderProgramContract::DeferredLighting;
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
            (program.contract != ShaderProgramContract::Compute &&
             program.contract != ShaderProgramContract::DeferredLighting)) {
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
    case ShaderProgramContract::GBuffer:
    case ShaderProgramContract::Fullscreen:
        if (!hasFragment)
            throw fieldError(field,
                             "this graphics contract requires a fragment stage");
        break;
    case ShaderProgramContract::ShadowDepth:
    case ShaderProgramContract::PunctualShadowDepth:
        break;
    case ShaderProgramContract::Compute:
    case ShaderProgramContract::DeferredLighting:
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
                            {"schemaVersion", "programs", "variants",
                             "materialShaderFamilies", "viewModes"});

        ShaderRegistry registry;
        registry.manifestPath_ =
            std::filesystem::absolute(manifestPath).lexically_normal();
        const std::filesystem::path shaderRoot =
            registry.manifestPath_.parent_path();

        const uint32_t schemaVersion =
            root.at("schemaVersion").get<uint32_t>();
        if (schemaVersion != kSchemaVersion &&
            schemaVersion != kLegacySchemaVersion)
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
                 "sceneLights", "clusteredLighting", "atmosphere",
                 "screenSpace", "ddgi",
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
            program.usesClusteredLighting =
                item.value("clusteredLighting", false);
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
            if (program.usesClusteredLighting &&
                program.contract != ShaderProgramContract::MainForward &&
                program.contract !=
                    ShaderProgramContract::DeferredLighting) {
                throw fieldError(
                    field + ".clusteredLighting",
                    "only main-forward and deferred-lighting programs may "
                    "consume clustered lighting resources");
            }
            if (program.contract == ShaderProgramContract::MainForward &&
                program.usesClusteredLighting &&
                !program.usesSceneLights) {
                throw fieldError(field + ".clusteredLighting",
                                 "requires sceneLights for main-forward "
                                 "programs");
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

        constexpr std::array<const char *,
                             static_cast<size_t>(MaterialShaderPass::Count)>
            familyProgramFields = {
                "forwardOpaque", "forwardTransparent", "surfaceOpaque",
                "surfaceMask", "gBufferOpaque", "gBufferMask",
                "directionalShadowMask", "pointShadowMask", "spotShadowMask"};
        constexpr std::array<ShaderProgramContract,
                             static_cast<size_t>(MaterialShaderPass::Count)>
            familyProgramContracts = {
                ShaderProgramContract::MainForward,
                ShaderProgramContract::MainForward,
                ShaderProgramContract::SurfacePrepass,
                ShaderProgramContract::SurfacePrepass,
                ShaderProgramContract::GBuffer,
                ShaderProgramContract::GBuffer,
                ShaderProgramContract::ShadowDepth,
                ShaderProgramContract::PunctualShadowDepth,
                ShaderProgramContract::PunctualShadowDepth};

        auto addFamily = [&](MaterialShaderFamily family,
                             const std::string &field) {
            if (!isStableId(family.id))
                throw fieldError(field + ".id", "invalid stable ID");
            const bool duplicate = std::any_of(
                registry.materialShaders_.families_.begin(),
                registry.materialShaders_.families_.end(),
                [&](const MaterialShaderFamily &existing) {
                    return existing.id == family.id;
                });
            if (duplicate)
                throw fieldError(field + ".id", "duplicate family ID");
            for (size_t pass = 0; pass < family.programIds.size(); ++pass) {
                const std::string &programId = family.programIds[pass];
                const auto programIt = programIndices.find(programId);
                if (programIt == programIndices.end()) {
                    throw fieldError(
                        field + ".programs." + familyProgramFields[pass],
                        "unknown program ID");
                }
                const ShaderProgram &program =
                    registry.programs_[programIt->second];
                if (program.contract != familyProgramContracts[pass]) {
                    throw fieldError(
                        field + ".programs." + familyProgramFields[pass],
                        "program contract does not match material pass");
                }
            }
            registry.materialShaders_.families_.push_back(std::move(family));
        };

        if (schemaVersion == kSchemaVersion) {
            if (root.contains("variants"))
                throw fieldError("variants", "schema v3 uses viewModes");
            const Json &families = root.at("materialShaderFamilies");
            if (!families.is_array() || families.empty()) {
                throw fieldError("materialShaderFamilies",
                                 "expected a non-empty array");
            }
            size_t defaultFamilyCount = 0;
            std::string defaultFamilyId;
            for (size_t index = 0; index < families.size(); ++index) {
                const Json &item = families[index];
                const std::string field =
                    "materialShaderFamilies[" + std::to_string(index) + "]";
                rejectUnknownFields(item, field,
                                    {"id", "displayName", "category",
                                     "default", "order", "programs"});
                MaterialShaderFamily family;
                family.id = requiredString(item, "id", field);
                family.displayName =
                    requiredString(item, "displayName", field);
                family.category = requiredString(item, "category", field);
                if (!isStableId(family.category)) {
                    throw fieldError(field + ".category",
                                     "invalid category ID");
                }
                family.isDefault = item.at("default").get<bool>();
                family.order = item.at("order").get<int32_t>();
                const Json &programMap = item.at("programs");
                rejectUnknownFields(
                    programMap, field + ".programs",
                    {"forwardOpaque", "forwardTransparent",
                     "surfaceOpaque", "surfaceMask",
                     "gBufferOpaque", "gBufferMask",
                     "directionalShadowMask", "pointShadowMask",
                     "spotShadowMask"});
                for (size_t pass = 0; pass < family.programIds.size(); ++pass) {
                    family.programIds[pass] = requiredString(
                        programMap, familyProgramFields[pass],
                        field + ".programs");
                }
                if (family.isDefault) {
                    ++defaultFamilyCount;
                    defaultFamilyId = family.id;
                }
                addFamily(std::move(family), field);
            }
            if (defaultFamilyCount != 1) {
                throw fieldError("materialShaderFamilies",
                                 "expected exactly one default family");
            }
            const auto defaultFamilyIt = std::find_if(
                registry.materialShaders_.families_.begin(),
                registry.materialShaders_.families_.end(),
                [&](const MaterialShaderFamily &family) {
                    return family.id == defaultFamilyId;
                });
            registry.materialShaders_.defaultFamilyIndex_ =
                static_cast<size_t>(defaultFamilyIt -
                                    registry.materialShaders_.families_.begin());
        } else {
            MaterialShaderFamily family;
            family.id = "builtin.default-lit";
            family.displayName = "Default Lit";
            family.category = "builtin";
            family.isDefault = true;
            family.programIds = {
                "forward.pbr-lite-normal-mapped",
                "forward.pbr-lite-normal-mapped",
                "surface.prepass-opaque", "surface.prepass-mask",
                "gbuffer.default-lit-opaque", "gbuffer.default-lit-mask",
                "shadow.mask", "shadow.point-mask", "shadow.spot-mask"};
            const bool hasProductionFamilyPrograms = std::all_of(
                family.programIds.begin(), family.programIds.end(),
                [&](const std::string &programId) {
                    return programIndices.find(programId) !=
                           programIndices.end();
                });
            if (hasProductionFamilyPrograms) {
                addFamily(std::move(family),
                          "schemaV2Migration.defaultFamily");
            } else {
                const auto fallbackProgram = std::find_if(
                    registry.programs_.begin(), registry.programs_.end(),
                    [](const ShaderProgram &program) {
                        return program.contract ==
                               ShaderProgramContract::MainForward;
                    });
                if (fallbackProgram == registry.programs_.end()) {
                    throw fieldError(
                        "schemaV2Migration.defaultFamily",
                        "no main-forward program is available");
                }
                family.programIds.fill(fallbackProgram->id);
                registry.materialShaders_.families_.push_back(std::move(family));
            }
            registry.materialShaders_.defaultFamilyIndex_ = 0;
        }

        const char *viewArrayName =
            schemaVersion == kSchemaVersion ? "viewModes" : "variants";
        const Json &views = root.at(viewArrayName);
        if (!views.is_array() || views.empty()) {
            throw fieldError(viewArrayName, "expected a non-empty array");
        }
        std::unordered_set<std::string> viewModeIds;
        std::unordered_set<std::string> displayNames;
        size_t defaultCount = 0;
        std::string defaultId;
        for (size_t index = 0; index < views.size(); ++index) {
            const Json &item = views[index];
            const std::string field =
                std::string(viewArrayName) + "[" + std::to_string(index) + "]";
            rejectUnknownFields(item, field,
                                {"id", "displayName", "program", "category",
                                 "toneMapping", "bloom", "default", "order"});

            ViewMode viewMode;
            viewMode.id = requiredString(item, "id", field);
            if (!isStableId(viewMode.id))
                throw fieldError(field + ".id", "invalid stable ID");
            if (!viewModeIds.insert(viewMode.id).second)
                throw fieldError(field + ".id", "duplicate view mode ID");
            viewMode.displayName = requiredString(item, "displayName", field);
            if (!displayNames.insert(asciiLower(viewMode.displayName)).second) {
                throw fieldError(field + ".displayName",
                                 "duplicate display name");
            }
            viewMode.overrideProgramId = optionalString(item, "program", field);
            const ShaderProgram *program = nullptr;
            if (!viewMode.overrideProgramId.empty()) {
                const auto programIt =
                    programIndices.find(viewMode.overrideProgramId);
                if (programIt == programIndices.end())
                    throw fieldError(field + ".program", "unknown program ID");
                program = &registry.programs_[programIt->second];
                if (program->contract != ShaderProgramContract::MainForward) {
                    throw fieldError(
                        field + ".program",
                        "view mode overrides require a main-forward program");
                }
            } else {
                const MaterialShaderFamily &defaultFamily =
                    registry.materialShaders_.families_.at(
                        registry.materialShaders_.defaultFamilyIndex_);
                program = &registry.program(
                    defaultFamily.programId(MaterialShaderPass::ForwardOpaque));
            }
            viewMode.category = requiredString(item, "category", field);
            if (!isStableId(viewMode.category))
                throw fieldError(field + ".category", "invalid category ID");
            viewMode.toneMapping = parseToneMapping(
                requiredString(item, "toneMapping", field),
                field + ".toneMapping");
            viewMode.supportsBloom = item.value("bloom", false);
            viewMode.supportsAtmosphere = program->usesAtmosphere;
            viewMode.supportsScreenSpace = program->usesScreenSpace;
            viewMode.supportsDdgi = program->usesDdgi;
            viewMode.supportsDeferred =
                viewMode.overrideProgramId.empty();
            viewMode.isDefault = item.at("default").get<bool>();
            viewMode.order = item.at("order").get<int32_t>();
            if (schemaVersion == kLegacySchemaVersion && viewMode.isDefault &&
                viewMode.overrideProgramId ==
                    "forward.pbr-lite-normal-mapped") {
                viewMode.overrideProgramId.clear();
            }
            if (viewMode.isDefault) {
                ++defaultCount;
                defaultId = viewMode.id;
            }
            registry.viewModes_.push_back(std::move(viewMode));
        }
        if (defaultCount != 1)
            throw fieldError(viewArrayName,
                             "expected exactly one default view mode");

        std::sort(registry.viewModes_.begin(), registry.viewModes_.end(),
                  [](const ViewMode &left, const ViewMode &right) {
                      if (left.order != right.order)
                          return left.order < right.order;
                      return left.id < right.id;
                  });
        const auto defaultIt =
            std::find_if(registry.viewModes_.begin(), registry.viewModes_.end(),
                         [&defaultId](const ViewMode &viewMode) {
                             return viewMode.id == defaultId;
                         });
        registry.defaultViewModeIndex_ =
            static_cast<size_t>(defaultIt - registry.viewModes_.begin());
        return registry;
    } catch (const std::exception &exception) {
        throw std::runtime_error("Could not load shader manifest '" +
                                 manifestPath.string() + "': " +
                                 exception.what());
    }
}

const ViewMode &ShaderRegistry::defaultViewMode() const {
    if (viewModes_.empty() || defaultViewModeIndex_ >= viewModes_.size())
        throw std::logic_error("shader registry has no default view mode");
    return viewModes_[defaultViewModeIndex_];
}

const ViewMode *
ShaderRegistry::findViewMode(std::string_view idOrDisplayName) const {
    const auto byId =
        std::find_if(viewModes_.begin(), viewModes_.end(),
                     [idOrDisplayName](const ViewMode &viewMode) {
                         return viewMode.id == idOrDisplayName;
                     });
    if (byId != viewModes_.end())
        return &*byId;
    const std::string lowered = asciiLower(std::string(idOrDisplayName));
    const auto byName =
        std::find_if(viewModes_.begin(), viewModes_.end(),
                     [&lowered](const ViewMode &viewMode) {
                         return asciiLower(viewMode.displayName) == lowered;
                     });
    return byName == viewModes_.end() ? nullptr : &*byName;
}

MaterialShaderFamilyHandle
ShaderRegistry::defaultMaterialShaderFamily() const {
    return materialShaders_.defaultFamily();
}

MaterialShaderFamilyHandle
ShaderRegistry::findMaterialShaderFamily(std::string_view id) const {
    return materialShaders_.find(id);
}

const MaterialShaderFamily &ShaderRegistry::materialShaderFamily(
    MaterialShaderFamilyHandle handle) const {
    return materialShaders_.get(handle);
}

MaterialShaderFamilyHandle
MaterialShaderFamilyRegistry::defaultFamily() const {
    if (families_.empty() || defaultFamilyIndex_ >= families_.size())
        throw std::logic_error("shader registry has no default material family");
    return {static_cast<uint32_t>(defaultFamilyIndex_)};
}

MaterialShaderFamilyHandle
MaterialShaderFamilyRegistry::find(std::string_view id) const {
    const auto found = std::find_if(
        families_.begin(), families_.end(),
        [id](const MaterialShaderFamily &family) { return family.id == id; });
    if (found == families_.end())
        return {};
    return {static_cast<uint32_t>(found - families_.begin())};
}

const MaterialShaderFamily &MaterialShaderFamilyRegistry::get(
    MaterialShaderFamilyHandle handle) const {
    if (!handle.valid() || handle.index >= families_.size())
        throw std::out_of_range("invalid material shader family handle");
    return families_[handle.index];
}

const ShaderProgram &ShaderRegistry::materialProgram(
    MaterialShaderFamilyHandle family, MaterialShaderPass pass,
    const ViewMode *viewMode) const {
    if ((pass == MaterialShaderPass::ForwardOpaque ||
         pass == MaterialShaderPass::ForwardTransparent) &&
        viewMode && viewMode->overridesMaterialShader()) {
        return program(viewMode->overrideProgramId);
    }
    return program(materialShaderFamily(family).programId(pass));
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
    case ShaderProgramContract::GBuffer:
        return "gbuffer";
    case ShaderProgramContract::DeferredLighting:
        return "deferred-lighting";
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

const char *materialShaderPassName(MaterialShaderPass pass) {
    switch (pass) {
    case MaterialShaderPass::ForwardOpaque:
        return "forward-opaque";
    case MaterialShaderPass::ForwardTransparent:
        return "forward-transparent";
    case MaterialShaderPass::SurfaceOpaque:
        return "surface-opaque";
    case MaterialShaderPass::SurfaceMask:
        return "surface-mask";
    case MaterialShaderPass::GBufferOpaque:
        return "gbuffer-opaque";
    case MaterialShaderPass::GBufferMask:
        return "gbuffer-mask";
    case MaterialShaderPass::DirectionalShadowMask:
        return "directional-shadow-mask";
    case MaterialShaderPass::PointShadowMask:
        return "point-shadow-mask";
    case MaterialShaderPass::SpotShadowMask:
        return "spot-shadow-mask";
    case MaterialShaderPass::Count:
        break;
    }
    return "unknown";
}

} // namespace vkr
