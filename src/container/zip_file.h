// Zip archive reader (read from a memory buffer).
//
// Used for three different jobs:
//   * Android .apk        - a plain zip
//   * .apks/.apkm/.xapk   - a zip *containing* apks (base.apk + split_*.apk)
//   * anything else       - to say "this is just a zip"
//
// Only the central directory is parsed up front; entries are inflated on
// demand. Stored (method 0) and deflated (method 8) entries are supported,
// zip64 records are followed, and everything is bounds checked because the
// input is untrusted.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apkinfo {

class ZipFile {
public:
    struct Entry {
        std::string name;
        uint16_t method = 0;          // 0 = stored, 8 = deflate, anything else unsupported
        uint16_t flags = 0;
        uint32_t crc32 = 0;
        uint64_t compressed_size = 0;
        uint64_t uncompressed_size = 0;
        uint64_t local_header_offset = 0;
        bool is_dir = false;

        bool is_utf8_name() const { return (flags & 0x0800) != 0; }
    };

    ZipFile() = default;

    // Parses the central directory of `zip`.
    // `zip` must stay alive (and unmoved) as long as this object is used.
    static std::optional<ZipFile> open(std::span<const uint8_t> zip, std::string* error);

    bool valid() const { return valid_; }
    const std::vector<Entry>& entries() const { return entries_; }
    // Entries joined with '\n' (still name-typed so callers can compare), the
    // raw central-directory comment ("" when absent).
    std::string comment() const { return comment_; }

    const Entry* find(std::string_view name) const;
    bool contains(std::string_view name) const { return find(name) != nullptr; }
    // All entries whose name ends with `suffix` (case insensitive), e.g. ".apk".
    std::vector<const Entry*> find_suffix(std::string_view suffix) const;
    // All entries whose name starts with `prefix`.
    std::vector<const Entry*> find_prefix(std::string_view prefix) const;

    // Extracts an entry into `out`. Returns false (and fills `error`) on
    // corruption, unsupported method or an implausible (huge) uncompressed size.
    bool extract(const Entry& entry, std::vector<uint8_t>& out, std::string* error) const;
    bool read(std::string_view name, std::vector<uint8_t>& out, std::string* error) const;
    // Extracts an entry to a file on disk, streaming (used for nested apks that
    // would otherwise need hundreds of MB of RAM).
    bool extract_to_file(const Entry& entry, const std::wstring& path, std::string* error) const;
    // Reads at most `max_bytes` of an entry (cheap sniffing of big entries).
    bool read_head(const Entry& entry, size_t max_bytes, std::vector<uint8_t>& out, std::string* error) const;

    // Total uncompressed size of all entries.
    uint64_t total_uncompressed_size() const;
    uint64_t total_compressed_size() const;

    // Offset/size of the central directory as recorded in the archive. Needed
    // to look for the APK Signing Block, which sits directly in front of the
    // central directory.
    uint64_t central_directory_offset() const { return cd_offset_; }
    uint64_t central_directory_size() const { return cd_size_; }

private:
    bool locate(std::span<const uint8_t> zip, std::string* error);
    // Locates the payload bytes of an entry, validating the local header.
    bool locate_payload(const Entry& entry, size_t* offset, size_t* length, std::string* error) const;

    std::span<const uint8_t> zip_{};
    std::vector<Entry> entries_;
    std::string comment_;
    uint64_t cd_offset_ = 0;
    uint64_t cd_size_ = 0;
    bool valid_ = false;
};

// Cheap magic check for "this is a zip archive".
bool looks_like_zip(std::span<const uint8_t> data);

}  // namespace apkinfo
