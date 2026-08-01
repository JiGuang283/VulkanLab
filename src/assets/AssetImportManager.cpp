#include "AssetImportManager.h"

#include "diagnostics/Profiling.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace vkr {
namespace {

constexpr size_t kMaxProgressLineBytes = 64 * 1024;

struct ScopedHandle {
    HANDLE value = nullptr;
    ~ScopedHandle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};

struct ScopedAttributeList {
    std::vector<uint8_t> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ~ScopedAttributeList() {
        if (value)
            DeleteProcThreadAttributeList(value);
    }
};

std::wstring quoteWindowsArgument(const std::wstring &argument) {
    if (argument.empty())
        return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::filesystem::path resolveAssetTool(
    const std::filesystem::path &requested) {
    if (!requested.empty()) {
        const auto absolute = std::filesystem::absolute(requested);
        if (!std::filesystem::is_regular_file(absolute))
            throw std::runtime_error("asset tool not found: " +
                                     absolute.string());
        return absolute;
    }
    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size())
        throw std::runtime_error("could not resolve VulkanLab executable");
    const std::filesystem::path sibling =
        std::filesystem::path(modulePath.data()).parent_path() /
        L"VulkanLabAssetTool.exe";
    if (!std::filesystem::is_regular_file(sibling)) {
        throw std::runtime_error(
            "VulkanLabAssetTool.exe is missing beside VulkanLab.exe");
    }
    return sibling;
}

uint64_t unsignedValue(const nlohmann::json &event, const char *name,
                       uint64_t fallback = 0) {
    const auto found = event.find(name);
    if (found == event.end())
        return fallback;
    if (found->is_number_unsigned())
        return found->get<uint64_t>();
    if (found->is_number_integer()) {
        const int64_t value = found->get<int64_t>();
        return value >= 0 ? static_cast<uint64_t>(value) : fallback;
    }
    return fallback;
}

} // namespace

const char *assetImportStateName(AssetImportState state) {
    switch (state) {
    case AssetImportState::Queued:
        return "Queued";
    case AssetImportState::Scanning:
        return "Scanning";
    case AssetImportState::Importing:
        return "Importing";
    case AssetImportState::Publishing:
        return "Publishing";
    case AssetImportState::Completed:
        return "Completed";
    case AssetImportState::Failed:
        return "Failed";
    case AssetImportState::Cancelling:
        return "Cancelling";
    case AssetImportState::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

const char *assetImportKindName(AssetImportKind kind) {
    switch (kind) {
    case AssetImportKind::SceneTextures:
        return "SceneTextures";
    case AssetImportKind::Environment:
        return "Environment";
    case AssetImportKind::SceneValidation:
        return "SceneValidation";
    }
    return "Unknown";
}

bool isTerminalAssetImportState(AssetImportState state) {
    return state == AssetImportState::Completed ||
           state == AssetImportState::Failed ||
           state == AssetImportState::Cancelled;
}

AssetImportManager::AssetImportManager(AssetImportManagerOptions options,
                                       AssetImportExecutor executor)
    : options_(std::move(options)), executor_(std::move(executor)) {
    if (!executor_)
        executor_ = runAssetImportProcess;
    if (options_.memoryBudgetMiB == 0)
        throw std::invalid_argument("asset import memory budget must be nonzero");
    worker_ = std::thread(&AssetImportManager::workerLoop, this);
}

AssetImportManager::~AssetImportManager() { shutdown(); }

std::shared_ptr<AssetImportTask>
AssetImportManager::request(const AssetImportRequest &request) {
    if (request.kind == AssetImportKind::SceneValidation) {
        if (request.sceneId.empty() && request.sourcePath.empty())
            throw std::invalid_argument(
                "scene validation requires sceneId or sourcePath");
    } else if (request.sceneId.empty() || request.profileId.empty()) {
        throw std::invalid_argument("sceneId and profileId are required");
    }
    std::lock_guard lock(mutex_);
    if (stopping_)
        throw std::runtime_error("AssetImportManager is shutting down");
    for (const auto &pair : tasks_) {
        const auto &existing = pair.second;
        if (existing->sceneId == request.sceneId &&
            existing->profileId == request.profileId &&
            existing->kind == request.kind &&
            existing->sourcePath == request.sourcePath &&
            !isTerminalAssetImportState(existing->state.load())) {
            return existing;
        }
    }

    auto task = std::make_shared<AssetImportTask>();
    task->id = nextTaskId_++;
    task->sceneId = request.sceneId;
    task->profileId = request.profileId;
    task->reason = request.reason;
    task->force = request.force;
    task->kind = request.kind;
    task->sourcePath = request.sourcePath;
    task->logPath =
        options_.cacheRoot / "logs" /
        ((request.kind == AssetImportKind::Environment
              ? "environment-"
              : request.kind == AssetImportKind::SceneValidation
                    ? "validation-"
                    : "import-") +
         std::to_string(task->id) + ".log");
    tasks_.emplace(task->id, task);
    historyIds_.push_back(task->id);
    pending_.push_back(task);
    pruneHistoryLocked();
    changed_.notify_one();
    return task;
}

bool AssetImportManager::cancel(uint64_t taskId) {
    std::lock_guard lock(mutex_);
    const auto found = tasks_.find(taskId);
    if (found == tasks_.end() ||
        isTerminalAssetImportState(found->second->state.load()))
        return false;
    found->second->cancellation->store(true);
    if (found->second == active_)
        found->second->state = AssetImportState::Cancelling;
    else {
        found->second->state = AssetImportState::Cancelled;
        found->second->completedAt = std::chrono::steady_clock::now();
    }
    changed_.notify_all();
    return true;
}

std::shared_ptr<AssetImportTask>
AssetImportManager::task(uint64_t taskId) const {
    std::lock_guard lock(mutex_);
    const auto found = tasks_.find(taskId);
    return found == tasks_.end() ? nullptr : found->second;
}

std::shared_ptr<AssetImportTask> AssetImportManager::activeTask() const {
    std::lock_guard lock(mutex_);
    return active_;
}

std::vector<std::shared_ptr<AssetImportTask>>
AssetImportManager::history() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<AssetImportTask>> result;
    result.reserve(historyIds_.size());
    for (auto it = historyIds_.rbegin(); it != historyIds_.rend(); ++it) {
        const auto found = tasks_.find(*it);
        if (found != tasks_.end())
            result.push_back(found->second);
    }
    return result;
}

void AssetImportManager::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        for (auto &pair : tasks_) {
            if (!isTerminalAssetImportState(pair.second->state.load()))
                pair.second->cancellation->store(true);
        }
    }
    changed_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void AssetImportManager::applyEvent(
    const std::shared_ptr<AssetImportTask> &task,
    const nlohmann::json &event) {
    const std::string type = event.value("event", std::string{});
    if (type == "started") {
        if (!event.contains("protocolVersion") ||
            !event["protocolVersion"].is_number_integer() ||
            event["protocolVersion"].get<int>() != 1) {
            throw std::runtime_error(
                "unsupported asset import progress protocol");
        }
        {
            std::lock_guard lock(task->mutex);
            task->protocolValidated = true;
        }
        task->totalArtifacts = unsignedValue(event, "total");
        task->reusedArtifacts = unsignedValue(event, "reused");
        task->workers = static_cast<uint32_t>(unsignedValue(event, "workers"));
        task->state = AssetImportState::Importing;
    } else if (type == "artifact_started") {
        task->state = AssetImportState::Importing;
        task->activeImage = unsignedValue(event, "image", UINT64_MAX);
        task->estimatedMemoryBytes = unsignedValue(event, "estimatedBytes");
    } else if (type == "progress") {
        task->completedArtifacts = unsignedValue(event, "completed");
        task->encodedArtifacts = unsignedValue(event, "encoded");
        task->reusedArtifacts = unsignedValue(event, "reused");
        task->failedArtifacts = unsignedValue(event, "failed");
    } else if (type == "publishing") {
        task->state = AssetImportState::Publishing;
    } else if (type == "validation") {
        const auto found = event.find("validation");
        if (found == event.end() || !found->is_object())
            throw std::runtime_error(
                "asset validation event is malformed");
        const nlohmann::json &validation = *found;
        const auto state = assetValidationStateFromName(
            validation.value("state", std::string{}));
        if (!state)
            throw std::runtime_error(
                "asset validation state is invalid");
        task->validationState = *state;
        task->validationErrors = unsignedValue(validation, "errors");
        task->validationWarnings = unsignedValue(validation, "warnings");
        std::lock_guard lock(task->mutex);
        task->validationReportKey =
            validation.value("reportKey", std::string{});
        task->validationInputFingerprint =
            validation.value("inputFingerprint", std::string{});
        task->validationFailureReason =
            validation.value("failureReason", std::string{});
    } else if (type == "completed") {
        task->completedArtifacts =
            unsignedValue(event, "completed",
                          task->totalArtifacts.load());
        task->encodedArtifacts = unsignedValue(event, "encoded");
        task->reusedArtifacts = unsignedValue(event, "reused");
        task->failedArtifacts = unsignedValue(event, "failed");
        std::lock_guard lock(task->mutex);
        task->manifestPath = event.value("manifest", std::string{});
        task->completedEventReceived = true;
    } else if (type == "failed" || type == "artifact_failed") {
        const std::string message = event.value("message", std::string{});
        if (!message.empty()) {
            std::lock_guard lock(task->mutex);
            task->error = message;
        }
    }
}

void AssetImportManager::workerLoop() {
    profileSetThreadName("AssetImport");
    for (;;) {
        std::shared_ptr<AssetImportTask> task;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock,
                          [&] { return stopping_ || !pending_.empty(); });
            if (stopping_ && pending_.empty())
                return;
            task = pending_.front();
            pending_.pop_front();
            if (task->cancellation->load())
                continue;
            active_ = task;
            task->state = AssetImportState::Scanning;
        }

        std::error_code directoryError;
        VKL_PROFILE_ZONE("Asset Import Task");
        VKL_PROFILE_TEXT(task->sceneId);
        std::filesystem::create_directories(task->logPath.parent_path(),
                                            directoryError);
        std::ofstream log(task->logPath,
                          std::ios::binary | std::ios::trunc);
        std::mutex logMutex;
        const AssetImportRequest request{
            task->sceneId, task->profileId, task->reason,
            task->force, task->kind, task->sourcePath};
        AssetImportExecutionResult execution;
        try {
            execution = executor_(
                options_, request, *task->cancellation,
                [&](const nlohmann::json &event) {
                    applyEvent(task, event);
                    if (log) {
                        std::lock_guard logLock(logMutex);
                        log << event.dump() << '\n';
                        log.flush();
                    }
                },
                [&](const std::string &message) {
                    if (log) {
                        std::lock_guard logLock(logMutex);
                        log << message;
                        log.flush();
                    }
                });
        } catch (const std::exception &exception) {
            execution.exitCode = 1;
            execution.error = exception.what();
        }

        task->processExitCode = execution.exitCode;
        task->completedAt = std::chrono::steady_clock::now();
        if (task->cancellation->load() || execution.cancelled) {
            task->state = AssetImportState::Cancelled;
        } else {
            bool completedEvent = false;
            {
                std::lock_guard taskLock(task->mutex);
                completedEvent = task->completedEventReceived &&
                                 task->protocolValidated;
                if (!execution.error.empty())
                    task->error = execution.error;
                if (execution.exitCode != 0 && task->error.empty()) {
                    task->error = "asset tool exited with code " +
                                  std::to_string(execution.exitCode);
                }
                if (execution.exitCode == 0 && !completedEvent &&
                    task->error.empty()) {
                    task->error =
                        "asset tool exited without a valid completed event";
                }
            }
            task->state = execution.exitCode == 0 && completedEvent
                              ? AssetImportState::Completed
                              : AssetImportState::Failed;
        }

        {
            std::lock_guard lock(mutex_);
            if (active_ == task)
                active_.reset();
            pruneHistoryLocked();
        }
        changed_.notify_all();
    }
}

void AssetImportManager::pruneHistoryLocked() {
    constexpr size_t kMaxHistory = 64;
    while (historyIds_.size() > kMaxHistory) {
        const uint64_t id = historyIds_.front();
        const auto found = tasks_.find(id);
        if (found != tasks_.end() &&
            !isTerminalAssetImportState(found->second->state.load()))
            break;
        historyIds_.erase(historyIds_.begin());
        tasks_.erase(id);
    }
}

AssetImportExecutionResult runAssetImportProcess(
    const AssetImportManagerOptions &options,
    const AssetImportRequest &request,
    const std::atomic_bool &cancelRequested,
    const AssetImportEventCallback &eventCallback,
    const AssetImportLogCallback &logCallback) {
    const std::filesystem::path tool = resolveAssetTool(options.assetToolPath);
    std::vector<std::wstring> arguments;
    if (request.kind == AssetImportKind::Environment) {
        arguments = {
            L"environment-cache", L"build", L"--project",
            options.projectRoot.wstring(), L"--environment-id",
            std::filesystem::path(request.sceneId).wstring(), L"--profile",
            std::filesystem::path(request.profileId).wstring(),
            L"--cache-root", options.cacheRoot.wstring(), L"--progress",
            L"ndjson"};
    } else if (request.kind == AssetImportKind::SceneValidation) {
        arguments = {L"validate", L"scene", L"--project",
                     options.projectRoot.wstring(), L"--cache-root",
                     options.cacheRoot.wstring(), L"--progress", L"ndjson"};
        if (!request.sourcePath.empty()) {
            arguments.push_back(L"--source");
            arguments.push_back(request.sourcePath.wstring());
        } else {
            arguments.push_back(L"--scene-id");
            arguments.push_back(
                std::filesystem::path(request.sceneId).wstring());
        }
        if (!options.gltfValidatorPath.empty()) {
            arguments.push_back(L"--gltf-validator");
            arguments.push_back(options.gltfValidatorPath.wstring());
        }
    } else {
        arguments = {
            L"import", L"scene", L"--project",
            options.projectRoot.wstring(), L"--scene-id",
            std::filesystem::path(request.sceneId).wstring(), L"--profile",
            std::filesystem::path(request.profileId).wstring(),
            L"--cache-root", options.cacheRoot.wstring(), L"--progress",
            L"ndjson", L"--memory-budget-mib",
            std::to_wstring(options.memoryBudgetMiB)};
    }
    if (options.workers != 0 &&
        request.kind != AssetImportKind::SceneValidation) {
        arguments.push_back(L"--workers");
        arguments.push_back(std::to_wstring(options.workers));
    }
    if (request.force)
        arguments.push_back(L"--force");
    std::wstring commandLine = quoteWindowsArgument(tool.wstring());
    for (const std::wstring &argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    ScopedHandle stdoutRead, stdoutWrite, stderrRead, stderrWrite, nullInput;
    if (!CreatePipe(&stdoutRead.value, &stdoutWrite.value, &security, 0) ||
        !CreatePipe(&stderrRead.value, &stderrWrite.value, &security, 0) ||
        !SetHandleInformation(stdoutRead.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderrRead.value, HANDLE_FLAG_INHERIT, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not create asset tool pipes");
    }
    nullInput.value = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput.value == INVALID_HANDLE_VALUE)
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not open asset tool null input");

    ScopedAttributeList attributes;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    attributes.storage.resize(attributeBytes);
    attributes.value = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributes.storage.data());
    if (!InitializeProcThreadAttributeList(attributes.value, 1, 0,
                                           &attributeBytes)) {
        attributes.value = nullptr;
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not initialize asset tool handle list");
    }
    HANDLE inherited[]{nullInput.value, stdoutWrite.value, stderrWrite.value};
    if (!UpdateProcThreadAttribute(
            attributes.value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
            sizeof(inherited), nullptr, nullptr)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not configure asset tool handle list");
    }

    ScopedHandle job{CreateJobObjectW(nullptr, nullptr)};
    if (!job.value)
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not create asset tool Job Object");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not configure asset tool Job Object");
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.value;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput.value;
    startup.StartupInfo.hStdOutput = stdoutWrite.value;
    startup.StartupInfo.hStdError = stderrWrite.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tool.c_str(), mutableCommand.data(), nullptr, nullptr,
                        TRUE,
                        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP |
                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, options.projectRoot.c_str(),
                        &startup.StartupInfo, &process)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not start VulkanLabAssetTool");
    }
    ScopedHandle processHandle{process.hProcess};
    ScopedHandle threadHandle{process.hThread};
    if (!AssignProcessToJobObject(job.value, process.hProcess)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, error);
        throw std::system_error(static_cast<int>(error),
                                std::system_category(),
                                "could not assign asset tool to Job Object");
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateJobObject(job.value, error);
        throw std::system_error(static_cast<int>(error),
                                std::system_category(),
                                "could not resume asset tool");
    }
    CloseHandle(stdoutWrite.value);
    stdoutWrite.value = nullptr;
    CloseHandle(stderrWrite.value);
    stderrWrite.value = nullptr;

    std::atomic_bool readerFailed{false};
    std::mutex readerErrorMutex;
    std::string readerError;
    std::thread stdoutReader([&] {
        profileSetThreadName("AssetToolStdout");
        try {
            std::string pending;
            std::array<char, 4096> buffer{};
            DWORD read = 0;
            while (ReadFile(stdoutRead.value, buffer.data(),
                            static_cast<DWORD>(buffer.size()),
                            &read, nullptr) &&
                   read != 0) {
                pending.append(buffer.data(), read);
                if (pending.size() > kMaxProgressLineBytes &&
                    pending.find('\n') == std::string::npos) {
                    throw std::runtime_error(
                        "asset tool progress line exceeds 64 KiB");
                }
                size_t newline = 0;
                while ((newline = pending.find('\n')) != std::string::npos) {
                    if (newline > kMaxProgressLineBytes)
                        throw std::runtime_error(
                            "asset tool progress line exceeds 64 KiB");
                    std::string line = pending.substr(0, newline);
                    pending.erase(0, newline + 1);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (!line.empty())
                        eventCallback(nlohmann::json::parse(line));
                }
            }
            if (pending.size() > kMaxProgressLineBytes)
                throw std::runtime_error(
                    "asset tool progress line exceeds 64 KiB");
            if (!pending.empty())
                eventCallback(nlohmann::json::parse(pending));
        } catch (const std::exception &exception) {
            {
                std::lock_guard lock(readerErrorMutex);
                readerError = exception.what();
            }
            readerFailed = true;
            TerminateJobObject(job.value, ERROR_INVALID_DATA);
        }
    });
    std::thread stderrReader([&] {
        profileSetThreadName("AssetToolStderr");
        std::array<char, 4096> buffer{};
        DWORD read = 0;
        while (ReadFile(stderrRead.value, buffer.data(),
                        static_cast<DWORD>(buffer.size()), &read, nullptr) &&
               read != 0) {
            logCallback(std::string(buffer.data(), read));
        }
    });

    bool cancelled = false;
    for (;;) {
        const DWORD waitResult = WaitForSingleObject(process.hProcess, 100);
        if (waitResult == WAIT_OBJECT_0)
            break;
        if (waitResult == WAIT_FAILED) {
            TerminateJobObject(job.value, GetLastError());
            readerFailed = true;
            std::lock_guard lock(readerErrorMutex);
            readerError = "could not wait for asset tool process";
            break;
        }
        if (cancelRequested.load()) {
            cancelled = true;
            TerminateJobObject(job.value, ERROR_CANCELLED);
            break;
        }
        if (readerFailed.load())
            break;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    stdoutReader.join();
    stderrReader.join();

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.hProcess, &exitCode))
        exitCode = GetLastError();
    AssetImportExecutionResult result{exitCode, cancelled, {}};
    if (readerFailed.load()) {
        std::lock_guard lock(readerErrorMutex);
        result.error = "invalid asset tool progress: " + readerError;
    }
    return result;
}

} // namespace vkr
