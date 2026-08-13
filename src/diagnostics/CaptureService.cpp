#include "CaptureService.h"

#include "Profiling.h"

#include "CaptureTaskQueue.h"
#include "assets/ContentHash.h"
#include "core/Buffer.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/GpuBarrier.h"
#include "core/Log.h"

#include <stb_image_write.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace vkr {
namespace {

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::filesystem::path defaultRelativePath(uint64_t taskId) {
    std::ostringstream name;
    name << "manual/capture-" << std::setw(8) << std::setfill('0')
         << (taskId - kCaptureTaskIdBase) << ".png";
    return name.str();
}

void appendEncodedBytes(void *context, void *data, int size) {
    if (!context || !data || size <= 0)
        return;
    auto &bytes = *static_cast<std::vector<uint8_t> *>(context);
    const auto *first = static_cast<const uint8_t *>(data);
    bytes.insert(bytes.end(), first, first + size);
}

void atomicPublish(const std::filesystem::path &temporary,
                   const std::filesystem::path &output) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), output.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("could not publish capture (Win32 error " +
                                 std::to_string(GetLastError()) + ")");
    }
#else
    std::filesystem::rename(temporary, output);
#endif
}

struct EncodeJob {
    uint64_t taskId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::filesystem::path outputPath;
    std::vector<uint8_t> rawPixels;
    std::shared_ptr<std::atomic_bool> cancelled;
};

struct EncodeResult {
    uint64_t taskId = 0;
    bool success = false;
    bool cancelled = false;
    double encodeMs = 0.0;
    std::filesystem::path outputPath;
    std::string sha256;
    std::string error;
};

EncodeResult encodeCapture(EncodeJob job) {
    EncodeResult result;
    result.taskId = job.taskId;
    result.outputPath = job.outputPath;
    const Clock::time_point start = Clock::now();
    const auto finish = [&result, start] {
        result.encodeMs = elapsedMs(start, Clock::now());
        return result;
    };
    std::filesystem::path temporary;
    try {
        if (job.cancelled && job.cancelled->load()) {
            result.cancelled = true;
            return finish();
        }

        const CaptureFormatDescription format =
            describeCaptureFormat(job.format);
        std::vector<uint8_t> encoded;
        if (format.encoding == CapturePixelEncoding::Unorm8) {
            std::vector<uint8_t> rgba = convertCapturePixelsToRgba(
                job.rawPixels.data(), job.rawPixels.size(), job.width,
                job.height, job.format);
            if (!stbi_write_png_to_func(
                    appendEncodedBytes, &encoded,
                    static_cast<int>(job.width),
                    static_cast<int>(job.height), 4, rgba.data(),
                    static_cast<int>(job.width * 4))) {
                throw std::runtime_error("stb failed to encode capture PNG");
            }
        } else {
            std::vector<float> rgb = convertCapturePixelsToRgbFloat(
                job.rawPixels.data(), job.rawPixels.size(), job.width,
                job.height, job.format);
            if (!stbi_write_hdr_to_func(
                    appendEncodedBytes, &encoded,
                    static_cast<int>(job.width),
                    static_cast<int>(job.height), 3, rgb.data())) {
                throw std::runtime_error("stb failed to encode capture HDR");
            }
        }
        if (job.cancelled && job.cancelled->load()) {
            result.cancelled = true;
            return finish();
        }

        std::filesystem::create_directories(job.outputPath.parent_path());
        temporary = job.outputPath;
        temporary += ".tmp-" + std::to_string(job.taskId);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error("could not open temporary capture");
            output.write(reinterpret_cast<const char *>(encoded.data()),
                         static_cast<std::streamsize>(encoded.size()));
            output.flush();
            if (!output)
                throw std::runtime_error("could not write temporary capture");
        }
        if (job.cancelled && job.cancelled->load()) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            result.cancelled = true;
            return finish();
        }

        atomicPublish(temporary, job.outputPath);
        if (job.cancelled && job.cancelled->load()) {
            std::error_code ignored;
            std::filesystem::remove(job.outputPath, ignored);
            result.cancelled = true;
            return finish();
        }

        result.sha256 = sha256Bytes(encoded);
        result.success = true;
    } catch (const std::exception &error) {
        result.error = error.what();
        if (!temporary.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
    }
    return finish();
}

} // namespace

class CaptureService::Impl {
  public:
    Impl(Device &device, std::filesystem::path captureRoot)
        : device_(&device) {
        if (captureRoot.empty())
            throw std::invalid_argument("capture root is empty");
        captureRoot_ =
            std::filesystem::absolute(std::move(captureRoot)).lexically_normal();
    }

    ~Impl() {
        if (!shutdown_) {
            accepting_ = false;
            for (auto &[taskId, runtime] : runtimes_) {
                (void)taskId;
                runtime.cancelled->store(true);
            }
            stopWorker();
        }
    }

    std::optional<CaptureTaskSnapshot> task(uint64_t taskId) const {
        return tasks_.snapshot(taskId);
    }

    std::vector<CaptureTaskSnapshot> tasks() const {
        return tasks_.snapshots();
    }

    const std::filesystem::path &captureRoot() const { return captureRoot_; }

    bool acceptingRequests() const { return accepting_; }

    uint64_t request(std::filesystem::path relativeOutputPath,
                     bool includeGui) {
        return requestSource(std::move(relativeOutputPath),
                             includeGui ? CaptureSourceKind::Workspace
                                        : CaptureSourceKind::Viewport,
                             includeGui);
    }

    uint64_t requestHdr(std::filesystem::path relativeOutputPath) {
        return requestSource(std::move(relativeOutputPath),
                             CaptureSourceKind::Hdr, false);
    }

    uint64_t requestSource(std::filesystem::path relativeOutputPath,
                           CaptureSourceKind source, bool includeGui) {
        if (!accepting_)
            throw std::runtime_error("capture service is shutting down");

        const uint64_t nextTaskId = tasks_.nextTaskId();
        if (relativeOutputPath.empty())
            relativeOutputPath = defaultRelativePath(nextTaskId);
        const std::filesystem::path outputPath =
            resolveCaptureOutputPath(captureRoot_, relativeOutputPath,
                                     source);

        const uint64_t taskId =
            tasks_.enqueue(relativeOutputPath.lexically_normal(), includeGui,
                           source);
        CaptureTaskSnapshot *task = tasks_.find(taskId);
        task->result.outputPath = outputPath;

        TaskRuntime runtime;
        runtime.requestedAt = Clock::now();
        runtime.cancelled = std::make_shared<std::atomic_bool>(false);
        runtimes_.emplace(taskId, std::move(runtime));
        VKR_LOG_INFO("Capture", "Queued capture task {} -> '{}'",
                     taskId, outputPath.string());
        return taskId;
    }

    bool cancel(uint64_t taskId) {
        auto runtime = runtimes_.find(taskId);
        if (runtime != runtimes_.end())
            runtime->second.cancelled->store(true);
        const bool cancelled = tasks_.cancel(taskId);
        if (cancelled) {
            if (const CaptureTaskSnapshot *task = tasks_.find(taskId);
                task && task->state == CaptureTaskState::Cancelled) {
                runtimes_.erase(taskId);
            }
            VKR_LOG_INFO("Capture", "Cancellation requested for task {}",
                         taskId);
        }
        return cancelled;
    }

    std::optional<CaptureFrameSelection>
    prepareFrame(const CaptureImageSource &viewport,
                 const CaptureImageSource &workspace,
                 const CaptureImageSource &hdr) {
        if (active_)
            return std::nullopt;

        const std::optional<uint64_t> next = tasks_.beginNext();
        if (!next)
            return std::nullopt;
        const uint64_t taskId = *next;
        CaptureTaskSnapshot *task = tasks_.find(taskId);
        TaskRuntime &runtime = runtimes_.at(taskId);
        runtime.recordingAt = Clock::now();

        try {
            const CaptureImageSource *sourcePtr = &viewport;
            switch (task->request.source) {
            case CaptureSourceKind::Viewport:
                sourcePtr = &viewport;
                break;
            case CaptureSourceKind::Workspace:
                sourcePtr = &workspace;
                break;
            case CaptureSourceKind::Hdr:
                sourcePtr = &hdr;
                break;
            }
            const CaptureImageSource &source = *sourcePtr;
            if (!source.supported)
                throw std::runtime_error(source.unsupportedReason.empty()
                                             ? "capture source is unsupported"
                                             : source.unsupportedReason);
            if (source.image == VK_NULL_HANDLE || source.extent.width == 0 ||
                source.extent.height == 0)
                throw std::runtime_error("capture source is invalid");
            if (!describeCaptureFormat(source.format).supported)
                throw std::runtime_error("capture source format is unsupported");
            const VkExtent2D extent = source.extent;
            const CaptureFormatDescription format =
                describeCaptureFormat(source.format);
            const uint64_t bytes = checkedCaptureByteSize(
                extent.width, extent.height, kMaxCaptureBytes,
                format.bytesPerPixel);

            ActiveCapture active;
            active.taskId = taskId;
            active.extent = extent;
            active.format = source.format;
            active.source = source;
            active.byteSize = bytes;
            active.buffer = std::make_unique<Buffer>(
                *device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                "Capture/Task" + std::to_string(taskId) + "/ReadbackBuffer");
            active.buffer->map();
            task->result.width = extent.width;
            task->result.height = extent.height;
            task->result.format = active.format;
            task->result.source = source.kind;
            active_ = std::move(active);
            return CaptureFrameSelection{taskId, source.kind};
        } catch (const std::exception &error) {
            failTask(taskId, error.what());
            return std::nullopt;
        }
    }

    void recordCopy(VkCommandBuffer commandBuffer) {
        if (!active_)
            return;
        const CaptureTaskSnapshot *task = tasks_.find(active_->taskId);
        if (!task || task->state != CaptureTaskState::Recording)
            throw std::logic_error(
                "capture copy requires a recording task");
        if (active_->copyRecorded)
            throw std::logic_error("capture copy was already recorded");
        ScopedGpuLabel label(device_->debugUtils(), commandBuffer,
                             "ScreenshotCopy");

        VkBufferImageCopy copy{};
        copy.bufferOffset = 0;
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.baseArrayLayer = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = {0, 0, 0};
        copy.imageExtent = {active_->extent.width, active_->extent.height, 1};
        vkCmdCopyImageToBuffer(commandBuffer, active_->source.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               active_->buffer->handle(), 1, &copy);
        active_->copyRecorded = true;
    }

    void frameSubmitted(uint64_t submissionSerial) {
        if (!active_)
            return;
        CaptureTaskSnapshot *task = tasks_.find(active_->taskId);
        if (!task || task->state != CaptureTaskState::Recording)
            throw std::logic_error(
                "capture submission requires a recording task");
        if (!active_->copyRecorded) {
            failTask(active_->taskId,
                     "capture copy was not recorded before frame submission");
            active_.reset();
            return;
        }

        TaskRuntime &runtime = runtimes_.at(active_->taskId);
        runtime.submittedAt = Clock::now();
        task->result.frameSerial = submissionSerial;
        task->result.timings.recordingMs =
            elapsedMs(runtime.recordingAt, runtime.submittedAt);
        active_->submissionSerial = submissionSerial;
        tasks_.transition(active_->taskId,
                          CaptureTaskState::WaitingForGpu);
    }

    void update(uint64_t completedSubmissionSerial) {
        processWorkerResults();
        if (!active_ || active_->submissionSerial == 0 ||
            active_->submissionSerial > completedSubmissionSerial) {
            return;
        }

        const uint64_t taskId = active_->taskId;
        CaptureTaskSnapshot *task = tasks_.find(taskId);
        TaskRuntime &runtime = runtimes_.at(taskId);
        task->result.timings.gpuWaitMs =
            elapsedMs(runtime.submittedAt, Clock::now());
        if (task->state == CaptureTaskState::Cancelling ||
            runtime.cancelled->load()) {
            tasks_.transition(taskId, CaptureTaskState::Cancelled);
            runtimes_.erase(taskId);
            active_.reset();
            return;
        }

        try {
            const Clock::time_point copyStart = Clock::now();
            active_->buffer->invalidate(0, active_->byteSize);
            std::vector<uint8_t> rawPixels(
                static_cast<size_t>(active_->byteSize));
            std::memcpy(rawPixels.data(), active_->buffer->mappedData(),
                        rawPixels.size());
            task->result.timings.cpuCopyMs =
                elapsedMs(copyStart, Clock::now());

            EncodeJob job;
            job.taskId = taskId;
            job.width = active_->extent.width;
            job.height = active_->extent.height;
            job.format = active_->format;
            job.outputPath = task->result.outputPath;
            job.rawPixels = std::move(rawPixels);
            job.cancelled = runtime.cancelled;
            tasks_.transition(taskId, CaptureTaskState::Encoding);
            active_.reset();
            enqueueEncode(std::move(job));
        } catch (const std::exception &error) {
            failTask(taskId, error.what());
            active_.reset();
        }
    }

    void onSwapChainRecreated(uint64_t completedSubmissionSerial) {
        processWorkerResults();
        if (!active_)
            return;
        if (active_->submissionSerial != 0 &&
            active_->submissionSerial > completedSubmissionSerial) {
            throw std::logic_error(
                "swapchain recreated before capture GPU work completed");
        }

        const uint64_t taskId = active_->taskId;
        CaptureTaskSnapshot *task = tasks_.find(taskId);
        if (task && task->state == CaptureTaskState::Cancelling) {
            tasks_.transition(taskId, CaptureTaskState::Cancelled);
        } else {
            failTask(taskId, "swapchain was recreated during capture");
        }
        runtimes_.erase(taskId);
        active_.reset();
    }

    void shutdown(uint64_t completedSubmissionSerial) {
        if (shutdown_)
            return;
        accepting_ = false;

        for (const CaptureTaskSnapshot &task : tasks_.snapshots()) {
            if (task.state == CaptureTaskState::Queued)
                cancel(task.request.taskId);
        }

        if (active_ && active_->submissionSerial != 0 &&
            active_->submissionSerial > completedSubmissionSerial) {
            throw std::logic_error(
                "capture shutdown before submitted GPU work completed");
        }
        if (active_ && active_->submissionSerial == 0) {
            failTask(active_->taskId,
                     "application shut down while recording capture");
            active_.reset();
        } else {
            update(completedSubmissionSerial);
        }

        stopWorker();
        processWorkerResults();
        for (const CaptureTaskSnapshot &task : tasks_.snapshots()) {
            if (isTerminalCaptureTaskState(task.state))
                continue;
            if (task.state == CaptureTaskState::Cancelling) {
                tasks_.transition(task.request.taskId,
                                  CaptureTaskState::Cancelled);
            } else {
                failTask(task.request.taskId,
                         "application shut down before capture completed");
            }
            runtimes_.erase(task.request.taskId);
        }
        shutdown_ = true;
    }

    void workerLoop() {
        profileSetThreadName("CaptureEncode");
        while (true) {
            EncodeJob job;
            {
                std::unique_lock lock(workerMutex_);
                workerChanged_.wait(lock, [this] {
                    return workerStopping_ || !encodeJobs_.empty();
                });
                if (encodeJobs_.empty() && workerStopping_)
                    break;
                job = std::move(encodeJobs_.front());
                encodeJobs_.pop_front();
            }

            VKL_PROFILE_ZONE("Capture Encode");
            EncodeResult result = encodeCapture(std::move(job));
            {
                std::lock_guard lock(workerMutex_);
                encodeResults_.push_back(std::move(result));
            }
        }
    }

    void enqueueEncode(EncodeJob job) {
        ensureWorker();
        {
            std::lock_guard lock(workerMutex_);
            encodeJobs_.push_back(std::move(job));
        }
        workerChanged_.notify_one();
    }

    void ensureWorker() {
        if (worker_.joinable())
            return;
        workerStopping_ = false;
        worker_ = std::thread(&Impl::workerLoop, this);
    }

    void stopWorker() {
        if (!worker_.joinable())
            return;
        {
            std::lock_guard lock(workerMutex_);
            workerStopping_ = true;
        }
        workerChanged_.notify_all();
        worker_.join();
    }

    void processWorkerResults() {
        std::deque<EncodeResult> results;
        {
            std::lock_guard lock(workerMutex_);
            results.swap(encodeResults_);
        }
        for (EncodeResult &result : results) {
            CaptureTaskSnapshot *task = tasks_.find(result.taskId);
            const auto runtime = runtimes_.find(result.taskId);
            if (!task || runtime == runtimes_.end())
                continue;

            task->result.timings.encodeMs = result.encodeMs;
            task->result.timings.totalMs =
                elapsedMs(runtime->second.requestedAt, Clock::now());
            if (result.cancelled ||
                task->state == CaptureTaskState::Cancelling) {
                if (task->state != CaptureTaskState::Cancelling)
                    tasks_.transition(result.taskId,
                                      CaptureTaskState::Cancelling);
                tasks_.transition(result.taskId, CaptureTaskState::Cancelled);
            } else if (!result.success) {
                failTask(result.taskId, result.error);
            } else {
                task->result.sha256 = std::move(result.sha256);
                const CaptureResult completedResult = task->result;
                tasks_.transition(result.taskId, CaptureTaskState::Completed);
                VKR_LOG_INFO(
                    "Capture",
                    "Capture task {} completed: {}x{} {}, serial {}, '{}'",
                    result.taskId, completedResult.width,
                    completedResult.height,
                    describeCaptureFormat(completedResult.format).name,
                    completedResult.frameSerial,
                    completedResult.outputPath.string());
            }
            runtimes_.erase(result.taskId);
        }
    }

    void failTask(uint64_t taskId, const std::string &error) {
        CaptureTaskSnapshot *task = tasks_.find(taskId);
        if (!task || isTerminalCaptureTaskState(task->state))
            return;
        const std::string message =
            error.empty() ? "capture failed" : error;
        task->result.error = message;
        const auto runtime = runtimes_.find(taskId);
        if (runtime != runtimes_.end()) {
            task->result.timings.totalMs =
                elapsedMs(runtime->second.requestedAt, Clock::now());
        }
        tasks_.transition(taskId, CaptureTaskState::Failed);
        runtimes_.erase(taskId);
        VKR_LOG_ERROR("Capture", "Capture task {} failed: {}", taskId,
                      message);
    }

    struct TaskRuntime {
        Clock::time_point requestedAt{};
        Clock::time_point recordingAt{};
        Clock::time_point submittedAt{};
        std::shared_ptr<std::atomic_bool> cancelled;
    };

    struct ActiveCapture {
        uint64_t taskId = 0;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        CaptureImageSource source{};
        uint64_t byteSize = 0;
        uint64_t submissionSerial = 0;
        bool copyRecorded = false;
        std::unique_ptr<Buffer> buffer;
    };

    Device *device_ = nullptr;
    std::filesystem::path captureRoot_;
    CaptureTaskQueue tasks_;
    std::unordered_map<uint64_t, TaskRuntime> runtimes_;
    std::optional<ActiveCapture> active_;
    bool accepting_ = true;
    bool shutdown_ = false;

    std::mutex workerMutex_;
    std::condition_variable workerChanged_;
    std::deque<EncodeJob> encodeJobs_;
    std::deque<EncodeResult> encodeResults_;
    bool workerStopping_ = false;
    std::thread worker_;
};

CaptureService::CaptureService(Device &device,
                               std::filesystem::path captureRoot)
    : impl_(std::make_unique<Impl>(device, std::move(captureRoot))) {}

CaptureService::~CaptureService() = default;

uint64_t CaptureService::request(std::filesystem::path relativeOutputPath,
                                 bool includeGui) {
    return impl_->request(std::move(relativeOutputPath), includeGui);
}

uint64_t CaptureService::requestHdr(
    std::filesystem::path relativeOutputPath) {
    return impl_->requestHdr(std::move(relativeOutputPath));
}

bool CaptureService::cancel(uint64_t taskId) {
    return impl_->cancel(taskId);
}

std::optional<CaptureTaskSnapshot>
CaptureService::task(uint64_t taskId) const {
    return impl_->task(taskId);
}

std::vector<CaptureTaskSnapshot> CaptureService::tasks() const {
    return impl_->tasks();
}

std::optional<CaptureFrameSelection>
CaptureService::prepareFrame(const CaptureImageSource &viewport,
                             const CaptureImageSource &workspace,
                             const CaptureImageSource &hdr) {
    return impl_->prepareFrame(viewport, workspace, hdr);
}

void CaptureService::recordCopy(VkCommandBuffer commandBuffer) {
    impl_->recordCopy(commandBuffer);
}

void CaptureService::frameSubmitted(uint64_t submissionSerial) {
    impl_->frameSubmitted(submissionSerial);
}

void CaptureService::update(uint64_t completedSubmissionSerial) {
    impl_->update(completedSubmissionSerial);
}

void CaptureService::onSwapChainRecreated(
    uint64_t completedSubmissionSerial) {
    impl_->onSwapChainRecreated(completedSubmissionSerial);
}

void CaptureService::shutdown(uint64_t completedSubmissionSerial) {
    impl_->shutdown(completedSubmissionSerial);
}

const std::filesystem::path &CaptureService::captureRoot() const {
    return impl_->captureRoot();
}

bool CaptureService::acceptingRequests() const {
    return impl_->acceptingRequests();
}

} // namespace vkr
