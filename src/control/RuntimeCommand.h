#pragma once

#include <json.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>

namespace vkr {

using ControlJson = nlohmann::json;

struct RuntimeCommand {
    uint64_t id = 0;
    std::string method;
    ControlJson params = ControlJson::object();
    std::promise<ControlJson> response;
    std::atomic_bool responseDelivered{false};
};

ControlJson makeRuntimeSuccess(uint64_t id,
                               ControlJson result = ControlJson::object());
ControlJson makeRuntimeError(uint64_t id, std::string code,
                             std::string message);

class RuntimeCommandQueue {
  public:
    bool push(std::shared_ptr<RuntimeCommand> command);
    std::shared_ptr<RuntimeCommand> popNext();
    void close();

  private:
    std::mutex mutex_;
    std::deque<std::shared_ptr<RuntimeCommand>> commands_;
    bool closed_ = false;
};

} // namespace vkr
