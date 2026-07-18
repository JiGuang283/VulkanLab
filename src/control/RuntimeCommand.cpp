#include "RuntimeCommand.h"

#include <utility>
#include <vector>

namespace vkr {

ControlJson makeRuntimeSuccess(uint64_t id, ControlJson result) {
    return {{"id", id}, {"ok", true}, {"result", std::move(result)}};
}

ControlJson makeRuntimeError(uint64_t id, std::string code,
                             std::string message) {
    return {{"id", id},
            {"ok", false},
            {"error", {{"code", std::move(code)},
                       {"message", std::move(message)}}}};
}

bool RuntimeCommandQueue::push(std::shared_ptr<RuntimeCommand> command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
        return false;
    commands_.push_back(std::move(command));
    return true;
}

std::shared_ptr<RuntimeCommand> RuntimeCommandQueue::popNext() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (commands_.empty())
        return {};
    auto command = std::move(commands_.front());
    commands_.pop_front();
    return command;
}

void RuntimeCommandQueue::close() {
    std::vector<std::shared_ptr<RuntimeCommand>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return;
        closed_ = true;
        pending.assign(commands_.begin(), commands_.end());
        commands_.clear();
    }

    for (const auto &command : pending) {
        command->response.set_value(makeRuntimeError(
            command->id, "application_shutting_down",
            "The application is shutting down."));
    }
}

} // namespace vkr
