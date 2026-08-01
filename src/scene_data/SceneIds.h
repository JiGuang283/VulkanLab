#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace vkr {

bool isStableAssetId(const std::string &value);

class PersistentEntityId {
  public:
    using Bytes = std::array<uint8_t, 16>;

    PersistentEntityId() = default;
    explicit PersistentEntityId(Bytes bytes) : bytes_(bytes) {}

    static PersistentEntityId generate();
    static std::optional<PersistentEntityId> parse(std::string_view value);

    std::string toString() const;
    const Bytes &bytes() const { return bytes_; }
    bool empty() const;

    friend bool operator==(const PersistentEntityId &left,
                           const PersistentEntityId &right) {
        return left.bytes_ == right.bytes_;
    }
    friend bool operator!=(const PersistentEntityId &left,
                           const PersistentEntityId &right) {
        return !(left == right);
    }
    friend bool operator<(const PersistentEntityId &left,
                          const PersistentEntityId &right) {
        return left.bytes_ < right.bytes_;
    }

  private:
    Bytes bytes_{};
};

struct PersistentEntityIdHash {
    size_t operator()(const PersistentEntityId &id) const noexcept;
};

class ModelAssetId {
  public:
    ModelAssetId() = default;
    explicit ModelAssetId(std::string value);

    const std::string &value() const { return value_; }
    bool empty() const { return value_.empty(); }

    friend bool operator==(const ModelAssetId &left,
                           const ModelAssetId &right) {
        return left.value_ == right.value_;
    }

  private:
    std::string value_;
};

class SceneDocumentId {
  public:
    SceneDocumentId() = default;
    explicit SceneDocumentId(std::string value);

    const std::string &value() const { return value_; }
    bool empty() const { return value_.empty(); }

    friend bool operator==(const SceneDocumentId &left,
                           const SceneDocumentId &right) {
        return left.value_ == right.value_;
    }

  private:
    std::string value_;
};

} // namespace vkr

template <> struct std::hash<vkr::PersistentEntityId> {
    size_t operator()(const vkr::PersistentEntityId &id) const noexcept {
        return vkr::PersistentEntityIdHash{}(id);
    }
};
