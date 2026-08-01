#include "ProcessRunner.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <chrono>
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
    ProcessResult result;
    if (cancelRequested.load()) {
        result.exitCode = ERROR_CANCELLED;
        result.cancelled = true;
        return result;
    }

    std::wstring commandLine =
        quoteWindowsArgument(request.executable.wstring());
    for (const std::wstring &argument : request.arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    ScopedHandle stdoutRead;
    ScopedHandle stdoutWrite;
    ScopedHandle stderrRead;
    ScopedHandle stderrWrite;
    ScopedHandle nullInput;
    SECURITY_ATTRIBUTES pipeSecurity{sizeof(SECURITY_ATTRIBUTES), nullptr,
                                     TRUE};
    if (!CreatePipe(&stdoutRead.value, &stdoutWrite.value, &pipeSecurity, 0) ||
        !CreatePipe(&stderrRead.value, &stderrWrite.value, &pipeSecurity, 0) ||
        !SetHandleInformation(stdoutRead.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderrRead.value, HANDLE_FLAG_INHERIT, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not create child process output pipes");
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
    HANDLE inheritedHandles[]{nullInput.value, stdoutWrite.value,
                              stderrWrite.value};
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
    startup.StartupInfo.hStdOutput = stdoutWrite.value;
    startup.StartupInfo.hStdError = stderrWrite.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(request.executable.c_str(), mutableCommand.data(),
                        nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP |
                            EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &startup.StartupInfo, &process)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "could not start child process");
    }
    ScopedHandle processHandle{process.hProcess};
    ScopedHandle threadHandle{process.hThread};
    CloseHandle(stdoutWrite.value);
    stdoutWrite.value = nullptr;
    CloseHandle(stderrWrite.value);
    stderrWrite.value = nullptr;

    const auto readOutput = [](HANDLE handle, size_t limit,
                               std::string &output, bool &truncated) {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) &&
               read != 0) {
            if (output.size() < limit) {
                const size_t remaining = limit - output.size();
                const size_t copied = std::min<size_t>(read, remaining);
                output.append(buffer, copied);
                truncated = truncated || copied != read;
            } else {
                truncated = true;
            }
        }
    };
    std::thread stdoutReader([&] {
        readOutput(stdoutRead.value, request.maxStdoutBytes,
                   result.stdoutText, result.stdoutTruncated);
    });
    std::thread stderrReader([&] {
        readOutput(stderrRead.value, request.maxStderrBytes,
                   result.stderrText, result.stderrTruncated);
    });
    const auto joinOutputReaders = [&] {
        if (stdoutReader.joinable())
            stdoutReader.join();
        if (stderrReader.joinable())
            stderrReader.join();
        result.output = result.stdoutText;
        result.output += result.stderrText;
    };

    {
        std::lock_guard lock(state_->mutex);
        if (state_->cancelled || cancelRequested.load()) {
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(process.hProcess, INFINITE);
            joinOutputReaders();
            result.exitCode = ERROR_CANCELLED;
            result.cancelled = true;
            return result;
        }
        if (!AssignProcessToJobObject(state_->job, process.hProcess)) {
            const DWORD error = GetLastError();
            TerminateProcess(process.hProcess, error);
            WaitForSingleObject(process.hProcess, INFINITE);
            joinOutputReaders();
            throw std::system_error(static_cast<int>(error),
                                    std::system_category(),
                                    "could not assign ktx tool to job object");
        }
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, error);
        WaitForSingleObject(process.hProcess, INFINITE);
        joinOutputReaders();
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "could not resume ktx tool");
    }

    const auto startedAt = std::chrono::steady_clock::now();
    while (true) {
        const DWORD wait = WaitForSingleObject(process.hProcess, 100);
        if (wait == WAIT_OBJECT_0)
            break;
        if (wait == WAIT_FAILED) {
            cancelAll();
            joinOutputReaders();
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "waiting for ktx tool failed");
        }
        if (cancelRequested.load()) {
            cancelAll();
            WaitForSingleObject(process.hProcess, INFINITE);
            joinOutputReaders();
            result.exitCode = ERROR_CANCELLED;
            result.cancelled = true;
            return result;
        }
        if (request.timeoutMs != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt)
                    .count() >= request.timeoutMs) {
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, INFINITE);
            joinOutputReaders();
            result.exitCode = ERROR_TIMEOUT;
            result.timedOut = true;
            return result;
        }
    }
    joinOutputReaders();

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "reading ktx tool exit code failed");
    }
    result.exitCode = exitCode;
    result.cancelled = exitCode == ERROR_CANCELLED;
    return result;
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
