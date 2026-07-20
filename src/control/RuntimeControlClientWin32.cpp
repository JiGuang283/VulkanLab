#include "RuntimeControlClientWin32.h"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace vkr {

#ifdef _WIN32
namespace {

class Handle {
  public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr)
            CloseHandle(value_);
    }

    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    HANDLE get() const { return value_; }
    bool valid() const {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }

  private:
    HANDLE value_;
};

bool writeExact(HANDLE pipe, const void *data, DWORD size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    DWORD completed = 0;
    while (completed < size) {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes + completed, size - completed, &written,
                       nullptr) ||
            written == 0) {
            return false;
        }
        completed += written;
    }
    return true;
}

bool readExact(HANDLE pipe, void *data, DWORD size) {
    auto *bytes = static_cast<unsigned char *>(data);
    DWORD completed = 0;
    while (completed < size) {
        DWORD read = 0;
        if (!ReadFile(pipe, bytes + completed, size - completed, &read,
                      nullptr) ||
            read == 0) {
            return false;
        }
        completed += read;
    }
    return true;
}

} // namespace
#endif

RuntimeControlClientWin32::RuntimeControlClientWin32(
    control::RuntimeControlEndpoint endpoint, uint32_t connectTimeoutMs)
    : endpoint_(std::move(endpoint)), connectTimeoutMs_(connectTimeoutMs) {
    if (connectTimeoutMs_ == 0)
        throw std::invalid_argument(
            "runtime control connect timeout must be non-zero");
}

ControlJson
RuntimeControlClientWin32::send(const ControlJson &request) const {
#ifdef _WIN32
    if (!WaitNamedPipeW(endpoint_.name.c_str(), connectTimeoutMs_)) {
        throw std::runtime_error("VulkanLab runtime control pipe '" +
                                 endpoint_.nameUtf8 + "' is unavailable");
    }

    Handle pipe(CreateFileW(endpoint_.name.c_str(),
                            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr));
    if (!pipe.valid()) {
        throw std::runtime_error(
            "failed to connect to VulkanLab runtime control pipe '" +
            endpoint_.nameUtf8 + "'");
    }

    const std::string payload = request.dump();
    if (payload.empty() || payload.size() > control::kMaxMessageBytes)
        throw std::runtime_error("request exceeds protocol message limit");
    const uint32_t length = static_cast<uint32_t>(payload.size());
    if (!writeExact(pipe.get(), &length, sizeof(length)) ||
        !writeExact(pipe.get(), payload.data(), length)) {
        throw std::runtime_error("failed to write runtime control request");
    }

    uint32_t responseLength = 0;
    if (!readExact(pipe.get(), &responseLength, sizeof(responseLength)) ||
        responseLength == 0 ||
        responseLength > control::kMaxMessageBytes) {
        throw std::runtime_error("invalid runtime control response size");
    }
    std::string responsePayload(responseLength, '\0');
    if (!readExact(pipe.get(), responsePayload.data(), responseLength))
        throw std::runtime_error("failed to read runtime control response");
    return ControlJson::parse(responsePayload);
#else
    (void)request;
    throw std::runtime_error(
        "runtime control client is only available on Windows");
#endif
}

ControlJson RuntimeControlClientWin32::invoke(uint64_t id,
                                              const std::string &method,
                                              ControlJson params) const {
    return send({{"id", id},
                 {"method", method},
                 {"params", std::move(params)}});
}

} // namespace vkr
