#include "util/hash.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <vector>

namespace apkinfo {
namespace {

std::string to_hex(const uint8_t* bytes, size_t count) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(count * 2);
    for (size_t i = 0; i < count; ++i) {
        out[i * 2] = kDigits[bytes[i] >> 4];
        out[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
    }
    return out;
}

// Streaming SHA-256: feed() may be called any number of times, finish() closes
// the provider exactly once. Shared by the memory and the file entry points.
class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return;
        DWORD object_size = 0, result_size = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                              sizeof object_size, &result_size, 0) < 0) {
            close();
            return;
        }
        if (BCryptGetProperty(algorithm_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size_),
                              sizeof hash_size_, &result_size, 0) < 0) {
            close();
            return;
        }
        object_.resize(object_size);
        if (BCryptCreateHash(algorithm_, &handle_, object_.data(), object_size, nullptr, 0, 0) < 0) {
            close();
            return;
        }
    }

    ~Sha256() { close(); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool ok() const { return handle_ != nullptr; }

    bool feed(const uint8_t* data, size_t size) {
        if (!handle_) return false;
        // Feed in chunks so a multi-hundred-megabyte mapping never needs one call.
        constexpr size_t kChunk = 4 * 1024 * 1024;
        size_t offset = 0;
        while (offset < size) {
            const size_t take = std::min(kChunk, size - offset);
            if (BCryptHashData(handle_, const_cast<PUCHAR>(data + offset), static_cast<ULONG>(take), 0) < 0) {
                return false;
            }
            offset += take;
        }
        return true;
    }

    std::string finish() {
        if (!handle_) return {};
        std::vector<uint8_t> digest(hash_size_);
        std::string hex;
        if (BCryptFinishHash(handle_, digest.data(), hash_size_, 0) >= 0) hex = to_hex(digest.data(), digest.size());
        close();
        return hex;
    }

private:
    void close() {
        if (handle_) {
            BCryptDestroyHash(handle_);
            handle_ = nullptr;
        }
        if (algorithm_) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
        }
    }

    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE handle_ = nullptr;
    std::vector<uint8_t> object_;
    DWORD hash_size_ = 0;
};

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.resize(static_cast<size_t>(length) - 1);  // drop the terminating NUL
    return wide;
}

}  // namespace

std::string sha256_hex(std::span<const uint8_t> data) {
    Sha256 hasher;
    if (!hasher.ok()) return {};
    if (!hasher.feed(data.data(), data.size())) return {};
    return hasher.finish();
}

std::string sha256_hex_file(const std::string& path) {
    const std::wstring wide = to_wide(path);
    if (wide.empty()) return {};
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    Sha256 hasher;
    bool ok = hasher.ok();
    std::vector<uint8_t> buffer(1024 * 1024);
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;  // end of file
        ok = hasher.feed(buffer.data(), read);
    }
    CloseHandle(file);
    if (!ok) return {};
    return hasher.finish();
}

}  // namespace apkinfo
