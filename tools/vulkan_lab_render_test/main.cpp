#include "RenderTestRunner.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace {

std::string utf8FromWide(const std::wstring &value) {
    if (value.empty())
        return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        throw std::invalid_argument("command line contains invalid Unicode");
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            length, nullptr, nullptr) != length) {
        throw std::invalid_argument("command line contains invalid Unicode");
    }
    return result;
}

void printUsage(std::ostream &out) {
    out << "Usage:\n"
        << "  VulkanLabRenderTest run --spec <file.json> "
           "[--runtime <VulkanLab.exe>] [--output <directory>]\n"
        << "      [--project <directory>] [--accept] "
           "[--startup-timeout-ms N]\n"
        << "      [--operation-timeout-ms N] [--render-timeout-ms N] "
           "[--capture-timeout-ms N] [--quit-timeout-ms N]\n";
}

uint32_t parseTimeout(const std::wstring &value, const char *name) {
    size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0 || parsed > UINT32_MAX)
        throw std::invalid_argument(std::string(name) +
                                    " must be in 1..4294967295");
    return static_cast<uint32_t>(parsed);
}

std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        throw std::runtime_error("Could not locate VulkanLabRenderTest.exe");
    path.resize(length);
    return std::filesystem::path(path).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

vkr::render_test::RenderTestRunOptions
parseArguments(int argc, wchar_t **argv) {
    if (argc < 2 || std::wstring(argv[1]) != L"run")
        throw std::invalid_argument("expected the 'run' command");
    vkr::render_test::RenderTestRunOptions options;
    options.runtimeExecutable = executableDirectory() / "VulkanLab.exe";
    options.outputRoot = std::filesystem::current_path() / "render-results";
    for (int index = 2; index < argc; ++index) {
        const std::wstring argument = argv[index];
        auto value = [&](const char *name) -> std::wstring {
            if (++index >= argc)
                throw std::invalid_argument(std::string(name) +
                                            " requires a value");
            return argv[index];
        };
        if (argument == L"--spec")
            options.specPath = std::filesystem::path(value("--spec"));
        else if (argument == L"--runtime")
            options.runtimeExecutable =
                std::filesystem::path(value("--runtime"));
        else if (argument == L"--output")
            options.outputRoot =
                std::filesystem::path(value("--output"));
        else if (argument == L"--project")
            options.projectRoot =
                std::filesystem::path(value("--project"));
        else if (argument == L"--accept")
            options.accept = true;
        else if (argument == L"--startup-timeout-ms")
            options.startupTimeoutMs =
                parseTimeout(value("--startup-timeout-ms"),
                             "--startup-timeout-ms");
        else if (argument == L"--operation-timeout-ms")
            options.operationTimeoutMs =
                parseTimeout(value("--operation-timeout-ms"),
                             "--operation-timeout-ms");
        else if (argument == L"--render-timeout-ms")
            options.renderTimeoutMs =
                parseTimeout(value("--render-timeout-ms"),
                             "--render-timeout-ms");
        else if (argument == L"--capture-timeout-ms")
            options.captureTimeoutMs =
                parseTimeout(value("--capture-timeout-ms"),
                             "--capture-timeout-ms");
        else if (argument == L"--quit-timeout-ms")
            options.quitTimeoutMs =
                parseTimeout(value("--quit-timeout-ms"),
                             "--quit-timeout-ms");
        else
            throw std::invalid_argument("unknown argument: " +
                                        utf8FromWide(argument));
    }
    if (options.specPath.empty())
        throw std::invalid_argument("--spec is required");
    return options;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        printUsage(std::cout);
        return 0;
    }
    try {
        const auto result =
            vkr::render_test::runRenderTest(parseArguments(argc, argv));
        std::ostream &output = result.exitCode == 0 ? std::cout : std::cerr;
        output << result.status;
        if (!result.code.empty())
            output << " [" << result.code << ']';
        if (!result.message.empty())
            output << ": " << result.message;
        output << "\nreport: " << result.reportPath.u8string() << '\n';
        return result.exitCode;
    } catch (const std::invalid_argument &error) {
        std::cerr << "error: " << error.what() << "\n\n";
        printUsage(std::cerr);
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
