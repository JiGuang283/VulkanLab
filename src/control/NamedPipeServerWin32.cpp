#include "NamedPipeServerWin32.h"

#include "RuntimeCommand.h"
#include "RuntimeControlProtocol.h"
#include "core/Log.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace vkr {

#ifdef _WIN32
namespace {

bool waitForIo(HANDLE pipe, HANDLE stopEvent, OVERLAPPED &overlapped,
               DWORD &transferred) {
    const HANDLE events[] = {stopEvent, overlapped.hEvent};
    const DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    if (waitResult == WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        return false;
    }
    if (waitResult != WAIT_OBJECT_0 + 1)
        return false;
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
}

bool transferExact(HANDLE pipe, HANDLE stopEvent, void *data, DWORD size,
                   bool write) {
    auto *bytes = static_cast<unsigned char *>(data);
    DWORD completed = 0;
    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ioEvent)
        return false;

    bool success = true;
    while (completed < size) {
        ResetEvent(ioEvent);
        OVERLAPPED overlapped{};
        overlapped.hEvent = ioEvent;
        DWORD transferred = 0;
        const BOOL started =
            write ? WriteFile(pipe, bytes + completed, size - completed,
                              &transferred, &overlapped)
                  : ReadFile(pipe, bytes + completed, size - completed,
                             &transferred, &overlapped);
        if (!started) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING ||
                !waitForIo(pipe, stopEvent, overlapped, transferred)) {
                success = false;
                break;
            }
        }
        if (transferred == 0) {
            success = false;
            break;
        }
        completed += transferred;
    }

    CloseHandle(ioEvent);
    return success;
}

bool readFrame(HANDLE pipe, HANDLE stopEvent, std::string &payload,
               std::string &error) {
    uint32_t length = 0;
    if (!transferExact(pipe, stopEvent, &length, sizeof(length), false))
        return false;
    if (length == 0 || length > control::kMaxMessageBytes) {
        error = "Message size must be between 1 and " +
                std::to_string(control::kMaxMessageBytes) + " bytes.";
        return true;
    }

    payload.resize(length);
    return transferExact(pipe, stopEvent, payload.data(), length, false);
}

bool writeFrame(HANDLE pipe, HANDLE stopEvent, const std::string &payload) {
    if (payload.empty() || payload.size() > control::kMaxMessageBytes)
        return false;
    uint32_t length = static_cast<uint32_t>(payload.size());
    return transferExact(pipe, stopEvent, &length, sizeof(length), true) &&
           transferExact(pipe, stopEvent,
                         const_cast<char *>(payload.data()), length, true);
}

ControlJson parseRequest(const std::string &payload,
                         std::shared_ptr<RuntimeCommand> &command) {
    uint64_t id = 0;
    try {
        const ControlJson request = ControlJson::parse(payload);
        if (!request.is_object())
            return makeRuntimeError(0, "invalid_request",
                                    "Request must be a JSON object.");
        if (!request.contains("id") || !request["id"].is_number_unsigned())
            return makeRuntimeError(0, "invalid_request",
                                    "Request id must be an unsigned integer.");
        id = request["id"].get<uint64_t>();
        if (!request.contains("method") || !request["method"].is_string())
            return makeRuntimeError(id, "invalid_request",
                                    "Request method must be a string.");

        ControlJson params = ControlJson::object();
        if (request.contains("params")) {
            if (!request["params"].is_object())
                return makeRuntimeError(id, "invalid_request",
                                        "Request params must be an object.");
            params = request["params"];
        }

        command = std::make_shared<RuntimeCommand>();
        command->id = id;
        command->method = request["method"].get<std::string>();
        command->params = std::move(params);
        return {};
    } catch (const std::exception &e) {
        return makeRuntimeError(id, "invalid_json", e.what());
    }
}

} // namespace
#endif

struct NamedPipeServerWin32::Impl {
    Impl(RuntimeCommandQueue &commandQueue,
         control::RuntimeControlEndpoint controlEndpoint)
        : queue(commandQueue), endpoint(std::move(controlEndpoint)) {}

    RuntimeCommandQueue &queue;
    control::RuntimeControlEndpoint endpoint;
    std::atomic_bool stopping{false};
    std::atomic_bool isRunning{false};
    std::thread worker;

#ifdef _WIN32
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;

    void run() {
        isRunning = true;
        VKR_LOG_INFO("Control", "Runtime control listening on {}",
                     endpoint.nameUtf8);

        HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!connectEvent) {
            isRunning = false;
            return;
        }

        while (!stopping) {
            ResetEvent(connectEvent);
            OVERLAPPED overlapped{};
            overlapped.hEvent = connectEvent;
            bool connected = false;
            const BOOL started = ConnectNamedPipe(pipe, &overlapped);
            if (started) {
                connected = true;
            } else {
                const DWORD error = GetLastError();
                if (error == ERROR_PIPE_CONNECTED) {
                    connected = true;
                } else if (error == ERROR_IO_PENDING) {
                    DWORD transferred = 0;
                    connected = waitForIo(pipe, stopEvent, overlapped,
                                          transferred);
                }
            }
            if (stopping)
                break;
            if (!connected) {
                DisconnectNamedPipe(pipe);
                continue;
            }

            std::string payload;
            std::string frameError;
            ControlJson response;
            std::shared_ptr<RuntimeCommand> command;
            if (!readFrame(pipe, stopEvent, payload, frameError)) {
                DisconnectNamedPipe(pipe);
                continue;
            }
            if (!frameError.empty()) {
                response = makeRuntimeError(0, "message_too_large",
                                            std::move(frameError));
            } else {
                response = parseRequest(payload, command);
            }

            if (command) {
                auto responseFuture = command->response.get_future();
                if (!queue.push(command)) {
                    response = makeRuntimeError(
                        command->id, "application_shutting_down",
                        "The application is shutting down.");
                } else {
                    while (!stopping &&
                           responseFuture.wait_for(std::chrono::milliseconds(50)) !=
                               std::future_status::ready) {
                    }
                    if (!stopping ||
                        responseFuture.wait_for(std::chrono::milliseconds(0)) ==
                            std::future_status::ready) {
                        response = responseFuture.get();
                    }
                }
            }

            if (!response.is_null() && !response.empty())
                writeFrame(pipe, stopEvent, response.dump());
            FlushFileBuffers(pipe);
            if (command)
                command->responseDelivered = true;
            DisconnectNamedPipe(pipe);
        }

        CloseHandle(connectEvent);
        isRunning = false;
    }
#endif
};

NamedPipeServerWin32::NamedPipeServerWin32(
    RuntimeCommandQueue &queue,
    control::RuntimeControlEndpoint endpoint)
    : impl_(std::make_unique<Impl>(queue, std::move(endpoint))) {}

NamedPipeServerWin32::~NamedPipeServerWin32() {
    stop();
}

bool NamedPipeServerWin32::start() {
#ifdef _WIN32
    if (impl_->worker.joinable())
        return impl_->isRunning;

    impl_->stopping = false;
    impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->stopEvent) {
        VKR_LOG_ERROR("Control", "Failed to create runtime control stop event");
        return false;
    }

    impl_->pipe = CreateNamedPipeW(
        impl_->endpoint.name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, control::kMaxMessageBytes + sizeof(uint32_t),
        control::kMaxMessageBytes + sizeof(uint32_t), 0, nullptr);
    if (impl_->pipe == INVALID_HANDLE_VALUE) {
        VKR_LOG_ERROR("Control", "Failed to create runtime control pipe: {}",
                      GetLastError());
        CloseHandle(impl_->stopEvent);
        impl_->stopEvent = nullptr;
        return false;
    }

    impl_->worker = std::thread([this] { impl_->run(); });
    return true;
#else
    VKR_LOG_WARN("Control", "Runtime control is only available on Windows");
    return false;
#endif
}

void NamedPipeServerWin32::stop() {
    if (!impl_)
        return;
    impl_->queue.close();
    impl_->stopping = true;
#ifdef _WIN32
    if (impl_->stopEvent)
        SetEvent(impl_->stopEvent);
    if (impl_->pipe != INVALID_HANDLE_VALUE)
        CancelIoEx(impl_->pipe, nullptr);
    if (impl_->worker.joinable())
        impl_->worker.join();
    if (impl_->pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->pipe);
        impl_->pipe = INVALID_HANDLE_VALUE;
    }
    if (impl_->stopEvent) {
        CloseHandle(impl_->stopEvent);
        impl_->stopEvent = nullptr;
    }
#endif
}

bool NamedPipeServerWin32::running() const {
    return impl_ && impl_->isRunning;
}

} // namespace vkr
