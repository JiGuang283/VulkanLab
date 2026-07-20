#include "ManagedProcessWin32.h"

#include <stdexcept>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace {

void requireProcess(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testWindowsArgumentQuoting() {
    using vkr::render_test::quoteWindowsArgument;
    requireProcess(quoteWindowsArgument(L"simple") == L"simple",
                   "simple Windows argument gained quoting");
    requireProcess(quoteWindowsArgument(L"") == L"\"\"",
                   "empty Windows argument was not preserved");
    requireProcess(quoteWindowsArgument(L"two words") == L"\"two words\"",
                   "space-containing Windows argument was not quoted");
    requireProcess(
        quoteWindowsArgument(LR"(C:\path with space\)") ==
            LR"("C:\path with space\\")",
        "trailing backslash was not doubled before closing quote");
    requireProcess(quoteWindowsArgument(LR"(say "hello")") ==
                       LR"("say \"hello\"")",
                   "embedded quotes were not escaped");
}

void testManagedProcessLifecycle() {
#ifdef _WIN32
    std::wstring modulePath(32768, L'\0');
    const DWORD moduleLength =
        GetModuleFileNameW(nullptr, modulePath.data(),
                           static_cast<DWORD>(modulePath.size()));
    requireProcess(moduleLength > 0 && moduleLength < modulePath.size(),
                   "current test executable path is unavailable");
    modulePath.resize(moduleLength);
    const std::filesystem::path command(modulePath);
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        std::filesystem::path(L"vulkan-lab-process-\u8fdb\u7a0b-") /
        std::to_wstring(GetCurrentProcessId());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    vkr::render_test::ManagedProcessOptions echoOptions;
    echoOptions.executable = command;
    echoOptions.arguments = {L"--managed-process-echo"};
    echoOptions.workingDirectory = root;
    echoOptions.outputLog = root / "echo output.log";
    echoOptions.environmentOverrides = {{L"VKR_PROCESS_TEST", L"passed"}};
    {
        vkr::render_test::ManagedProcessWin32 process(echoOptions);
        const auto exit = process.waitForExit(5000);
        requireProcess(exit && *exit == 0 && !process.running(),
                       "managed process did not report normal exit");
    }
    std::ifstream output(echoOptions.outputLog, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(output)),
                           std::istreambuf_iterator<char>());
    requireProcess(text.find("passed") != std::string::npos,
                   "child environment or redirected output was lost");

    vkr::render_test::ManagedProcessOptions longOptions;
    longOptions.executable = command;
    longOptions.arguments = {L"--managed-process-sleep"};
    longOptions.workingDirectory = root;
    longOptions.outputLog = root / "long output.log";
    {
        vkr::render_test::ManagedProcessWin32 process(longOptions);
        requireProcess(process.running(),
                       "long-running process exited before termination test");
        process.terminate(9);
        const auto exit = process.waitForExit(1000);
        requireProcess(exit && *exit == 9 && !process.running(),
                       "Job Object termination did not propagate exit code");
    }
    std::filesystem::remove_all(root, ignored);
#endif
}

} // namespace

void runManagedProcessWin32Tests() {
    testWindowsArgumentQuoting();
    testManagedProcessLifecycle();
}
