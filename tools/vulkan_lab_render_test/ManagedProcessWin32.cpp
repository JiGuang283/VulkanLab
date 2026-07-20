#include "ManagedProcessWin32.h"

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace vkr::render_test {

std::wstring quoteWindowsArgument(std::wstring_view argument) {
    if (argument.empty())
        return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);

    std::wstring result;
    result.push_back(L'\"');
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

#ifdef _WIN32
namespace {

class Handle {
  public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }

    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    Handle(Handle &&other) noexcept : value_(other.release()) {}
    Handle &operator=(Handle &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const { return value_; }
    bool valid() const {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() {
        HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr) {
        if (valid())
            CloseHandle(value_);
        value_ = value;
    }

  private:
    HANDLE value_ = nullptr;
};

std::runtime_error win32Error(const char *operation) {
    return std::runtime_error(std::string(operation) +
                              " failed with Win32 error " +
                              std::to_string(GetLastError()));
}

struct CaseInsensitiveLess {
    bool operator()(const std::wstring &lhs,
                    const std::wstring &rhs) const {
        return std::lexicographical_compare(
            lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
            [](wchar_t left, wchar_t right) {
                return std::towlower(left) < std::towlower(right);
            });
    }
};

std::vector<wchar_t> buildEnvironment(
    const std::map<std::wstring, std::wstring> &overrides) {
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> values;
    wchar_t *block = GetEnvironmentStringsW();
    if (!block)
        throw win32Error("GetEnvironmentStringsW");
    for (const wchar_t *cursor = block; *cursor;) {
        std::wstring entry(cursor);
        cursor += entry.size() + 1;
        const size_t separator = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
        if (separator != std::wstring::npos)
            values[entry.substr(0, separator)] = entry.substr(separator + 1);
    }
    FreeEnvironmentStringsW(block);
    for (const auto &[key, value] : overrides) {
        if (key.empty() || key.find(L'=') != std::wstring::npos ||
            key.find(L'\0') != std::wstring::npos ||
            value.find(L'\0') != std::wstring::npos) {
            throw std::invalid_argument("invalid child environment override");
        }
        values[key] = value;
    }

    std::vector<wchar_t> result;
    for (const auto &[key, value] : values) {
        result.insert(result.end(), key.begin(), key.end());
        result.push_back(L'=');
        result.insert(result.end(), value.begin(), value.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

std::wstring buildCommandLine(const ManagedProcessOptions &options) {
    std::wstring command = quoteWindowsArgument(options.executable.wstring());
    for (const std::wstring &argument : options.arguments) {
        command.push_back(L' ');
        command += quoteWindowsArgument(argument);
    }
    return command;
}

} // namespace
#endif

class ManagedProcessWin32::Impl {
  public:
#ifdef _WIN32
    Handle job;
    Handle process;
    uint32_t processId = 0;
#endif
};

ManagedProcessWin32::ManagedProcessWin32(
    const ManagedProcessOptions &options)
    : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    if (options.executable.empty() ||
        !std::filesystem::is_regular_file(options.executable)) {
        throw std::invalid_argument("managed executable does not exist");
    }
    if (options.workingDirectory.empty() ||
        !std::filesystem::is_directory(options.workingDirectory)) {
        throw std::invalid_argument(
            "managed process working directory does not exist");
    }
    if (options.outputLog.empty())
        throw std::invalid_argument("managed process output log is empty");
    if (options.outputLog.has_parent_path())
        std::filesystem::create_directories(options.outputLog.parent_path());

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    Handle output(CreateFileW(options.outputLog.c_str(),
                              GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &security, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!output.valid())
        throw win32Error("CreateFileW(output log)");
    Handle input(CreateFileW(L"NUL", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input.valid())
        throw win32Error("CreateFileW(NUL)");

    impl_->job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!impl_->job.valid())
        throw win32Error("CreateJobObjectW");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(impl_->job.get(),
                                 JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        throw win32Error("SetInformationJobObject");
    }

    std::wstring commandLine = buildCommandLine(options);
    std::vector<wchar_t> environment =
        buildEnvironment(options.environmentOverrides);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.get();
    startup.hStdOutput = output.get();
    startup.hStdError = output.get();
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(options.executable.c_str(), commandLine.data(),
                        nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        environment.data(), options.workingDirectory.c_str(),
                        &startup, &processInfo)) {
        throw win32Error("CreateProcessW");
    }

    Handle thread(processInfo.hThread);
    impl_->process.reset(processInfo.hProcess);
    impl_->processId = processInfo.dwProcessId;
    if (!AssignProcessToJobObject(impl_->job.get(), impl_->process.get())) {
        TerminateProcess(impl_->process.get(), 1);
        WaitForSingleObject(impl_->process.get(), INFINITE);
        throw win32Error("AssignProcessToJobObject");
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        TerminateJobObject(impl_->job.get(), 1);
        WaitForSingleObject(impl_->process.get(), INFINITE);
        throw win32Error("ResumeThread");
    }
#else
    (void)options;
    throw std::runtime_error("managed process is only available on Windows");
#endif
}

ManagedProcessWin32::~ManagedProcessWin32() {
    try {
        if (running())
            terminate(1);
    } catch (...) {
    }
}

uint32_t ManagedProcessWin32::processId() const {
#ifdef _WIN32
    return impl_->processId;
#else
    return 0;
#endif
}

bool ManagedProcessWin32::running() const {
#ifdef _WIN32
    if (!impl_ || !impl_->process.valid())
        return false;
    return WaitForSingleObject(impl_->process.get(), 0) == WAIT_TIMEOUT;
#else
    return false;
#endif
}

std::optional<uint32_t>
ManagedProcessWin32::waitForExit(uint32_t timeoutMs) const {
#ifdef _WIN32
    if (!impl_ || !impl_->process.valid())
        return std::nullopt;
    const DWORD wait = WaitForSingleObject(impl_->process.get(), timeoutMs);
    if (wait == WAIT_TIMEOUT)
        return std::nullopt;
    if (wait != WAIT_OBJECT_0)
        throw win32Error("WaitForSingleObject(process)");
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(impl_->process.get(), &exitCode))
        throw win32Error("GetExitCodeProcess");
    return static_cast<uint32_t>(exitCode);
#else
    (void)timeoutMs;
    return std::nullopt;
#endif
}

void ManagedProcessWin32::terminate(uint32_t exitCode) {
#ifdef _WIN32
    if (!impl_ || !impl_->job.valid() || !running())
        return;
    if (!TerminateJobObject(impl_->job.get(), exitCode))
        throw win32Error("TerminateJobObject");
    if (WaitForSingleObject(impl_->process.get(), 10000) != WAIT_OBJECT_0)
        throw std::runtime_error("managed process did not terminate");
#else
    (void)exitCode;
#endif
}

} // namespace vkr::render_test
