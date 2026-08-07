#include "EnvironmentAssetRepository.h"

#include "assets/DerivedEnvironmentCache.h"
#include "assets/EnvironmentLoadManager.h"
#include "assets/PreparedEnvironment.h"
#include "core/Device.h"
#include "core/Log.h"
#include "diagnostics/Profiling.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace vkr {
namespace detail {

struct EnvironmentAssetRecord {
    EnvironmentAssetRequest request;
    uint64_t generation = 0;
    std::atomic<EnvironmentAssetState> state{
        EnvironmentAssetState::Queued};
    std::atomic<uint64_t> consumers{0};
    std::shared_ptr<EnvironmentLoadTask> task;
    mutable std::mutex mutex;
    std::unique_ptr<PreparedEnvironmentData> prepared;
    std::shared_ptr<EnvironmentGpuResources> asset;
    std::string error;
    bool superseded = false;
    std::optional<uint64_t> retireAfterSerial;
};

struct EnvironmentAssetLease {
    explicit EnvironmentAssetLease(
        std::shared_ptr<EnvironmentAssetRecord> value)
        : record(std::move(value)) {
        ++record->consumers;
    }

    ~EnvironmentAssetLease() {
        if (record)
            --record->consumers;
    }

    std::shared_ptr<EnvironmentAssetRecord> record;
};

} // namespace detail

namespace {

using Record = detail::EnvironmentAssetRecord;

bool loadingState(EnvironmentAssetState state) {
    return state == EnvironmentAssetState::Queued ||
           state == EnvironmentAssetState::PreparingCpu ||
           state == EnvironmentAssetState::ReadyForUpload ||
           state == EnvironmentAssetState::Uploading ||
           state == EnvironmentAssetState::WaitingForGpu;
}

EnvironmentAssetState builderState(EnvironmentLoadState state) {
    switch (state) {
    case EnvironmentLoadState::Uploading:
        return EnvironmentAssetState::Uploading;
    case EnvironmentLoadState::WaitingForGpu:
    case EnvironmentLoadState::ReadyToPublish:
        return EnvironmentAssetState::WaitingForGpu;
    case EnvironmentLoadState::Cancelling:
        return EnvironmentAssetState::WaitingForGpu;
    case EnvironmentLoadState::Cancelled:
        return EnvironmentAssetState::Cancelled;
    case EnvironmentLoadState::Failed:
        return EnvironmentAssetState::Failed;
    default:
        return EnvironmentAssetState::Uploading;
    }
}

} // namespace

const char *environmentAssetStateName(EnvironmentAssetState state) {
    switch (state) {
    case EnvironmentAssetState::Unloaded:
        return "Unloaded";
    case EnvironmentAssetState::Queued:
        return "Queued";
    case EnvironmentAssetState::PreparingCpu:
        return "PreparingCpu";
    case EnvironmentAssetState::ReadyForUpload:
        return "ReadyForUpload";
    case EnvironmentAssetState::Uploading:
        return "Uploading";
    case EnvironmentAssetState::WaitingForGpu:
        return "WaitingForGpu";
    case EnvironmentAssetState::Ready:
        return "Ready";
    case EnvironmentAssetState::Failed:
        return "Failed";
    case EnvironmentAssetState::Cancelled:
        return "Cancelled";
    case EnvironmentAssetState::Retiring:
        return "Retiring";
    }
    return "Unknown";
}

size_t EnvironmentAssetKeyHash::operator()(
    const EnvironmentAssetKey &key) const noexcept {
    const size_t left = std::hash<std::string>{}(key.environmentId);
    const size_t right = std::hash<std::string>{}(key.profileId);
    return left ^ (right + 0x9e3779b9u + (left << 6u) + (left >> 2u));
}

EnvironmentAssetKey EnvironmentAssetHandle::key() const {
    return lease_ && lease_->record ? lease_->record->request.key
                                    : EnvironmentAssetKey{};
}

uint64_t EnvironmentAssetHandle::taskId() const {
    return lease_ && lease_->record && lease_->record->task
               ? lease_->record->task->id
               : 0;
}

uint64_t EnvironmentAssetHandle::generation() const {
    return lease_ && lease_->record ? lease_->record->generation : 0;
}

EnvironmentAssetState EnvironmentAssetHandle::state() const {
    return lease_ && lease_->record ? lease_->record->state.load()
                                    : EnvironmentAssetState::Unloaded;
}

std::shared_ptr<EnvironmentGpuResources>
EnvironmentAssetHandle::asset() const {
    if (!lease_ || !lease_->record)
        return {};
    std::lock_guard<std::mutex> lock(lease_->record->mutex);
    return lease_->record->asset;
}

EnvironmentAssetHandleSnapshot EnvironmentAssetHandle::snapshot() const {
    EnvironmentAssetHandleSnapshot result{};
    if (!lease_ || !lease_->record)
        return result;
    const Record &record = *lease_->record;
    result.key = record.request.key;
    result.generation = record.generation;
    result.state = record.state.load();
    result.uploadedImages = record.task ? record.task->uploadedImages.load() : 0;
    if (record.task)
        result.totalImages = record.task->totalImages;
    std::lock_guard<std::mutex> lock(record.mutex);
    result.error = record.error;
    return result;
}

class EnvironmentAssetRepository::Impl {
  public:
    explicit Impl(Device &device)
        : device_(&device), worker_([this] { workerLoop(); }) {}

    ~Impl() { shutdown(); }

    EnvironmentAssetHandle request(const EnvironmentAssetRequest &request,
                                   bool *repositoryHit,
                                   bool *coalesced) {
        if (repositoryHit)
            *repositoryHit = false;
        if (coalesced)
            *coalesced = false;
        if (request.key.environmentId.empty() ||
            request.key.profileId.empty() || request.cacheRoot.empty()) {
            throw std::invalid_argument(
                "Environment asset request is incomplete");
        }

        auto found = active_.find(request.key);
        if (found != active_.end() &&
            request.policy == EnvironmentAssetRequestPolicy::UseCached) {
            const std::shared_ptr<Record> &record = found->second;
            const EnvironmentAssetState state = record->state.load();
            if (state == EnvironmentAssetState::Ready ||
                state == EnvironmentAssetState::Retiring) {
                record->retireAfterSerial.reset();
                record->state = EnvironmentAssetState::Ready;
                ++readyHits_;
                if (repositoryHit)
                    *repositoryHit = true;
                return makeHandle(record);
            }
            if (loadingState(state)) {
                ++coalescedRequests_;
                if (coalesced)
                    *coalesced = true;
                return makeHandle(record);
            }
        }

        if (found != active_.end())
            supersede(found->second);

        auto record = std::make_shared<Record>();
        record->request = request;
        record->generation = nextGeneration_++;
        record->task = std::make_shared<EnvironmentLoadTask>();
        record->task->id = nextTaskId_++;
        record->task->generation = record->generation;
        record->task->environmentId = request.key.environmentId;
        record->task->displayName = request.displayName;
        record->task->profileId = request.key.profileId;
        EnvironmentAssetHandle handle = makeHandle(record);
        active_[request.key] = record;
        tasks_[record->task->id] = record;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            cpuQueue_.push_back(record);
        }
        queueCondition_.notify_one();
        VKR_LOG_INFO("EnvironmentAsset", "Queued '{}:{}' generation {}",
                     request.key.environmentId, request.key.profileId,
                     record->generation);
        return handle;
    }

    void pump(const EnvironmentGpuBuilder::Budget &budget) {
        VKL_PROFILE_ZONE("EnvironmentAssetRepository::pump");
        if (gpuBuilder_) {
            gpuBuilder_->pump(budget);
            gpuRecord_->state = builderState(gpuRecord_->task->state.load());
            if (gpuBuilder_->ready()) {
                {
                    std::lock_guard<std::mutex> lock(gpuRecord_->mutex);
                    gpuRecord_->asset = gpuBuilder_->takeResources();
                }
                gpuRecord_->task->state = EnvironmentLoadState::Completed;
                gpuRecord_->state = EnvironmentAssetState::Ready;
                VKR_LOG_INFO("EnvironmentAsset",
                             "Ready '{}:{}' generation {}",
                             gpuRecord_->request.key.environmentId,
                             gpuRecord_->request.key.profileId,
                             gpuRecord_->generation);
                gpuBuilder_.reset();
                gpuRecord_.reset();
            } else if (gpuBuilder_->finished()) {
                const EnvironmentLoadState taskState =
                    gpuRecord_->task->state.load();
                {
                    std::lock_guard<std::mutex> taskLock(
                        gpuRecord_->task->mutex);
                    std::lock_guard<std::mutex> recordLock(
                        gpuRecord_->mutex);
                    gpuRecord_->error = gpuRecord_->task->error;
                }
                gpuRecord_->state =
                    taskState == EnvironmentLoadState::Cancelled
                        ? EnvironmentAssetState::Cancelled
                        : EnvironmentAssetState::Failed;
                gpuBuilder_.reset();
                gpuRecord_.reset();
            }
        }

        if (gpuBuilder_)
            return;

        for (;;) {
            std::shared_ptr<Record> record;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (gpuQueue_.empty())
                    return;
                record = std::move(gpuQueue_.front());
                gpuQueue_.pop_front();
            }
            if (!record || record->state.load() !=
                               EnvironmentAssetState::ReadyForUpload) {
                continue;
            }
            if (record->consumers.load() == 0 ||
                record->task->cancellation->load()) {
                record->task->cancellation->store(true);
                record->state = EnvironmentAssetState::Cancelled;
                continue;
            }

            std::unique_ptr<PreparedEnvironmentData> prepared;
            {
                std::lock_guard<std::mutex> lock(record->mutex);
                prepared = std::move(record->prepared);
            }
            if (!prepared) {
                record->state = EnvironmentAssetState::Failed;
                std::lock_guard<std::mutex> lock(record->mutex);
                record->error = "Prepared environment data is unavailable";
                continue;
            }
            gpuRecord_ = record;
            gpuBuilder_ = std::make_unique<EnvironmentGpuBuilder>(
                *device_, record->task, std::move(prepared));
            record->state = EnvironmentAssetState::Uploading;
            ++gpuBuildStarts_;
            return;
        }
    }

    bool cancel(uint64_t taskId) {
        const auto found = tasks_.find(taskId);
        if (found == tasks_.end())
            return false;
        const std::shared_ptr<Record> record = found->second.lock();
        if (!record)
            return false;
        const EnvironmentAssetState state = record->state.load();
        if (!loadingState(state))
            return false;
        record->task->cancellation->store(true);
        if (state == EnvironmentAssetState::Queued ||
            state == EnvironmentAssetState::ReadyForUpload) {
            record->state = EnvironmentAssetState::Cancelled;
            record->task->state = EnvironmentLoadState::Cancelled;
        }
        return true;
    }

    std::shared_ptr<EnvironmentLoadTask> task(uint64_t taskId) const {
        const auto found = tasks_.find(taskId);
        if (found == tasks_.end())
            return {};
        const std::shared_ptr<Record> record = found->second.lock();
        return record ? record->task : nullptr;
    }

    void invalidate(const std::string &environmentId,
                    const std::string *profileId) {
        for (auto it = active_.begin(); it != active_.end();) {
            if (it->first.environmentId == environmentId &&
                (!profileId || it->first.profileId == *profileId)) {
                supersede(it->second);
                it = active_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void releaseUnused(uint64_t lastSubmittedSerial,
                       uint64_t completedSerial) {
        for (auto it = active_.begin(); it != active_.end();) {
            if (collectRecord(it->second, lastSubmittedSerial,
                              completedSerial)) {
                it = active_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = inactive_.begin(); it != inactive_.end();) {
            if (collectRecord(*it, lastSubmittedSerial, completedSerial))
                it = inactive_.erase(it);
            else
                ++it;
        }
    }

    EnvironmentAssetRepositorySnapshot snapshot() const {
        EnvironmentAssetRepositorySnapshot result{};
        result.cpuPrepareStarts = cpuPrepareStarts_.load();
        result.gpuBuildStarts = gpuBuildStarts_;
        result.readyHits = readyHits_;
        result.coalescedRequests = coalescedRequests_;
        const auto append = [&](const std::shared_ptr<Record> &record) {
            if (!record)
                return;
            EnvironmentAssetRecordSnapshot item{};
            item.key = record->request.key;
            item.generation = record->generation;
            item.state = record->state.load();
            item.consumerCount = record->consumers.load();
            item.uploadedImages =
                record->task ? record->task->uploadedImages.load() : 0;
            {
                std::lock_guard<std::mutex> lock(record->mutex);
                item.error = record->error;
            }
            ++result.recordCount;
            if (item.state == EnvironmentAssetState::Ready)
                ++result.readyCount;
            else if (item.state == EnvironmentAssetState::Retiring)
                ++result.retiringCount;
            else if (item.state == EnvironmentAssetState::Failed)
                ++result.failedCount;
            else if (loadingState(item.state))
                ++result.loadingCount;
            result.records.push_back(std::move(item));
        };
        for (const auto &pair : active_)
            append(pair.second);
        for (const auto &record : inactive_)
            append(record);
        std::sort(result.records.begin(), result.records.end(),
                  [](const auto &left, const auto &right) {
                      if (left.key.environmentId !=
                          right.key.environmentId) {
                          return left.key.environmentId <
                                 right.key.environmentId;
                      }
                      if (left.key.profileId != right.key.profileId)
                          return left.key.profileId < right.key.profileId;
                      return left.generation < right.generation;
                  });
        return result;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (stopping_)
                return;
            stopping_ = true;
            for (auto &pair : active_)
                pair.second->task->cancellation->store(true);
            for (auto &record : inactive_)
                record->task->cancellation->store(true);
        }
        queueCondition_.notify_all();
        if (worker_.joinable())
            worker_.join();
        if (gpuBuilder_)
            gpuBuilder_->cancel();
        gpuBuilder_.reset();
        gpuRecord_.reset();
        active_.clear();
        inactive_.clear();
    }

  private:
    EnvironmentAssetHandle makeHandle(
        const std::shared_ptr<Record> &record) {
        return EnvironmentAssetHandle(
            std::make_shared<detail::EnvironmentAssetLease>(record));
    }

    void supersede(const std::shared_ptr<Record> &record) {
        if (!record)
            return;
        record->superseded = true;
        if (record->consumers.load() == 0 &&
            loadingState(record->state.load())) {
            record->task->cancellation->store(true);
        }
        inactive_.push_back(record);
    }

    bool collectRecord(const std::shared_ptr<Record> &record,
                       uint64_t lastSubmittedSerial,
                       uint64_t completedSerial) {
        if (!record)
            return true;
        if (record->consumers.load() > 0) {
            if (record->state.load() == EnvironmentAssetState::Retiring) {
                record->retireAfterSerial.reset();
                record->state = EnvironmentAssetState::Ready;
            }
            return false;
        }
        EnvironmentAssetState state = record->state.load();
        if (loadingState(state)) {
            record->task->cancellation->store(true);
            return false;
        }
        if (state == EnvironmentAssetState::Failed ||
            state == EnvironmentAssetState::Cancelled) {
            return true;
        }
        if (state == EnvironmentAssetState::Ready) {
            record->retireAfterSerial = lastSubmittedSerial;
            record->state = EnvironmentAssetState::Retiring;
            state = EnvironmentAssetState::Retiring;
        }
        if (state == EnvironmentAssetState::Retiring &&
            record->retireAfterSerial &&
            completedSerial >= *record->retireAfterSerial) {
            std::lock_guard<std::mutex> lock(record->mutex);
            record->asset.reset();
            return true;
        }
        return false;
    }

    void workerLoop() {
        profileSetThreadName("EnvironmentPrepare");
        for (;;) {
            std::shared_ptr<Record> record;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCondition_.wait(lock, [this] {
                    return stopping_ || !cpuQueue_.empty();
                });
                if (stopping_)
                    return;
                record = std::move(cpuQueue_.front());
                cpuQueue_.pop_front();
            }
            if (!record || record->task->cancellation->load() ||
                record->consumers.load() == 0) {
                if (record)
                    record->state = EnvironmentAssetState::Cancelled;
                continue;
            }

            record->state = EnvironmentAssetState::PreparingCpu;
            record->task->state = EnvironmentLoadState::PreparingCpu;
            ++cpuPrepareStarts_;
            try {
                DerivedEnvironmentCache cache(
                    record->request.cacheRoot,
                    record->request.sourcePath,
                    record->request.projectId,
                    record->request.key.environmentId,
                    record->request.displayName,
                    record->request.key.profileId,
                    record->request.validateSource);
                auto prepared =
                    std::make_unique<PreparedEnvironmentData>(cache.load());
                if (record->task->cancellation->load()) {
                    record->state = EnvironmentAssetState::Cancelled;
                    record->task->state = EnvironmentLoadState::Cancelled;
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(record->mutex);
                    record->prepared = std::move(prepared);
                }
                record->state = EnvironmentAssetState::ReadyForUpload;
                record->task->state = EnvironmentLoadState::ReadyForUpload;
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    gpuQueue_.push_back(record);
                }
            } catch (const std::exception &error) {
                std::lock_guard<std::mutex> lock(record->mutex);
                if (record->task->cancellation->load()) {
                    record->state = EnvironmentAssetState::Cancelled;
                    record->task->state = EnvironmentLoadState::Cancelled;
                } else {
                    record->error = error.what();
                    record->state = EnvironmentAssetState::Failed;
                    record->task->state = EnvironmentLoadState::Failed;
                    VKR_LOG_ERROR("EnvironmentAsset",
                                  "Prepare '{}:{}' failed: {}",
                                  record->request.key.environmentId,
                                  record->request.key.profileId,
                                  record->error);
                }
            }
        }
    }

    Device *device_ = nullptr;
    std::unordered_map<EnvironmentAssetKey, std::shared_ptr<Record>,
                       EnvironmentAssetKeyHash>
        active_;
    std::unordered_map<uint64_t, std::weak_ptr<Record>> tasks_;
    std::vector<std::shared_ptr<Record>> inactive_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::deque<std::shared_ptr<Record>> cpuQueue_;
    std::deque<std::shared_ptr<Record>> gpuQueue_;
    std::thread worker_;
    bool stopping_ = false;
    std::shared_ptr<Record> gpuRecord_;
    std::unique_ptr<EnvironmentGpuBuilder> gpuBuilder_;
    uint64_t nextTaskId_ = EnvironmentLoadManager::kTaskIdMask;
    uint64_t nextGeneration_ = 1;
    std::atomic<uint64_t> cpuPrepareStarts_{0};
    uint64_t gpuBuildStarts_ = 0;
    uint64_t readyHits_ = 0;
    uint64_t coalescedRequests_ = 0;
};

EnvironmentAssetRepository::EnvironmentAssetRepository(Device &device)
    : impl_(std::make_unique<Impl>(device)) {}

EnvironmentAssetRepository::~EnvironmentAssetRepository() = default;

EnvironmentAssetHandle EnvironmentAssetRepository::request(
    const EnvironmentAssetRequest &request, bool *repositoryHit,
    bool *coalesced) {
    return impl_->request(request, repositoryHit, coalesced);
}

void EnvironmentAssetRepository::pump(
    const EnvironmentGpuBuilder::Budget &budget) {
    impl_->pump(budget);
}

bool EnvironmentAssetRepository::cancel(uint64_t taskId) {
    return impl_->cancel(taskId);
}

std::shared_ptr<EnvironmentLoadTask>
EnvironmentAssetRepository::task(uint64_t taskId) const {
    return impl_->task(taskId);
}

void EnvironmentAssetRepository::invalidate(
    const std::string &environmentId, const std::string *profileId) {
    impl_->invalidate(environmentId, profileId);
}

void EnvironmentAssetRepository::releaseUnused(
    uint64_t lastSubmittedSerial, uint64_t completedSerial) {
    impl_->releaseUnused(lastSubmittedSerial, completedSerial);
}

EnvironmentAssetRepositorySnapshot
EnvironmentAssetRepository::snapshot() const {
    return impl_->snapshot();
}

void EnvironmentAssetRepository::shutdown() { impl_->shutdown(); }

} // namespace vkr
