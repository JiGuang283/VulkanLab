#pragma once

#include <memory>

namespace vkr {

class RuntimeCommandQueue;

class NamedPipeServerWin32 {
  public:
    explicit NamedPipeServerWin32(RuntimeCommandQueue &queue);
    ~NamedPipeServerWin32();

    NamedPipeServerWin32(const NamedPipeServerWin32 &) = delete;
    NamedPipeServerWin32 &operator=(const NamedPipeServerWin32 &) = delete;

    bool start();
    void stop();
    bool running() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vkr
