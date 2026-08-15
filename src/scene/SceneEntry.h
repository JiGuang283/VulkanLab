#pragma once

#include "ModelPrepareFactory.h"

#include <string>

namespace vkr {

enum class SceneEntryKind {
    ModelPreview,
    NativeScene,
};

const char *sceneEntryKindName(SceneEntryKind kind);

struct SceneEntry {
    SceneEntryKind kind = SceneEntryKind::ModelPreview;
    std::string name;
    ModelPrepareFactory prepareFactory;
    std::string id;
    std::string profileId;
    std::string sourcePath;
    bool available = true;
    std::string unavailableReason;

    bool supportsBackgroundPrepare() const {
        return static_cast<bool>(prepareFactory);
    }
    bool isModelPreview() const {
        return kind == SceneEntryKind::ModelPreview;
    }
    bool isNativeScene() const {
        return kind == SceneEntryKind::NativeScene;
    }
};

} // namespace vkr
