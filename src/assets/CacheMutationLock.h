#pragma once

#include <atomic>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace vkr {

class CacheMutationLock {
  public:
    explicit CacheMutationLock(
        const std::filesystem::path &cacheRoot,
        const std::atomic_bool *cancelRequested = nullptr);
    ~CacheMutationLock();

    CacheMutationLock(const CacheMutationLock &) = delete;
    CacheMutationLock &operator=(const CacheMutationLock &) = delete;

  private:
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
};

} // namespace vkr
