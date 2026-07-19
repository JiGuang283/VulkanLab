#include "ContentHash.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace vkr {
namespace {

class Sha256Hasher {
  public:
    Sha256Hasher() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM,
                                        nullptr, 0) < 0)
            throw std::runtime_error("Could not open SHA-256 provider");
        DWORD resultBytes = 0;
        DWORD objectBytes = 0;
        DWORD digestBytes = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectBytes),
                              sizeof(objectBytes), &resultBytes, 0) < 0 ||
            BCryptGetProperty(algorithm_, BCRYPT_HASH_LENGTH,
                              reinterpret_cast<PUCHAR>(&digestBytes),
                              sizeof(digestBytes), &resultBytes, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw std::runtime_error("Could not query SHA-256 provider");
        }
        object_.resize(objectBytes);
        digest_.resize(digestBytes);
        if (BCryptCreateHash(algorithm_, &hash_, object_.data(), objectBytes,
                             nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw std::runtime_error("Could not create SHA-256 hash");
        }
    }

    ~Sha256Hasher() {
        if (hash_)
            BCryptDestroyHash(hash_);
        if (algorithm_)
            BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    Sha256Hasher(const Sha256Hasher &) = delete;
    Sha256Hasher &operator=(const Sha256Hasher &) = delete;

    void update(const uint8_t *data, size_t size) {
        while (size > 0) {
            const ULONG chunk = static_cast<ULONG>(
                std::min<size_t>(size, static_cast<size_t>(ULONG_MAX)));
            if (BCryptHashData(hash_, const_cast<PUCHAR>(data), chunk, 0) < 0)
                throw std::runtime_error("Could not update SHA-256 hash");
            data += chunk;
            size -= chunk;
        }
    }

    std::string finish() {
        if (BCryptFinishHash(hash_, digest_.data(),
                             static_cast<ULONG>(digest_.size()), 0) < 0)
            throw std::runtime_error("Could not finish SHA-256 hash");
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (uint8_t byte : digest_)
            output << std::setw(2) << static_cast<unsigned int>(byte);
        return output.str();
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<uint8_t> object_;
    std::vector<uint8_t> digest_;
};

} // namespace

std::string sha256Bytes(const uint8_t *data, size_t size) {
    Sha256Hasher hasher;
    hasher.update(data, size);
    return hasher.finish();
}

std::string sha256Bytes(const std::vector<uint8_t> &bytes) {
    return sha256Bytes(bytes.data(), bytes.size());
}

std::string sha256String(const std::string &text) {
    return sha256Bytes(reinterpret_cast<const uint8_t *>(text.data()),
                       text.size());
}

std::string sha256File(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not open file for SHA-256: " +
                                 path.string());
    Sha256Hasher hasher;
    std::vector<uint8_t> buffer(1024 * 1024);
    while (input) {
        input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
        const std::streamsize count = input.gcount();
        if (count > 0)
            hasher.update(buffer.data(), static_cast<size_t>(count));
    }
    if (!input.eof())
        throw std::runtime_error("Could not read file for SHA-256: " +
                                 path.string());
    return hasher.finish();
}

} // namespace vkr
