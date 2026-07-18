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

bool isTerminalSceneLoadState(SceneLoadState state) {
    return state == SceneLoadState::Completed ||
           state == SceneLoadState::Cancelled ||
           state == SceneLoadState::Failed;
}

} // namespace vkr
