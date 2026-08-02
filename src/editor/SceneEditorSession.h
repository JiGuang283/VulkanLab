#pragma once

#include "scene/RuntimeWorld.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

enum class EditorCameraMode {
    Editor,
    ActiveScene,
};

class EditorCommandStack {
  public:
    struct Command {
        std::string label;
        SceneDocument before;
        SceneDocument after;
        uint64_t beforeStateId = 0;
        uint64_t afterStateId = 0;
    };

    void reset();
    void push(std::string label, SceneDocument before,
              SceneDocument after);
    bool undo(RuntimeWorld &world);
    bool redo(RuntimeWorld &world);
    void markSaved();

    bool canUndo() const { return cursor_ > 0; }
    bool canRedo() const { return cursor_ < commands_.size(); }
    bool dirty() const { return currentStateId_ != savedStateId_; }
    const char *undoLabel() const;
    const char *redoLabel() const;

  private:
    static constexpr size_t kMaxCommands = 256;

    std::vector<Command> commands_;
    size_t cursor_ = 0;
    uint64_t nextStateId_ = 1;
    uint64_t currentStateId_ = 0;
    uint64_t savedStateId_ = 0;
};

class SceneEditorSession {
  public:
    using WorldChangedCallback = std::function<void()>;

    void attach(std::shared_ptr<RuntimeWorld> world,
                std::filesystem::path documentPath,
                SceneDocumentFileStamp sourceStamp);
    void detach();

    bool active() const { return world_ != nullptr; }
    std::shared_ptr<RuntimeWorld> world() const { return world_; }
    const std::filesystem::path &documentPath() const {
        return documentPath_;
    }
    SceneDocumentFileStamp sourceStamp() const { return sourceStamp_; }

    const std::optional<PersistentEntityId> &selection() const {
        return selection_;
    }
    void select(std::optional<PersistentEntityId> id);

    EditorCameraMode cameraMode() const { return cameraMode_; }
    void setCameraMode(EditorCameraMode mode) { cameraMode_ = mode; }

    bool dirty() const { return commands_.dirty(); }
    bool canUndo() const { return commands_.canUndo(); }
    bool canRedo() const { return commands_.canRedo(); }
    bool continuousEditActive() const { return continuousBefore_.has_value(); }
    const char *undoLabel() const { return commands_.undoLabel(); }
    const char *redoLabel() const { return commands_.redoLabel(); }

    bool execute(const std::string &label,
                 const std::function<bool(RuntimeWorld &)> &mutation);
    void beginContinuousEdit(std::string label);
    bool commitContinuousEdit();
    void cancelContinuousEdit();
    bool undo();
    bool redo();
    void discardChanges();

    void save(const std::filesystem::path &projectRoot,
              const SceneDocumentReferences &references);
    void saveAs(const std::filesystem::path &path,
                const std::filesystem::path &projectRoot,
                SceneDocumentId id, std::string displayName,
                const SceneDocumentReferences &references);
    void reload(const std::filesystem::path &projectRoot,
                const SceneDocumentReferences &references);

    void setWorldChangedCallback(WorldChangedCallback callback) {
        worldChanged_ = std::move(callback);
    }

  private:
    void notifyWorldChanged();
    void repairSelection();

    std::shared_ptr<RuntimeWorld> world_;
    std::filesystem::path documentPath_;
    SceneDocumentFileStamp sourceStamp_{};
    std::optional<PersistentEntityId> selection_;
    EditorCameraMode cameraMode_ = EditorCameraMode::Editor;
    EditorCommandStack commands_;
    SceneDocument savedDocument_;
    bool hasSavedDocument_ = false;
    std::optional<SceneDocument> continuousBefore_;
    std::string continuousLabel_;
    WorldChangedCallback worldChanged_;
};

} // namespace vkr
