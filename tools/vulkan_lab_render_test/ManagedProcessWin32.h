#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vkr::render_test {

std::wstring quoteWindowsArgument(std::wstring_view argument);

struct ManagedProcessOptions {
    std::filesystem::path executable;
    std::vector<std::wstring> arguments;
    std::filesystem::path workingDirectory;
    std::filesystem::path outputLog;
    std::map<std::wstring, std::wstring> environmentOverrides;
};

class ManagedProcessWin32 {
  public:
    explicit ManagedProcessWin32(const ManagedProcessOptions &options);
    ~ManagedProcessWin32();

    ManagedProcessWin32(const ManagedProcessWin32 &) = delete;
    ManagedProcessWin32 &operator=(const ManagedProcessWin32 &) = delete;

    uint32_t processId() const;
    bool running() const;
    std::optional<uint32_t> waitForExit(uint32_t timeoutMs) const;
    void terminate(uint32_t exitCode = 1);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vkr::render_test
