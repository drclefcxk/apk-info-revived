#include "file_map.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace apkinfo {

FileMap::~FileMap() { close(); }

FileMap::FileMap(FileMap&& other) noexcept
    : file_(other.file_), mapping_(other.mapping_), data_(other.data_), size_(other.size_) {
    other.file_ = nullptr;
    other.mapping_ = nullptr;
    other.data_ = nullptr;
    other.size_ = 0;
}

FileMap& FileMap::operator=(FileMap&& other) noexcept {
    if (this != &other) {
        close();
        file_ = other.file_;
        mapping_ = other.mapping_;
        data_ = other.data_;
        size_ = other.size_;
        other.file_ = nullptr;
        other.mapping_ = nullptr;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void FileMap::close() {
    if (data_) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    if (file_ && file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(file_));
    }
    file_ = nullptr;
    size_ = 0;
}

std::optional<FileMap> FileMap::open(const std::string& utf8_path, std::string* error) {
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
    if (wide_len <= 0) {
        if (error) *error = "invalid UTF-8 path";
        return std::nullopt;
    }
    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, wide.data(), wide_len);
    if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
    return open(wide, error);
}

std::optional<FileMap> FileMap::open(const std::wstring& path, std::string* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot open file (error " + std::to_string(GetLastError()) + ")";
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        if (error) *error = "cannot read file size";
        return std::nullopt;
    }
    if (size.QuadPart == 0) {
        CloseHandle(file);
        if (error) *error = "file is empty";
        return std::nullopt;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        if (error) *error = "cannot map file (error " + std::to_string(GetLastError()) + ")";
        return std::nullopt;
    }
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(file);
        if (error) *error = "cannot map a view of the file";
        return std::nullopt;
    }
    FileMap result;
    result.file_ = file;
    result.mapping_ = mapping;
    result.data_ = static_cast<const uint8_t*>(view);
    result.size_ = static_cast<uint64_t>(size.QuadPart);
    return result;
}

}  // namespace apkinfo
