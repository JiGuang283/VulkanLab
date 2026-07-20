#pragma once

#include "RuntimeCommand.h"
#include "RuntimeControlProtocol.h"

#include <cstdint>
#include <string>

namespace vkr {

class RuntimeControlClientWin32 {
  public:
    explicit RuntimeControlClientWin32(
        control::RuntimeControlEndpoint endpoint =
            control::makeRuntimeControlEndpoint(),
        uint32_t connectTimeoutMs = 5000);

    ControlJson send(const ControlJson &request) const;
    ControlJson invoke(uint64_t id, const std::string &method,
                       ControlJson params = ControlJson::object()) const;

    const control::RuntimeControlEndpoint &endpoint() const {
        return endpoint_;
    }

  private:
    control::RuntimeControlEndpoint endpoint_;
    uint32_t connectTimeoutMs_ = 5000;
};

} // namespace vkr
