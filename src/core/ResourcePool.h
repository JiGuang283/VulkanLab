#pragma once

#include "ResourceHandle.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace vkr {

template <typename T, typename Tag> class ResourcePool {
  public:
    using HandleT = Handle<Tag>;

    template <typename... Args> HandleT emplace(Args &&...args);

    HandleT  insert(T value);
    T       *get(HandleT handle);
    const T *get(HandleT handle) const;
    bool     alive(HandleT handle) const;
    void     release(HandleT handle);
    void     clear();
    size_t   size() const { return liveCount_; }

  private:
    struct Slot {
        std::optional<T> value;
        uint32_t         generation = 1;
    };

    static uint32_t nextGeneration(uint32_t value) {
        ++value;
        return value == 0 ? 1 : value;
    }

    std::vector<Slot>     slots_;
    std::vector<uint32_t> freeList_;
    size_t                liveCount_ = 0;
};

template <typename T, typename Tag>
template <typename... Args>
typename ResourcePool<T, Tag>::HandleT
ResourcePool<T, Tag>::emplace(Args &&...args) {
    uint32_t index = 0;
    if (!freeList_.empty()) {
        index = freeList_.back();
        freeList_.pop_back();
        slots_[index].value.emplace(std::forward<Args>(args)...);
    } else {
        index = static_cast<uint32_t>(slots_.size());
        Slot slot;
        slot.value.emplace(std::forward<Args>(args)...);
        slots_.push_back(std::move(slot));
    }
    ++liveCount_;
    return HandleT{index, slots_[index].generation};
}

template <typename T, typename Tag>
typename ResourcePool<T, Tag>::HandleT ResourcePool<T, Tag>::insert(T value) {
    return emplace(std::move(value));
}

template <typename T, typename Tag>
T *ResourcePool<T, Tag>::get(HandleT handle) {
    if (!alive(handle))
        return nullptr;
    return &*slots_[handle.index].value;
}

template <typename T, typename Tag>
const T *ResourcePool<T, Tag>::get(HandleT handle) const {
    if (!alive(handle))
        return nullptr;
    return &*slots_[handle.index].value;
}

template <typename T, typename Tag>
bool ResourcePool<T, Tag>::alive(HandleT handle) const {
    if (!handle.valid() || handle.index >= slots_.size())
        return false;
    const Slot &slot = slots_[handle.index];
    return slot.value.has_value() && slot.generation == handle.generation;
}

template <typename T, typename Tag>
void ResourcePool<T, Tag>::release(HandleT handle) {
    if (!alive(handle))
        return;

    Slot &slot = slots_[handle.index];
    slot.value.reset();
    slot.generation = nextGeneration(slot.generation);
    freeList_.push_back(handle.index);
    --liveCount_;
}

template <typename T, typename Tag> void ResourcePool<T, Tag>::clear() {
    freeList_.clear();
    liveCount_ = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(slots_.size()); ++i) {
        Slot &slot = slots_[i];
        if (slot.value)
            slot.value.reset();
        slot.generation = nextGeneration(slot.generation);
        freeList_.push_back(i);
    }
}

} // namespace vkr