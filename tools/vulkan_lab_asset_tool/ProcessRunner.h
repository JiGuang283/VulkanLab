#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace vkr::assettool {

struct ProcessRequest {
    std::filesystem::path executable;
    std::vector<std::wstring> arguments;
};

struct ProcessResult {
    uint32_t exitCode = 0;
    bool cancelled = false;
    std::string output;
};

class IProcessRunner {
  public:
    virtual ~IProcessRunner() = default;
    virtual ProcessResult run(const ProcessRequest &request,
                              const std::atomic_bool &cancelRequested) = 0;
    virtual void cancelAll() noexcept = 0;
};

class Win32JobProcessRunner final : public IProcessRunner {
  public:
    Win32JobProcessRunner();
    ~Win32JobProcessRunner() override;

    Win32JobProcessRunner(const Win32JobProcessRunner &) = delete;
    Win32JobProcessRunner &operator=(const Win32JobProcessRunner &) = delete;

    ProcessResult run(const ProcessRequest &request,
                      const std::atomic_bool &cancelRequested) override;
    void cancelAll() noexcept override;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace vkr::assettool
