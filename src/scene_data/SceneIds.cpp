#include "SceneIds.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace vkr {
namespace {

int hexValue(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

void validateStableId(const std::string &value, const char *kind) {
    if (!isStableAssetId(value))
        throw std::invalid_argument(std::string(kind) +
                                    " must be a stable lowercase asset ID");
}

} // namespace

bool isStableAssetId(const std::string &value) {
    if (value.empty())
        return false;
    for (size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        const bool valid =
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            (index > 0 && (character == '_' || character == '-'));
        if (!valid)
            return false;
    }
    return true;
}

PersistentEntityId PersistentEntityId::generate() {
    Bytes bytes{};
    const NTSTATUS result = BCryptGenRandom(
        nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (result < 0)
        throw std::runtime_error("Could not generate a persistent entity ID");

    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
    return PersistentEntityId(bytes);
}

std::optional<PersistentEntityId>
PersistentEntityId::parse(std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
        return std::nullopt;
    }

    Bytes bytes{};
    size_t byteIndex = 0;
    for (size_t index = 0; index < value.size();) {
        if (value[index] == '-') {
            ++index;
            continue;
        }
        if (index + 1 >= value.size() || byteIndex >= bytes.size())
            return std::nullopt;
        const int high = hexValue(value[index]);
        const int low = hexValue(value[index + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        bytes[byteIndex++] = static_cast<uint8_t>((high << 4) | low);
        index += 2;
    }
    if (byteIndex != bytes.size())
        return std::nullopt;
    return PersistentEntityId(bytes);
}

std::string PersistentEntityId::toString() const {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (size_t index = 0; index < bytes_.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            stream << '-';
        stream << std::setw(2) << static_cast<unsigned int>(bytes_[index]);
    }
    return stream.str();
}

bool PersistentEntityId::empty() const {
    return std::all_of(bytes_.begin(), bytes_.end(),
                       [](uint8_t value) { return value == 0; });
}

size_t PersistentEntityIdHash::operator()(
    const PersistentEntityId &id) const noexcept {
    size_t result = 1469598103934665603ull;
    for (const uint8_t value : id.bytes()) {
        result ^= value;
        result *= 1099511628211ull;
    }
    return result;
}

ModelAssetId::ModelAssetId(std::string value) : value_(std::move(value)) {
    validateStableId(value_, "Model asset ID");
}

SceneDocumentId::SceneDocumentId(std::string value)
    : value_(std::move(value)) {
    validateStableId(value_, "Scene document ID");
}

} // namespace vkr
