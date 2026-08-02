#include "SceneLoadTask.h"

namespace vkr {

const char *sceneLoadStateName(SceneLoadState state) {
    switch (state) {
    case SceneLoadState::Queued:
        return "Queued";
    case SceneLoadState::PreparingCpu:
        return "PreparingCpu";
    case SceneLoadState::ReadyForUpload:
        return "ReadyForUpload";
    case SceneLoadState::ReleasingPreviousScene:
        return "ReleasingPreviousScene";
    case SceneLoadState::Uploading:
        return "Uploading";
    case SceneLoadState::WaitingForGpu:
        return "WaitingForGpu";
    case SceneLoadState::ReadyToPublish:
        return "ReadyToPublish";
    case SceneLoadState::Completed:
        return "Completed";
    case SceneLoadState::Cancelling:
        return "Cancelling";
    case SceneLoadState::Cancelled:
        return "Cancelled";
    case SceneLoadState::Failed:
        return "Failed";
    }
    return "Unknown";
}

const char *sceneLoadKindName(SceneLoadKind kind) {
    switch (kind) {
    case SceneLoadKind::ModelPreview:
        return "modelPreview";
    case SceneLoadKind::NativeScene:
        return "nativeScene";
    }
    return "unknown";
}

const char *sceneLoadPhaseName(SceneLoadPhase phase) {
    switch (phase) {
    case SceneLoadPhase::Queued:
        return "queued";
    case SceneLoadPhase::PreparingModel:
        return "preparingModel";
    case SceneLoadPhase::UploadingModel:
        return "uploadingModel";
    case SceneLoadPhase::ParsingDocument:
        return "parsingDocument";
    case SceneLoadPhase::ResolvingModels:
        return "resolvingModels";
    case SceneLoadPhase::LoadingModels:
        return "loadingModels";
    case SceneLoadPhase::LoadingEnvironment:
        return "loadingEnvironment";
    case SceneLoadPhase::PublishingWorld:
        return "publishingWorld";
    case SceneLoadPhase::Complete:
        return "complete";
    }
    return "unknown";
}

bool isTerminalSceneLoadState(SceneLoadState state) {
    return state == SceneLoadState::Completed ||
           state == SceneLoadState::Cancelled ||
           state == SceneLoadState::Failed;
}

} // namespace vkr
