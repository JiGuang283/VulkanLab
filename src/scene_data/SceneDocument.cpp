#include "SceneDocument.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace vkr {
namespace {

using Json = nlohmann::ordered_json;

constexpr float kEpsilon = 1.0e-6f;
constexpr float kHalfPi = 1.57079632679f;
constexpr float kPi = 3.14159265359f;

glm::quat rotationLookingAlong(const glm::vec3 &forward,
                               const glm::vec3 &upHint) {
    const glm::vec3 normalizedForward = glm::normalize(forward);
    glm::vec3 right = glm::cross(normalizedForward, upHint);
    if (glm::dot(right, right) <= kEpsilon)
        right = glm::cross(normalizedForward, glm::vec3(0.0f, 1.0f, 0.0f));
    right = glm::normalize(right);
    const glm::vec3 up = glm::normalize(glm::cross(right, normalizedForward));
    return glm::normalize(
        glm::quat_cast(glm::mat3(right, up, -normalizedForward)));
}

std::runtime_error documentError(const std::string &field,
                                 const std::string &message) {
    return std::runtime_error("Invalid scene document field '" + field +
                              "': " + message);
}

void requireObject(const Json &value, const std::string &field) {
    if (!value.is_object())
        throw documentError(field, "expected an object");
}

void requireOnlyKeys(const Json &value, const std::string &field,
                     std::initializer_list<const char *> allowed) {
    requireObject(value, field);
    for (auto it = value.begin(); it != value.end(); ++it) {
        const bool known = std::any_of(
            allowed.begin(), allowed.end(),
            [&](const char *candidate) { return it.key() == candidate; });
        if (!known)
            throw documentError(field + "." + it.key(), "unknown field");
    }
}

glm::vec3 readVec3(const Json &value, const std::string &field) {
    if (!value.is_array() || value.size() != 3)
        throw documentError(field, "expected three numbers");
    glm::vec3 result{};
    for (size_t index = 0; index < 3; ++index) {
        result[static_cast<glm::length_t>(index)] =
            value.at(index).get<float>();
        if (!std::isfinite(result[static_cast<glm::length_t>(index)]))
            throw documentError(field, "values must be finite");
    }
    return result;
}

glm::quat readQuaternion(const Json &value, const std::string &field) {
    if (!value.is_array() || value.size() != 4)
        throw documentError(field, "expected four numbers in x/y/z/w order");
    const float x = value.at(0).get<float>();
    const float y = value.at(1).get<float>();
    const float z = value.at(2).get<float>();
    const float w = value.at(3).get<float>();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(w)) {
        throw documentError(field, "values must be finite");
    }
    const glm::quat result{w, x, y, z};
    const float length = glm::length(result);
    if (!std::isfinite(length) || length <= kEpsilon)
        throw documentError(field, "quaternion length is too small");
    return glm::normalize(result);
}

float finiteFloat(const Json &value, const std::string &field) {
    const float result = value.get<float>();
    if (!std::isfinite(result))
        throw documentError(field, "expected a finite number");
    return result;
}

PersistentEntityId readEntityId(const Json &value,
                                const std::string &field) {
    const auto parsed =
        PersistentEntityId::parse(value.get<std::string>());
    if (!parsed || parsed->empty())
        throw documentError(field, "expected a non-zero UUID");
    return *parsed;
}

SceneDocumentLightType readLightType(const std::string &value,
                                     const std::string &field) {
    if (value == "directional")
        return SceneDocumentLightType::Directional;
    if (value == "point")
        return SceneDocumentLightType::Point;
    if (value == "spot")
        return SceneDocumentLightType::Spot;
    throw documentError(field, "expected directional, point, or spot");
}

const char *lightTypeName(SceneDocumentLightType type) {
    switch (type) {
    case SceneDocumentLightType::Directional:
        return "directional";
    case SceneDocumentLightType::Point:
        return "point";
    case SceneDocumentLightType::Spot:
        return "spot";
    }
    return "directional";
}

ReflectionProbeShape readReflectionProbeShape(
    const std::string &value, const std::string &field) {
    if (value == "box")
        return ReflectionProbeShape::Box;
    if (value == "sphere")
        return ReflectionProbeShape::Sphere;
    throw documentError(field, "expected box or sphere");
}

const char *reflectionProbeShapeName(ReflectionProbeShape shape) {
    switch (shape) {
    case ReflectionProbeShape::Box:
        return "box";
    case ReflectionProbeShape::Sphere:
        return "sphere";
    }
    return "box";
}

bool pathWithin(const std::filesystem::path &root,
                const std::filesystem::path &candidate) {
    std::error_code error;
    const auto normalizedRoot =
        std::filesystem::weakly_canonical(root, error).lexically_normal();
    if (error)
        return false;
    error.clear();
    auto normalizedCandidate =
        std::filesystem::weakly_canonical(candidate, error).lexically_normal();
    if (error) {
        error.clear();
        normalizedCandidate =
            std::filesystem::absolute(candidate, error).lexically_normal();
    }
    if (error)
        return false;
    auto rootIt = normalizedRoot.begin();
    auto candidateIt = normalizedCandidate.begin();
    for (; rootIt != normalizedRoot.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == normalizedCandidate.end())
            return false;
#ifdef _WIN32
        std::string left = rootIt->string();
        std::string right = candidateIt->string();
        const auto lower = [](char value) {
            return static_cast<char>(std::tolower(
                static_cast<unsigned char>(value)));
        };
        std::transform(left.begin(), left.end(), left.begin(), lower);
        std::transform(right.begin(), right.end(), right.begin(), lower);
        if (left != right)
#else
        if (*rootIt != *candidateIt)
#endif
            return false;
    }
    return true;
}

bool hasSceneDocumentSuffix(const std::filesystem::path &path) {
    std::string filename = path.filename().string();
    std::transform(filename.begin(), filename.end(), filename.begin(),
                   [](char value) {
                       return static_cast<char>(std::tolower(
                           static_cast<unsigned char>(value)));
                   });
    constexpr const char *suffix = ".vkscene.json";
    const size_t suffixLength = std::char_traits<char>::length(suffix);
    return filename.size() >= suffixLength &&
           filename.compare(filename.size() - suffixLength, suffixLength,
                            suffix) == 0;
}

SceneTransformDocument parseTransform(const Json &value,
                                      const std::string &field) {
    requireOnlyKeys(value, field,
                    {"translation", "rotation", "scale"});
    SceneTransformDocument transform;
    transform.translation =
        readVec3(value.at("translation"), field + ".translation");
    transform.rotation =
        readQuaternion(value.at("rotation"), field + ".rotation");
    transform.scale = readVec3(value.at("scale"), field + ".scale");
    return transform;
}

ModelInstanceDocument parseModelInstance(const Json &value,
                                         const std::string &field) {
    requireOnlyKeys(value, field, {"model"});
    try {
        return {ModelAssetId(value.at("model").get<std::string>())};
    } catch (const std::invalid_argument &exception) {
        throw documentError(field + ".model", exception.what());
    }
}

LightComponentDocument parseLight(const Json &value, const std::string &field,
                                  uint32_t sourceSchemaVersion) {
    if (sourceSchemaVersion >= 3) {
        requireOnlyKeys(value, field,
                        {"type", "castsShadow", "atmosphereSunIndex",
                         "sourceAngularRadiusRadians", "color", "intensity",
                         "range", "innerConeRadians", "outerConeRadians"});
    } else if (sourceSchemaVersion >= 2) {
        requireOnlyKeys(value, field,
                        {"type", "castsShadow", "color", "intensity",
                         "range", "innerConeRadians", "outerConeRadians"});
    } else {
        requireOnlyKeys(value, field,
                        {"type", "color", "intensity", "range",
                         "innerConeRadians", "outerConeRadians"});
    }
    LightComponentDocument light;
    light.type = readLightType(value.at("type").get<std::string>(),
                               field + ".type");
    light.castsShadow = sourceSchemaVersion >= 2
                            ? value.at("castsShadow").get<bool>()
                            : light.type ==
                                  SceneDocumentLightType::Directional;
    if (sourceSchemaVersion >= 3 &&
        value.contains("atmosphereSunIndex") &&
        !value.at("atmosphereSunIndex").is_null()) {
        light.atmosphereSunIndex =
            value.at("atmosphereSunIndex").get<uint32_t>();
    }
    if (sourceSchemaVersion >= 3 &&
        value.contains("sourceAngularRadiusRadians")) {
        light.sourceAngularRadiusRadians = finiteFloat(
            value.at("sourceAngularRadiusRadians"),
            field + ".sourceAngularRadiusRadians");
    }
    light.color = readVec3(value.at("color"), field + ".color");
    light.intensity = finiteFloat(value.at("intensity"),
                                  field + ".intensity");
    if (value.contains("range") && !value.at("range").is_null())
        light.range = finiteFloat(value.at("range"), field + ".range");
    light.innerConeRadians = value.contains("innerConeRadians")
                                 ? finiteFloat(value.at("innerConeRadians"),
                                               field + ".innerConeRadians")
                                 : 0.0f;
    light.outerConeRadians = value.contains("outerConeRadians")
                                 ? finiteFloat(value.at("outerConeRadians"),
                                               field + ".outerConeRadians")
                                 : 0.785398163f;
    return light;
}

CameraComponentDocument parseCamera(const Json &value,
                                    const std::string &field) {
    requireOnlyKeys(value, field,
                    {"verticalFovRadians", "nearPlane", "farPlane"});
    CameraComponentDocument camera;
    camera.verticalFovRadians = finiteFloat(
        value.at("verticalFovRadians"), field + ".verticalFovRadians");
    camera.nearPlane =
        finiteFloat(value.at("nearPlane"), field + ".nearPlane");
    camera.farPlane =
        finiteFloat(value.at("farPlane"), field + ".farPlane");
    return camera;
}

AtmosphereComponentDocument parseAtmosphere(const Json &value,
                                            const std::string &field) {
    requireOnlyKeys(
        value, field,
        {"bottomRadiusKm", "atmosphereHeightKm",
         "rayleighScatteringPerKm", "rayleighScaleHeightKm",
         "mieScatteringPerKm", "mieExtinctionPerKm", "mieScaleHeightKm",
         "mieAnisotropy", "ozoneAbsorptionPerKm", "ozoneCenterHeightKm",
         "ozoneHalfWidthKm", "groundAlbedo", "multipleScatteringFactor",
         "aerialPerspectiveStartMeters",
         "aerialPerspectiveDistanceScale"});
    AtmosphereComponentDocument atmosphere;
    atmosphere.bottomRadiusKm = finiteFloat(
        value.at("bottomRadiusKm"), field + ".bottomRadiusKm");
    atmosphere.atmosphereHeightKm = finiteFloat(
        value.at("atmosphereHeightKm"), field + ".atmosphereHeightKm");
    atmosphere.rayleighScatteringPerKm = readVec3(
        value.at("rayleighScatteringPerKm"),
        field + ".rayleighScatteringPerKm");
    atmosphere.rayleighScaleHeightKm = finiteFloat(
        value.at("rayleighScaleHeightKm"),
        field + ".rayleighScaleHeightKm");
    atmosphere.mieScatteringPerKm = finiteFloat(
        value.at("mieScatteringPerKm"), field + ".mieScatteringPerKm");
    atmosphere.mieExtinctionPerKm = finiteFloat(
        value.at("mieExtinctionPerKm"), field + ".mieExtinctionPerKm");
    atmosphere.mieScaleHeightKm = finiteFloat(
        value.at("mieScaleHeightKm"), field + ".mieScaleHeightKm");
    atmosphere.mieAnisotropy = finiteFloat(
        value.at("mieAnisotropy"), field + ".mieAnisotropy");
    atmosphere.ozoneAbsorptionPerKm = readVec3(
        value.at("ozoneAbsorptionPerKm"),
        field + ".ozoneAbsorptionPerKm");
    atmosphere.ozoneCenterHeightKm = finiteFloat(
        value.at("ozoneCenterHeightKm"), field + ".ozoneCenterHeightKm");
    atmosphere.ozoneHalfWidthKm = finiteFloat(
        value.at("ozoneHalfWidthKm"), field + ".ozoneHalfWidthKm");
    atmosphere.groundAlbedo =
        readVec3(value.at("groundAlbedo"), field + ".groundAlbedo");
    atmosphere.multipleScatteringFactor = finiteFloat(
        value.at("multipleScatteringFactor"),
        field + ".multipleScatteringFactor");
    atmosphere.aerialPerspectiveStartMeters = finiteFloat(
        value.at("aerialPerspectiveStartMeters"),
        field + ".aerialPerspectiveStartMeters");
    atmosphere.aerialPerspectiveDistanceScale = finiteFloat(
        value.at("aerialPerspectiveDistanceScale"),
        field + ".aerialPerspectiveDistanceScale");
    return atmosphere;
}

ReflectionProbeComponentDocument
parseReflectionProbe(const Json &value, const std::string &field) {
    requireOnlyKeys(value, field,
                    {"environment", "shape", "boxExtents",
                     "sphereRadius", "blendDistance", "priority",
                     "intensity", "boxProjection", "captureOffset"});
    ReflectionProbeComponentDocument probe;
    if (value.contains("environment") &&
        !value.at("environment").is_null()) {
        probe.environmentId = value.at("environment").get<std::string>();
    }
    probe.shape = readReflectionProbeShape(
        value.at("shape").get<std::string>(), field + ".shape");
    probe.boxExtents =
        readVec3(value.at("boxExtents"), field + ".boxExtents");
    probe.sphereRadius = finiteFloat(value.at("sphereRadius"),
                                     field + ".sphereRadius");
    probe.blendDistance = finiteFloat(value.at("blendDistance"),
                                      field + ".blendDistance");
    probe.priority = value.at("priority").get<int32_t>();
    probe.intensity =
        finiteFloat(value.at("intensity"), field + ".intensity");
    probe.boxProjection = value.at("boxProjection").get<bool>();
    probe.captureOffset =
        readVec3(value.at("captureOffset"), field + ".captureOffset");
    return probe;
}

SceneEntityDocument parseEntity(const Json &value, size_t index,
                                uint32_t sourceSchemaVersion) {
    const std::string field = "entities[" + std::to_string(index) + "]";
    requireOnlyKeys(value, field,
                    {"id", "name", "parent", "enabled", "transform",
                     "components"});
    SceneEntityDocument entity;
    entity.id = readEntityId(value.at("id"), field + ".id");
    entity.name = value.at("name").get<std::string>();
    if (!value.at("parent").is_null())
        entity.parent = readEntityId(value.at("parent"), field + ".parent");
    entity.enabled = value.value("enabled", true);
    entity.transform = parseTransform(value.at("transform"),
                                      field + ".transform");

    const Json &components = value.at("components");
    requireOnlyKeys(components, field + ".components",
                    sourceSchemaVersion >= 4
                        ? std::initializer_list<const char *>{
                              "modelInstance", "light", "camera",
                              "atmosphere", "reflectionProbe"}
                    : sourceSchemaVersion >= 3
                        ? std::initializer_list<const char *>{
                              "modelInstance", "light", "camera",
                              "atmosphere"}
                        : std::initializer_list<const char *>{
                              "modelInstance", "light", "camera"});
    if (components.contains("modelInstance")) {
        entity.modelInstance = parseModelInstance(
            components.at("modelInstance"), field + ".components.modelInstance");
    }
    if (components.contains("light"))
        entity.light = parseLight(components.at("light"),
                                  field + ".components.light",
                                  sourceSchemaVersion);
    if (components.contains("camera")) {
        entity.camera = parseCamera(components.at("camera"),
                                    field + ".components.camera");
    }
    if (sourceSchemaVersion >= 3 && components.contains("atmosphere")) {
        entity.atmosphere = parseAtmosphere(
            components.at("atmosphere"), field + ".components.atmosphere");
    }
    if (sourceSchemaVersion >= 4 &&
        components.contains("reflectionProbe")) {
        entity.reflectionProbe = parseReflectionProbe(
            components.at("reflectionProbe"),
            field + ".components.reflectionProbe");
    }
    return entity;
}

SceneDocument parseDocument(const Json &root) {
    requireOnlyKeys(root, "$",
                    {"schemaVersion", "id", "displayName", "activeCamera",
                     "ambient", "environment", "entities"});
    SceneDocument document;
    const uint32_t sourceSchemaVersion =
        root.at("schemaVersion").get<uint32_t>();
    if (sourceSchemaVersion < 1 ||
        sourceSchemaVersion > SceneDocument::kSchemaVersion) {
        throw documentError("schemaVersion", "unsupported schema");
    }
    document.schemaVersion = SceneDocument::kSchemaVersion;
    try {
        document.id = SceneDocumentId(root.at("id").get<std::string>());
    } catch (const std::invalid_argument &exception) {
        throw documentError("id", exception.what());
    }
    document.displayName = root.at("displayName").get<std::string>();
    if (!root.at("activeCamera").is_null()) {
        document.activeCamera =
            readEntityId(root.at("activeCamera"), "activeCamera");
    }

    const Json &ambient = root.at("ambient");
    requireOnlyKeys(ambient, "ambient", {"color", "intensity"});
    document.ambient.color = readVec3(ambient.at("color"), "ambient.color");
    document.ambient.intensity =
        finiteFloat(ambient.at("intensity"), "ambient.intensity");

    if (!root.at("environment").is_null()) {
        const Json &environment = root.at("environment");
        requireOnlyKeys(environment, "environment",
                        {"id", "intensity", "rotationRadians"});
        SceneEnvironmentDocument parsed;
        parsed.environmentId = environment.at("id").get<std::string>();
        parsed.intensity =
            finiteFloat(environment.at("intensity"), "environment.intensity");
        parsed.rotationRadians = finiteFloat(environment.at("rotationRadians"),
                                             "environment.rotationRadians");
        document.environment = std::move(parsed);
    }

    const Json &entities = root.at("entities");
    if (!entities.is_array())
        throw documentError("entities", "expected an array");
    document.entities.reserve(entities.size());
    for (size_t index = 0; index < entities.size(); ++index)
        document.entities.push_back(
            parseEntity(entities.at(index), index, sourceSchemaVersion));
    return document;
}

Json writeVec3(const glm::vec3 &value) {
    return Json::array({value.x, value.y, value.z});
}

Json writeQuaternion(const glm::quat &value) {
    return Json::array({value.x, value.y, value.z, value.w});
}

Json serializeDocument(const SceneDocument &document) {
    Json root = Json::object();
    root["schemaVersion"] = document.schemaVersion;
    root["id"] = document.id.value();
    root["displayName"] = document.displayName;
    root["activeCamera"] = document.activeCamera
                               ? Json(document.activeCamera->toString())
                               : Json(nullptr);
    root["ambient"] = {{"color", writeVec3(document.ambient.color)},
                       {"intensity", document.ambient.intensity}};
    if (document.environment) {
        root["environment"] = {
            {"id", document.environment->environmentId},
            {"intensity", document.environment->intensity},
            {"rotationRadians", document.environment->rotationRadians}};
    } else {
        root["environment"] = nullptr;
    }

    root["entities"] = Json::array();
    for (const SceneEntityDocument &entity : document.entities) {
        Json components = Json::object();
        if (entity.modelInstance) {
            components["modelInstance"] = {
                {"model", entity.modelInstance->model.value()}};
        }
        if (entity.light) {
            Json light = {{"type", lightTypeName(entity.light->type)},
                          {"castsShadow", entity.light->castsShadow},
                          {"atmosphereSunIndex",
                           entity.light->atmosphereSunIndex
                               ? Json(*entity.light->atmosphereSunIndex)
                               : Json(nullptr)},
                          {"sourceAngularRadiusRadians",
                           entity.light->sourceAngularRadiusRadians},
                          {"color", writeVec3(entity.light->color)},
                          {"intensity", entity.light->intensity}};
            light["range"] = entity.light->range
                                 ? Json(*entity.light->range)
                                 : Json(nullptr);
            light["innerConeRadians"] = entity.light->innerConeRadians;
            light["outerConeRadians"] = entity.light->outerConeRadians;
            components["light"] = std::move(light);
        }
        if (entity.camera) {
            components["camera"] = {
                {"verticalFovRadians", entity.camera->verticalFovRadians},
                {"nearPlane", entity.camera->nearPlane},
                {"farPlane", entity.camera->farPlane}};
        }
        if (entity.atmosphere) {
            const AtmosphereComponentDocument &a = *entity.atmosphere;
            components["atmosphere"] = {
                {"bottomRadiusKm", a.bottomRadiusKm},
                {"atmosphereHeightKm", a.atmosphereHeightKm},
                {"rayleighScatteringPerKm",
                 writeVec3(a.rayleighScatteringPerKm)},
                {"rayleighScaleHeightKm", a.rayleighScaleHeightKm},
                {"mieScatteringPerKm", a.mieScatteringPerKm},
                {"mieExtinctionPerKm", a.mieExtinctionPerKm},
                {"mieScaleHeightKm", a.mieScaleHeightKm},
                {"mieAnisotropy", a.mieAnisotropy},
                {"ozoneAbsorptionPerKm",
                 writeVec3(a.ozoneAbsorptionPerKm)},
                {"ozoneCenterHeightKm", a.ozoneCenterHeightKm},
                {"ozoneHalfWidthKm", a.ozoneHalfWidthKm},
                {"groundAlbedo", writeVec3(a.groundAlbedo)},
                {"multipleScatteringFactor", a.multipleScatteringFactor},
                {"aerialPerspectiveStartMeters",
                 a.aerialPerspectiveStartMeters},
                {"aerialPerspectiveDistanceScale",
                 a.aerialPerspectiveDistanceScale}};
        }
        if (entity.reflectionProbe) {
            const ReflectionProbeComponentDocument &probe =
                *entity.reflectionProbe;
            components["reflectionProbe"] = {
                {"environment",
                 probe.environmentId ? Json(*probe.environmentId)
                                     : Json(nullptr)},
                {"shape", reflectionProbeShapeName(probe.shape)},
                {"boxExtents", writeVec3(probe.boxExtents)},
                {"sphereRadius", probe.sphereRadius},
                {"blendDistance", probe.blendDistance},
                {"priority", probe.priority},
                {"intensity", probe.intensity},
                {"boxProjection", probe.boxProjection},
                {"captureOffset", writeVec3(probe.captureOffset)}};
        }

        Json item = Json::object();
        item["id"] = entity.id.toString();
        item["name"] = entity.name;
        item["parent"] = entity.parent ? Json(entity.parent->toString())
                                        : Json(nullptr);
        item["enabled"] = entity.enabled;
        item["transform"] = {
            {"translation", writeVec3(entity.transform.translation)},
            {"rotation", writeQuaternion(entity.transform.rotation)},
            {"scale", writeVec3(entity.transform.scale)}};
        item["components"] = std::move(components);
        root["entities"].push_back(std::move(item));
    }
    return root;
}

void validateFiniteVec3(const glm::vec3 &value, const std::string &field) {
    if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
        !std::isfinite(value.z)) {
        throw documentError(field, "values must be finite");
    }
}

std::filesystem::path temporaryPath(const std::filesystem::path &path) {
    return path.string() + ".write-" +
           std::to_string(std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()) +
           ".tmp";
}

} // namespace

SceneDocumentFileStamp
SceneDocumentService::fileStamp(const std::filesystem::path &path) {
    SceneDocumentFileStamp stamp;
    std::error_code error;
    stamp.exists = std::filesystem::is_regular_file(path, error);
    if (error || !stamp.exists)
        return stamp;
    stamp.size = std::filesystem::file_size(path, error);
    if (error)
        throw std::runtime_error("Could not read scene document size: " +
                                 path.string());
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        throw std::runtime_error("Could not read scene document timestamp: " +
                                 path.string());
    stamp.writeTime = writeTime.time_since_epoch().count();
    return stamp;
}

LoadedSceneDocument SceneDocumentService::load(
    const std::filesystem::path &path,
    const std::filesystem::path &projectRoot,
    const SceneDocumentReferences *references) {
    if (!pathWithin(projectRoot, path) || !hasSceneDocumentSuffix(path))
        throw std::runtime_error("Scene document path must be a project-local "
                                 ".vkscene.json file");
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("could not open file");
        Json root;
        input >> root;
        SceneDocument document = parseDocument(root);
        validate(document, references);
        return {std::move(document), path, fileStamp(path)};
    } catch (const std::runtime_error &) {
        throw;
    } catch (const std::exception &exception) {
        throw std::runtime_error("Could not load scene document '" +
                                 path.string() + "': " + exception.what());
    }
}

SceneDocument SceneDocumentService::createDefault(
    const std::string &id, const std::string &displayName) {
    SceneDocument document;
    document.id = SceneDocumentId(id);
    document.displayName = displayName;

    SceneEntityDocument camera;
    camera.id = PersistentEntityId::generate();
    camera.name = "Camera";
    camera.transform.translation = {0.0f, -5.0f, 2.0f};
    camera.transform.rotation = rotationLookingAlong(
        -camera.transform.translation, glm::vec3(0.0f, 0.0f, 1.0f));
    camera.camera = CameraComponentDocument{};
    document.activeCamera = camera.id;
    document.entities.push_back(camera);

    SceneEntityDocument sun;
    sun.id = PersistentEntityId::generate();
    sun.name = "Sun";
    sun.light = LightComponentDocument{};
    sun.light->type = SceneDocumentLightType::Directional;
    sun.light->intensity = 3.0f;
    sun.light->atmosphereSunIndex = 0;
    const glm::vec3 surfaceToSun =
        glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f));
    sun.transform.rotation = rotationLookingAlong(
        -surfaceToSun, glm::vec3(0.0f, 0.0f, 1.0f));
    document.entities.push_back(sun);

    SceneEntityDocument atmosphere;
    atmosphere.id = PersistentEntityId::generate();
    atmosphere.name = "Sky Atmosphere";
    atmosphere.atmosphere = AtmosphereComponentDocument{};
    document.entities.push_back(atmosphere);

    validate(document);
    return document;
}

void SceneDocumentService::validate(
    const SceneDocument &document,
    const SceneDocumentReferences *references) {
    if (document.schemaVersion != SceneDocument::kSchemaVersion)
        throw documentError("schemaVersion", "unsupported schema");
    if (document.id.empty())
        throw documentError("id", "cannot be empty");
    if (document.displayName.empty())
        throw documentError("displayName", "cannot be empty");
    validateFiniteVec3(document.ambient.color, "ambient.color");
    if (glm::any(glm::lessThan(document.ambient.color, glm::vec3(0.0f))))
        throw documentError("ambient.color", "values cannot be negative");
    if (!std::isfinite(document.ambient.intensity) ||
        document.ambient.intensity < 0.0f)
        throw documentError("ambient.intensity", "must be finite and non-negative");
    if (document.environment) {
        if (!isStableAssetId(document.environment->environmentId))
            throw documentError("environment.id", "invalid stable ID");
        if (!std::isfinite(document.environment->intensity) ||
            document.environment->intensity < 0.0f)
            throw documentError("environment.intensity",
                                "must be finite and non-negative");
        if (!std::isfinite(document.environment->rotationRadians))
            throw documentError("environment.rotationRadians", "must be finite");
        if (references && references->environmentIds.count(
                              document.environment->environmentId) == 0) {
            throw documentError("environment.id", "unknown Catalog environment");
        }
    }

    std::unordered_map<PersistentEntityId, size_t, PersistentEntityIdHash>
        indices;
    size_t atmosphereCount = 0;
    size_t atmosphereSunCount = 0;
    for (size_t index = 0; index < document.entities.size(); ++index) {
        const SceneEntityDocument &entity = document.entities[index];
        const std::string field = "entities[" + std::to_string(index) + "]";
        if (entity.id.empty())
            throw documentError(field + ".id", "cannot be zero");
        if (!indices.emplace(entity.id, index).second)
            throw documentError(field + ".id", "duplicate UUID");
        if (entity.name.empty())
            throw documentError(field + ".name", "cannot be empty");
        validateFiniteVec3(entity.transform.translation,
                           field + ".transform.translation");
        validateFiniteVec3(entity.transform.scale, field + ".transform.scale");
        if (std::abs(entity.transform.scale.x) <= kEpsilon ||
            std::abs(entity.transform.scale.y) <= kEpsilon ||
            std::abs(entity.transform.scale.z) <= kEpsilon)
            throw documentError(field + ".transform.scale",
                                "components cannot be near zero");
        const float quaternionLength = glm::length(entity.transform.rotation);
        if (!std::isfinite(quaternionLength) || quaternionLength <= kEpsilon)
            throw documentError(field + ".transform.rotation",
                                "quaternion length is too small");

        if (entity.modelInstance && references &&
            references->modelIds.count(entity.modelInstance->model.value()) == 0) {
            throw documentError(field + ".components.modelInstance.model",
                                "unknown Catalog model");
        }
        if (entity.light) {
            validateFiniteVec3(entity.light->color,
                               field + ".components.light.color");
            if (glm::any(glm::lessThan(entity.light->color, glm::vec3(0.0f))))
                throw documentError(field + ".components.light.color",
                                    "values cannot be negative");
            if (!std::isfinite(entity.light->intensity) ||
                entity.light->intensity < 0.0f)
                throw documentError(field + ".components.light.intensity",
                                    "must be finite and non-negative");
            if (entity.light->range &&
                (!std::isfinite(*entity.light->range) ||
                 *entity.light->range <= 0.0f))
                throw documentError(field + ".components.light.range",
                                    "must be finite and positive");
            if (!std::isfinite(entity.light->innerConeRadians) ||
                !std::isfinite(entity.light->outerConeRadians))
                throw documentError(field + ".components.light",
                                    "cone angles must be finite");
            if (entity.light->castsShadow &&
                entity.light->type != SceneDocumentLightType::Directional)
                throw documentError(field + ".components.light.castsShadow",
                                    "only directional lights can cast shadows");
            if (!std::isfinite(entity.light->sourceAngularRadiusRadians) ||
                entity.light->sourceAngularRadiusRadians <= 0.0f ||
                entity.light->sourceAngularRadiusRadians > 0.1f) {
                throw documentError(
                    field + ".components.light.sourceAngularRadiusRadians",
                    "must be finite and in the range (0, 0.1]");
            }
            if (entity.light->atmosphereSunIndex) {
                if (*entity.light->atmosphereSunIndex != 0) {
                    throw documentError(
                        field + ".components.light.atmosphereSunIndex",
                        "v1 only supports atmosphere Sun index 0");
                }
                if (entity.light->type !=
                    SceneDocumentLightType::Directional) {
                    throw documentError(
                        field + ".components.light.atmosphereSunIndex",
                        "only directional lights can be an atmosphere Sun");
                }
                if (++atmosphereSunCount > 1) {
                    throw documentError(
                        field + ".components.light.atmosphereSunIndex",
                        "only one atmosphere Sun is supported");
                }
            }
            if (entity.light->type == SceneDocumentLightType::Spot &&
                (entity.light->innerConeRadians < 0.0f ||
                 entity.light->outerConeRadians <
                     entity.light->innerConeRadians ||
                 entity.light->outerConeRadians > kHalfPi))
                throw documentError(field + ".components.light",
                                    "invalid spot cone angles");
        }
        if (entity.camera) {
            if (!std::isfinite(entity.camera->verticalFovRadians) ||
                entity.camera->verticalFovRadians <= 0.0f ||
                entity.camera->verticalFovRadians >= kPi)
                throw documentError(field + ".components.camera.verticalFovRadians",
                                    "must be between zero and pi");
            if (!std::isfinite(entity.camera->nearPlane) ||
                !std::isfinite(entity.camera->farPlane) ||
                entity.camera->nearPlane <= 0.0f ||
                entity.camera->farPlane <= entity.camera->nearPlane)
                throw documentError(field + ".components.camera",
                                    "expected 0 < nearPlane < farPlane");
        }
        if (entity.atmosphere) {
            if (++atmosphereCount > 1) {
                throw documentError(field + ".components.atmosphere",
                                    "only one atmosphere is supported");
            }
            if (entity.parent) {
                throw documentError(field + ".parent",
                                    "an atmosphere entity must be at the scene root");
            }
            const glm::quat rotation =
                glm::normalize(entity.transform.rotation);
            if (std::abs(rotation.w - 1.0f) > 1.0e-4f ||
                std::abs(rotation.x) > 1.0e-4f ||
                std::abs(rotation.y) > 1.0e-4f ||
                std::abs(rotation.z) > 1.0e-4f) {
                throw documentError(field + ".transform.rotation",
                                    "an atmosphere entity must use identity rotation");
            }
            if (glm::any(glm::greaterThan(
                    glm::abs(entity.transform.scale - glm::vec3(1.0f)),
                    glm::vec3(1.0e-4f)))) {
                throw documentError(field + ".transform.scale",
                                    "an atmosphere entity must use unit scale");
            }
            const AtmosphereComponentDocument &a = *entity.atmosphere;
            validateFiniteVec3(a.rayleighScatteringPerKm,
                               field + ".components.atmosphere.rayleighScatteringPerKm");
            validateFiniteVec3(a.ozoneAbsorptionPerKm,
                               field + ".components.atmosphere.ozoneAbsorptionPerKm");
            validateFiniteVec3(a.groundAlbedo,
                               field + ".components.atmosphere.groundAlbedo");
            if (!std::isfinite(a.bottomRadiusKm) || a.bottomRadiusKm <= 0.0f ||
                !std::isfinite(a.atmosphereHeightKm) ||
                a.atmosphereHeightKm <= 0.0f ||
                !std::isfinite(a.rayleighScaleHeightKm) ||
                a.rayleighScaleHeightKm <= 0.0f ||
                !std::isfinite(a.mieScatteringPerKm) ||
                a.mieScatteringPerKm < 0.0f ||
                !std::isfinite(a.mieExtinctionPerKm) ||
                a.mieExtinctionPerKm < a.mieScatteringPerKm ||
                !std::isfinite(a.mieScaleHeightKm) ||
                a.mieScaleHeightKm <= 0.0f ||
                !std::isfinite(a.mieAnisotropy) || a.mieAnisotropy < -0.99f ||
                a.mieAnisotropy > 0.99f ||
                !std::isfinite(a.ozoneCenterHeightKm) ||
                a.ozoneCenterHeightKm < 0.0f ||
                !std::isfinite(a.ozoneHalfWidthKm) ||
                a.ozoneHalfWidthKm <= 0.0f ||
                !std::isfinite(a.multipleScatteringFactor) ||
                a.multipleScatteringFactor < 0.0f ||
                !std::isfinite(a.aerialPerspectiveStartMeters) ||
                a.aerialPerspectiveStartMeters < 0.0f ||
                !std::isfinite(a.aerialPerspectiveDistanceScale) ||
                a.aerialPerspectiveDistanceScale <= 0.0f) {
                throw documentError(field + ".components.atmosphere",
                                    "contains an invalid physical parameter");
            }
            if (glm::any(glm::lessThan(a.rayleighScatteringPerKm,
                                       glm::vec3(0.0f))) ||
                glm::any(glm::lessThan(a.ozoneAbsorptionPerKm,
                                       glm::vec3(0.0f))) ||
                glm::any(glm::lessThan(a.groundAlbedo, glm::vec3(0.0f))) ||
                glm::any(glm::greaterThan(a.groundAlbedo,
                                          glm::vec3(1.0f)))) {
                throw documentError(field + ".components.atmosphere",
                                    "scattering must be non-negative and albedo must be in [0, 1]");
            }
        }
        if (entity.reflectionProbe) {
            const ReflectionProbeComponentDocument &probe =
                *entity.reflectionProbe;
            const std::string probeField =
                field + ".components.reflectionProbe";
            validateFiniteVec3(probe.boxExtents,
                               probeField + ".boxExtents");
            validateFiniteVec3(probe.captureOffset,
                               probeField + ".captureOffset");
            if (glm::any(glm::lessThanEqual(probe.boxExtents,
                                            glm::vec3(kEpsilon)))) {
                throw documentError(probeField + ".boxExtents",
                                    "components must be positive");
            }
            if (!std::isfinite(probe.sphereRadius) ||
                probe.sphereRadius <= kEpsilon) {
                throw documentError(probeField + ".sphereRadius",
                                    "must be finite and positive");
            }
            if (!std::isfinite(probe.blendDistance) ||
                probe.blendDistance < 0.0f) {
                throw documentError(probeField + ".blendDistance",
                                    "must be finite and non-negative");
            }
            if (!std::isfinite(probe.intensity) ||
                probe.intensity < 0.0f) {
                throw documentError(probeField + ".intensity",
                                    "must be finite and non-negative");
            }
            if (probe.environmentId) {
                if (!isStableAssetId(*probe.environmentId)) {
                    throw documentError(probeField + ".environment",
                                        "invalid stable ID");
                }
                if (references &&
                    references->environmentIds.count(
                        *probe.environmentId) == 0) {
                    throw documentError(probeField + ".environment",
                                        "unknown Catalog environment");
                }
            }
        }
    }

    if (atmosphereSunCount > 0 && atmosphereCount == 0) {
        throw documentError("entities",
                            "an atmosphere Sun requires an atmosphere component");
    }

    std::vector<uint8_t> visit(document.entities.size(), 0);
    const auto visitParent = [&](auto &&self, size_t index) -> void {
        if (visit[index] == 2)
            return;
        if (visit[index] == 1)
            throw documentError("entities", "parent hierarchy contains a cycle");
        visit[index] = 1;
        const auto &parent = document.entities[index].parent;
        if (parent) {
            const auto found = indices.find(*parent);
            if (found == indices.end())
                throw documentError("entities[" + std::to_string(index) +
                                        "].parent",
                                    "parent UUID does not exist");
            self(self, found->second);
        }
        visit[index] = 2;
    };
    for (size_t index = 0; index < document.entities.size(); ++index)
        visitParent(visitParent, index);

    if (!document.activeCamera)
        throw documentError("activeCamera",
                            "must reference an entity with a camera component");
    const auto found = indices.find(*document.activeCamera);
    if (found == indices.end() || !document.entities[found->second].camera)
        throw documentError("activeCamera",
                            "must reference an entity with a camera component");
}

SceneDocumentFileStamp SceneDocumentService::saveAtomic(
    const std::filesystem::path &path,
    const std::filesystem::path &projectRoot, const SceneDocument &document,
    std::optional<SceneDocumentFileStamp> expectedStamp) {
    if (!pathWithin(projectRoot, path) || !hasSceneDocumentSuffix(path))
        throw std::runtime_error("Scene document path must be a project-local "
                                 ".vkscene.json file");
    validate(document);

    const SceneDocumentFileStamp current = fileStamp(path);
    if (expectedStamp) {
        if (current != *expectedStamp)
            throw std::runtime_error("scene_changed_on_disk");
    } else if (current.exists) {
        throw std::runtime_error("scene_changed_on_disk");
    }

    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path temporary = temporaryPath(path);
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Could not create temporary scene document");
        output << serializeDocument(document).dump(2) << '\n';
        output.flush();
        if (!output)
            throw std::runtime_error("Could not flush temporary scene document");
        output.close();

        if (fileStamp(path) != current)
            throw std::runtime_error("scene_changed_on_disk");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(
                "Could not atomically replace scene document (error " +
                std::to_string(GetLastError()) + ")");
        }
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return fileStamp(path);
}

} // namespace vkr
