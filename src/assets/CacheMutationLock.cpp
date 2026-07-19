#include "CacheMutationLock.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace vkr {
namespace {

std::wstring mutationMutexName(const std::filesystem::path &cacheRoot) {
    std::wstring normalized =
        std::filesystem::absolute(cacheRoot).lexically_normal().wstring();
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t character : normalized) {
        if (character >= L'A' && character <= L'Z')
            character = character - L'A' + L'a';
        hash ^= static_cast<uint64_t>(character);
        hash *= 1099511628211ull;
    }
    return L"Local\\VulkanLab.CacheMutation." + std::to_wstring(hash);
}

} // namespace

CacheMutationLock::CacheMutationLock(
    const std::filesystem::path &cacheRoot,
    const std::atomic_bool *cancelRequested) {
    const std::wstring name = mutationMutexName(cacheRoot);
    mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!mutex_)
        throw std::runtime_error("Could not create cache mutation mutex");
    for (;;) {
        const DWORD result = WaitForSingleObject(mutex_, 100);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            owned_ = true;
            return;
        }
        if (result == WAIT_FAILED) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
            throw std::runtime_error("Could not wait for cache mutation mutex");
        }
        if (cancelRequested && cancelRequested->load()) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
            throw std::runtime_error("Cache mutation cancelled while waiting");
        }
    }
}

CacheMutationLock::~CacheMutationLock() {
    if (owned_)
        ReleaseMutex(mutex_);
    if (mutex_)
        CloseHandle(mutex_);
}

} // namespace vkr
