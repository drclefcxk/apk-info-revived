// Read-only memory mapped file (Windows).
//
// APK bundles reach hundreds of megabytes (the Google Play Services sample is
// 191 MB uncompressed inside a 120 MB .apks), so files are mapped instead of
// copied into std::vector. This keeps a single view alive for the lifetime of
// the reader and lets the zip central directory be parsed in place.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace apkinfo {

class FileMap {
public:
    FileMap() = default;
    ~FileMap();
    FileMap(FileMap&& other) noexcept;
    FileMap& operator=(FileMap&& other) noexcept;
    FileMap(const FileMap&) = delete;
    FileMap& operator=(const FileMap&) = delete;

    // Maps the file read-only. Returns nullopt and fills `error` on failure.
    static std::optional<FileMap> open(const std::wstring& path, std::string* error);
    static std::optional<FileMap> open(const std::string& utf8_path, std::string* error);

    bool valid() const { return data_ != nullptr; }
    std::span<const uint8_t> bytes() const {
        return {data_, static_cast<size_t>(size_)};
    }
    uint64_t size() const { return size_; }

    void close();

private:
    void* file_ = nullptr;   // HANDLE
    void* mapping_ = nullptr;  // HANDLE
    const uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
};

}  // namespace apkinfo
