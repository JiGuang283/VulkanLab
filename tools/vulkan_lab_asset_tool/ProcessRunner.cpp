#include "ProcessRunner.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace vkr::assettool {
namespace {

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

} // namespace

struct Win32JobProcessRunner::State {
    HANDLE job = nullptr;
    std::mutex mutex;
    bool cancelled = false;
};

Win32JobProcessRunner::Win32JobProcessRunner()
    : state_(std::make_unique<State>()) {
    state_->job = CreateJobObjectW(nullptr, nullptr);
    if (!state_->job) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not create asset import job object");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(state_->job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        const DWORD error = GetLastError();
        CloseHandle(state_->job);
        state_->job = nullptr;
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "could not configure asset import job object");
    }
}

Win32JobProcessRunner::~Win32JobProcessRunner() {
    cancelAll();
    if (state_ && state_->job)
        CloseHandle(state_->job);
}

ProcessResult
Win32JobProcessRunner::run(const ProcessRequest &request,
                           const std::atomic_bool &cancelRequested) {
    if (cancelRequested.load())
        return {ERROR_CANCELLED, true, {}};

    std::wstring commandLine =
        quoteWindowsArgument(request.executable.wstring());
    for (const std::wstring &argument : request.arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    ScopedHandle pipeRead;
    ScopedHandle pipeWrite;
    ScopedHandle nullInput;
    SECURITY_ATTRIBUTES pipeSecurity{sizeof(SECURITY_ATTRIBUTES), nullptr,
                                     TRUE};
    if (!CreatePipe(&pipeRead.value, &pipeWrite.value, &pipeSecurity, 0) ||
        !SetHandleInformation(pipeRead.value, HANDLE_FLAG_INHERIT, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not create ktx output pipe");
    }
    nullInput.value = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &pipeSecurity,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput.value == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not open null input for ktx tool");
    }

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
                                "could not initialize ktx handle list");
    }
    HANDLE inheritedHandles[]{nullInput.value, pipeWrite.value};
    if (!UpdateProcThreadAttribute(
            attributes.value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not configure ktx handle list");
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.value;
    startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput.value;
    startup.StartupInfo.hStdOutput = pipeWrite.value;
    startup.StartupInfo.hStdError = pipeWrite.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(request.executable.c_str(), mutableCommand.data(),
                        nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP |
                            EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &startup.StartupInfo, &process)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not start ktx tool");
    }
    ScopedHandle processHandle{process.hProcess};
    ScopedHandle threadHandle{process.hThread};
    CloseHandle(pipeWrite.value);
    pipeWrite.value = nullptr;

    std::string processOutput;
    std::thread outputReader([&] {
        char buffer[4096];
        DWORD read = 0;
        while (
            ReadFile(pipeRead.value, buffer, sizeof(buffer), &read, nullptr) &&
            read != 0) {
            if (processOutput.size() < 1024 * 1024) {
                const size_t remaining = 1024 * 1024 - processOutput.size();
                processOutput.append(buffer, std::min<size_t>(read, remaining));
            }
        }
    });
    const auto joinOutputReader = [&] {
        if (outputReader.joinable())
            outputReader.join();
    };

    {
        std::lock_guard lock(state_->mutex);
        if (state_->cancelled || cancelRequested.load()) {
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(process.hProcess, INFINITE);
            joinOutputReader();
            return {ERROR_CANCELLED, true, std::move(processOutput)};
        }
        if (!AssignProcessToJobObject(state_->job, process.hProcess)) {
            const DWORD error = GetLastError();
            TerminateProcess(process.hProcess, error);
            WaitForSingleObject(process.hProcess, INFINITE);
            joinOutputReader();
            throw std::system_error(static_cast<int>(error),
                                    std::system_category(),
                                    "could not assign ktx tool to job object");
        }
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, error);
        WaitForSingleObject(process.hProcess, INFINITE);
        joinOutputReader();
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "could not resume ktx tool");
    }

    while (true) {
        const DWORD wait = WaitForSingleObject(process.hProcess, 100);
        if (wait == WAIT_OBJECT_0)
            break;
        if (wait == WAIT_FAILED) {
            cancelAll();
            joinOutputReader();
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "waiting for ktx tool failed");
        }
        if (cancelRequested.load()) {
            cancelAll();
            WaitForSingleObject(process.hProcess, INFINITE);
            joinOutputReader();
            return {ERROR_CANCELLED, true, std::move(processOutput)};
        }
    }
    joinOutputReader();

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "reading ktx tool exit code failed");
    }
    return {exitCode, exitCode == ERROR_CANCELLED, std::move(processOutput)};
}

void Win32JobProcessRunner::cancelAll() noexcept {
    if (!state_)
        return;
    std::lock_guard lock(state_->mutex);
    if (state_->cancelled)
        return;
    state_->cancelled = true;
    if (state_->job)
        TerminateJobObject(state_->job, ERROR_CANCELLED);
}

} // namespace vkr::assettool
