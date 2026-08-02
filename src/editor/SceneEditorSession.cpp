#include "SceneEditorSession.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vkr {

void EditorCommandStack::reset() {
    commands_.clear();
    cursor_ = 0;
    nextStateId_ = 1;
    currentStateId_ = 0;
    savedStateId_ = 0;
}

void EditorCommandStack::push(std::string label, SceneDocument before,
                              SceneDocument after) {
    if (cursor_ < commands_.size())
        commands_.erase(commands_.begin() +
                            static_cast<std::ptrdiff_t>(cursor_),
                        commands_.end());
    Command command;
    command.label = std::move(label);
    command.before = std::move(before);
    command.after = std::move(after);
    command.beforeStateId = currentStateId_;
    command.afterStateId = nextStateId_++;
    commands_.push_back(std::move(command));
    cursor_ = commands_.size();
    currentStateId_ = commands_.back().afterStateId;
    if (commands_.size() > kMaxCommands) {
        const size_t removeCount = commands_.size() - kMaxCommands;
        commands_.erase(commands_.begin(),
                        commands_.begin() +
                            static_cast<std::ptrdiff_t>(removeCount));
        cursor_ -= removeCount;
    }
}

bool EditorCommandStack::undo(RuntimeWorld &world) {
    if (!canUndo())
        return false;
    const Command &command = commands_[cursor_ - 1];
    world.replaceDocument(command.before);
    currentStateId_ = command.beforeStateId;
    --cursor_;
    return true;
}

bool EditorCommandStack::redo(RuntimeWorld &world) {
    if (!canRedo())
        return false;
    const Command &command = commands_[cursor_];
    world.replaceDocument(command.after);
    currentStateId_ = command.afterStateId;
    ++cursor_;
    return true;
}

void EditorCommandStack::markSaved() { savedStateId_ = currentStateId_; }

const char *EditorCommandStack::undoLabel() const {
    return canUndo() ? commands_[cursor_ - 1].label.c_str() : nullptr;
}

const char *EditorCommandStack::redoLabel() const {
    return canRedo() ? commands_[cursor_].label.c_str() : nullptr;
}

void SceneEditorSession::attach(std::shared_ptr<RuntimeWorld> world,
                                std::filesystem::path documentPath,
                                SceneDocumentFileStamp sourceStamp) {
    if (!world)
        throw std::invalid_argument("Editor session requires RuntimeWorld");
    world_ = std::move(world);
    documentPath_ = std::move(documentPath);
    sourceStamp_ = sourceStamp;
    selection_.reset();
    cameraMode_ = EditorCameraMode::Editor;
    commands_.reset();
    savedDocument_ = world_->toDocument();
    hasSavedDocument_ = true;
    continuousBefore_.reset();
    continuousLabel_.clear();
}

void SceneEditorSession::detach() {
    world_.reset();
    documentPath_.clear();
    sourceStamp_ = {};
    selection_.reset();
    cameraMode_ = EditorCameraMode::Editor;
    commands_.reset();
    savedDocument_ = {};
    hasSavedDocument_ = false;
    continuousBefore_.reset();
    continuousLabel_.clear();
}

void SceneEditorSession::select(std::optional<PersistentEntityId> id) {
    if (id && (!world_ || !world_->find(*id)))
        id.reset();
    selection_ = std::move(id);
}

bool SceneEditorSession::execute(
    const std::string &label,
    const std::function<bool(RuntimeWorld &)> &mutation) {
    if (!world_ || continuousBefore_)
        return false;
    SceneDocument before = world_->toDocument();
    if (!mutation(*world_)) {
        world_->replaceDocument(before);
        notifyWorldChanged();
        return false;
    }
    try {
        SceneDocument after = world_->toDocument();
        SceneDocumentService::validate(after);
        commands_.push(label, std::move(before), std::move(after));
        repairSelection();
        notifyWorldChanged();
        return true;
    } catch (...) {
        world_->replaceDocument(before);
        repairSelection();
        notifyWorldChanged();
        throw;
    }
}

void SceneEditorSession::beginContinuousEdit(std::string label) {
    if (!world_ || continuousBefore_)
        return;
    continuousBefore_ = world_->toDocument();
    continuousLabel_ = std::move(label);
}

bool SceneEditorSession::commitContinuousEdit() {
    if (!world_ || !continuousBefore_)
        return false;
    SceneDocument before = std::move(*continuousBefore_);
    continuousBefore_.reset();
    try {
        SceneDocument after = world_->toDocument();
        SceneDocumentService::validate(after);
        commands_.push(std::move(continuousLabel_), std::move(before),
                       std::move(after));
        continuousLabel_.clear();
        repairSelection();
        notifyWorldChanged();
        return true;
    } catch (...) {
        world_->replaceDocument(before);
        continuousLabel_.clear();
        repairSelection();
        notifyWorldChanged();
        throw;
    }
}

void SceneEditorSession::cancelContinuousEdit() {
    if (!world_ || !continuousBefore_)
        return;
    world_->replaceDocument(*continuousBefore_);
    continuousBefore_.reset();
    continuousLabel_.clear();
    repairSelection();
    notifyWorldChanged();
}

bool SceneEditorSession::undo() {
    if (!world_ || !commands_.undo(*world_))
        return false;
    repairSelection();
    notifyWorldChanged();
    return true;
}

bool SceneEditorSession::redo() {
    if (!world_ || !commands_.redo(*world_))
        return false;
    repairSelection();
    notifyWorldChanged();
    return true;
}

void SceneEditorSession::discardChanges() {
    if (!world_ || !hasSavedDocument_)
        return;
    world_->replaceDocument(savedDocument_);
    commands_.reset();
    continuousBefore_.reset();
    continuousLabel_.clear();
    repairSelection();
    notifyWorldChanged();
}

void SceneEditorSession::save(
    const std::filesystem::path &projectRoot,
    const SceneDocumentReferences &references) {
    if (!world_ || documentPath_.empty())
        throw std::runtime_error("No native scene is open for editing");
    SceneDocument document = world_->toDocument();
    SceneDocumentService::validate(document, &references);
    sourceStamp_ = SceneDocumentService::saveAtomic(
        documentPath_, projectRoot, document, sourceStamp_);
    savedDocument_ = std::move(document);
    hasSavedDocument_ = true;
    commands_.markSaved();
}

void SceneEditorSession::saveAs(
    const std::filesystem::path &path,
    const std::filesystem::path &projectRoot, SceneDocumentId id,
    std::string displayName, const SceneDocumentReferences &references) {
    if (!world_)
        throw std::runtime_error("No native scene is open for editing");
    SceneDocument document = world_->toDocument();
    document.id = std::move(id);
    document.displayName = std::move(displayName);
    SceneDocumentService::validate(document, &references);
    const SceneDocumentFileStamp stamp = SceneDocumentService::saveAtomic(
        path, projectRoot, document, std::nullopt);
    world_->replaceDocument(document);
    documentPath_ = path;
    sourceStamp_ = stamp;
    savedDocument_ = std::move(document);
    hasSavedDocument_ = true;
    commands_.reset();
    notifyWorldChanged();
}

void SceneEditorSession::reload(
    const std::filesystem::path &projectRoot,
    const SceneDocumentReferences &references) {
    if (!world_ || documentPath_.empty())
        throw std::runtime_error("No native scene is open for editing");
    const LoadedSceneDocument loaded = SceneDocumentService::load(
        documentPath_, projectRoot, &references);
    world_->replaceDocument(loaded.document);
    sourceStamp_ = loaded.sourceStamp;
    savedDocument_ = loaded.document;
    hasSavedDocument_ = true;
    commands_.reset();
    repairSelection();
    notifyWorldChanged();
}

void SceneEditorSession::notifyWorldChanged() {
    if (worldChanged_)
        worldChanged_();
}

void SceneEditorSession::repairSelection() {
    if (selection_ && (!world_ || !world_->find(*selection_)))
        selection_.reset();
}

} // namespace vkr
