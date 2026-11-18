#include "zip_file.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace apkinfo {
namespace {

constexpr uint32_t kEocdSignature = 0x06054b50;       // "PK\5\6"
constexpr uint32_t kEocd64LocatorSignature = 0x07064b50;
constexpr uint32_t kEocd64Signature = 0x06064b50;
constexpr uint32_t kCentralDirSignature = 0x02014b50;  // "PK\1\2"
constexpr uint32_t kLocalHeaderSignature = 0x04034b50; // "PK\3\4"

// Hard ceiling for a single inflated entry. Icons/manifests are small; this
// only guards against zip bombs.
constexpr uint64_t kMaxEntrySize = 512ull * 1024 * 1024;
constexpr size_t kMaxEntries = 500000;

bool read16(std::span<const uint8_t> d, size_t off, uint16_t* out) {
    if (off + 2 > d.size()) return false;
    *out = static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
    return true;
}

bool read32(std::span<const uint8_t> d, size_t off, uint32_t* out) {
    if (off + 4 > d.size()) return false;
    *out = static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
    return true;
}

bool read64(std::span<const uint8_t> d, size_t off, uint64_t* out) {
    uint32_t lo = 0, hi = 0;
    if (!read32(d, off, &lo) || !read32(d, off + 4, &hi)) return false;
    *out = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    return true;
}

// Parses the zip64 extended information extra field (id 0x0001) for the values
// the central directory marked as 0xFFFFFFFF. Field order is fixed: usize,
// csize, local header offset, disk start - each present only when the
// corresponding central-directory field was 0xFFFFFFFF.
void apply_zip64_extra(std::span<const uint8_t> extra, uint64_t* usize, uint64_t* csize,
                       uint64_t* lho) {
    size_t off = 0;
    while (off + 4 <= extra.size()) {
        uint16_t id = static_cast<uint16_t>(extra[off] | (extra[off + 1] << 8));
        uint16_t len = static_cast<uint16_t>(extra[off + 2] | (extra[off + 3] << 8));
        off += 4;
        if (off + len > extra.size()) return;
        if (id == 0x0001) {
            auto field = extra.subspan(off, len);
            size_t p = 0;
            auto take64 = [&](uint64_t* dst) -> bool {
                if (p + 8 > field.size()) return false;
                uint64_t v = 0;
                if (!read64(field, p, &v)) return false;
                p += 8;
                *dst = v;
                return true;
            };
            if (usize && *usize == 0xFFFFFFFFull) take64(usize);
            if (csize && *csize == 0xFFFFFFFFull) take64(csize);
            if (lho && *lho == 0xFFFFFFFFull) take64(lho);
            return;
        }
        off += len;
    }
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    std::string_view tail = s.substr(s.size() - suffix.size());
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = tail[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace

bool looks_like_zip(std::span<const uint8_t> data) {
    if (data.size() < 4) return false;
    // "PK\3\4" (normal), "PK\5\6" (empty archive), "PK\7\10" (spanned)
    return (data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 && data[3] == 0x04) ||
           (data[0] == 'P' && data[1] == 'K' && data[2] == 0x05 && data[3] == 0x06) ||
           (data[0] == 'P' && data[1] == 'K' && data[2] == 0x07 && data[3] == 0x08);
}

std::optional<ZipFile> ZipFile::open(std::span<const uint8_t> zip, std::string* error) {
    ZipFile result;
    result.zip_ = zip;
    if (!result.locate(zip, error)) return std::nullopt;
    result.valid_ = true;
    return result;
}

bool ZipFile::locate(std::span<const uint8_t> zip, std::string* error) {
    const size_t size = zip.size();
    if (size < 22) {
        if (error) *error = "file too small for a zip archive";
        return false;
    }
    // Locate the End Of Central Directory record: it is at the end, followed by
    // a comment of at most 64 KiB.
    const size_t max_comment = 65535;
    size_t search_start = size >= max_comment + 22 ? size - max_comment - 22 : 0;
    std::optional<size_t> eocd;
    for (size_t i = size - 22 + 1; i-- > search_start;) {
        uint32_t sig = 0;
        if (!read32(zip, i, &sig) || sig != kEocdSignature) continue;
        uint16_t comment_len = 0;
        if (!read16(zip, i + 20, &comment_len)) continue;
        if (i + 22 + comment_len == size) {
            eocd = i;
            break;
        }
    }
    if (!eocd) {
        if (error) *error = "no zip end-of-central-directory record found";
        return false;
    }
    uint16_t eocd_comment_len = 0;
    read16(zip, *eocd + 20, &eocd_comment_len);
    comment_.assign(reinterpret_cast<const char*>(zip.data() + *eocd + 22), eocd_comment_len);

    uint16_t disk = 0, cd_disk = 0, entries_this_disk = 0, entries_total = 0;
    uint32_t cd_size32 = 0, cd_offset32 = 0;
    read16(zip, *eocd + 4, &disk);
    read16(zip, *eocd + 6, &cd_disk);
    read16(zip, *eocd + 8, &entries_this_disk);
    read16(zip, *eocd + 10, &entries_total);
    read32(zip, *eocd + 12, &cd_size32);
    read32(zip, *eocd + 16, &cd_offset32);

    uint64_t entries = entries_total;
    uint64_t cd_size = cd_size32;
    uint64_t cd_offset = cd_offset32;

    const bool needs_zip64 = entries_total == 0xFFFF || cd_size32 == 0xFFFFFFFFull ||
                             cd_offset32 == 0xFFFFFFFFull;
    if (needs_zip64) {
        // The zip64 EOCD locator sits immediately before the EOCD record.
        if (*eocd >= 20) {
            uint32_t locator_sig = 0;
            read32(zip, *eocd - 20, &locator_sig);
            if (locator_sig == kEocd64LocatorSignature) {
                uint64_t eocd64_offset = 0;
                read64(zip, *eocd - 20 + 8, &eocd64_offset);
                uint32_t eocd64_sig = 0;
                if (eocd64_offset < zip.size() && read32(zip, static_cast<size_t>(eocd64_offset), &eocd64_sig) &&
                    eocd64_sig == kEocd64Signature) {
                    size_t base = static_cast<size_t>(eocd64_offset);
                    read64(zip, base + 32, &entries);
                    read64(zip, base + 40, &cd_size);
                    read64(zip, base + 48, &cd_offset);
                }
            }
        }
    }
    (void)disk;
    (void)cd_disk;
    (void)entries_this_disk;

    if (cd_offset > zip.size() || cd_size > zip.size() || cd_offset + cd_size > zip.size()) {
        if (error) *error = "zip central directory lies outside the file";
        return false;
    }
    if (entries > kMaxEntries) {
        if (error) *error = "zip declares an implausible number of entries";
        return false;
    }

    cd_offset_ = cd_offset;
    cd_size_ = cd_size;

    entries_.reserve(static_cast<size_t>(entries));
    size_t pos = static_cast<size_t>(cd_offset);
    const size_t cd_end = static_cast<size_t>(cd_offset + cd_size);
    for (uint64_t i = 0; i < entries; ++i) {
        uint32_t sig = 0;
        if (!read32(zip, pos, &sig) || sig != kCentralDirSignature) {
            if (error) *error = "corrupt zip central directory entry";
            return false;
        }
        Entry entry;
        uint16_t name_len = 0, extra_len = 0, comment_len = 0;
        uint32_t crc = 0, csize32 = 0, usize32 = 0, lho32 = 0;
        read16(zip, pos + 4, &entry.method);  // version needed / flags follow
        read16(zip, pos + 6, &entry.flags);
        read16(zip, pos + 10, &entry.method);
        read32(zip, pos + 16, &crc);
        read32(zip, pos + 20, &csize32);
        read32(zip, pos + 24, &usize32);
        read16(zip, pos + 28, &name_len);
        read16(zip, pos + 30, &extra_len);
        read16(zip, pos + 32, &comment_len);
        read32(zip, pos + 42, &lho32);
        entry.crc32 = crc;
        entry.compressed_size = csize32;
        entry.uncompressed_size = usize32;
        entry.local_header_offset = lho32;

        const size_t name_off = pos + 46;
        if (name_off + name_len + extra_len + comment_len > cd_end) {
            if (error) *error = "zip central directory entry overruns the directory";
            return false;
        }
        entry.name.assign(reinterpret_cast<const char*>(zip.data() + name_off), name_len);
        if (extra_len) {
            apply_zip64_extra(zip.subspan(name_off + name_len, extra_len), &entry.uncompressed_size,
                              &entry.compressed_size, &entry.local_header_offset);
        }
        entry.is_dir = !entry.name.empty() && entry.name.back() == '/';
        if (entry.compressed_size > kMaxEntrySize || entry.uncompressed_size > kMaxEntrySize) {
            // Store it but let extraction refuse; keeps listing of huge media
            // entries working.
        }
        entries_.push_back(std::move(entry));
        pos = name_off + name_len + extra_len + comment_len;
    }
    return true;
}

const ZipFile::Entry* ZipFile::find(std::string_view name) const {
    for (const auto& e : entries_) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

std::vector<const ZipFile::Entry*> ZipFile::find_suffix(std::string_view suffix) const {
    std::vector<const Entry*> out;
    for (const auto& e : entries_) {
        if (!e.is_dir && ends_with_ci(e.name, suffix)) out.push_back(&e);
    }
    return out;
}

std::vector<const ZipFile::Entry*> ZipFile::find_prefix(std::string_view prefix) const {
    std::vector<const Entry*> out;
    for (const auto& e : entries_) {
        if (e.name.size() >= prefix.size() && e.name.compare(0, prefix.size(), prefix) == 0) {
            out.push_back(&e);
        }
    }
    return out;
}

uint64_t ZipFile::total_uncompressed_size() const {
    uint64_t total = 0;
    for (const auto& e : entries_) total += e.uncompressed_size;
    return total;
}

uint64_t ZipFile::total_compressed_size() const {
    uint64_t total = 0;
    for (const auto& e : entries_) total += e.compressed_size;
    return total;
}

bool ZipFile::locate_payload(const Entry& entry, size_t* offset, size_t* length,
                             std::string* error) const {
    const size_t lho = static_cast<size_t>(entry.local_header_offset);
    uint32_t sig = 0;
    if (!read32(zip_, lho, &sig) || sig != kLocalHeaderSignature) {
        if (error) *error = "corrupt zip local header for " + entry.name;
        return false;
    }
    uint16_t name_len = 0, extra_len = 0;
    read16(zip_, lho + 26, &name_len);
    read16(zip_, lho + 28, &extra_len);
    const size_t data_off = lho + 30 + name_len + extra_len;
    const size_t data_len = static_cast<size_t>(entry.compressed_size);
    if (data_off > zip_.size() || data_len > zip_.size() - data_off) {
        if (error) *error = "zip entry data lies outside the file: " + entry.name;
        return false;
    }
    *offset = data_off;
    *length = data_len;
    return true;
}

namespace {

// Inflates `src` (raw deflate) into a sink. `sink` receives chunks of output.
template <typename Sink>
bool inflate_raw(std::span<const uint8_t> src, uint64_t expected_size, Sink&& sink,
                 std::string* error) {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        if (error) *error = "zlib initialisation failed";
        return false;
    }
    stream.next_in = const_cast<Bytef*>(src.data());
    stream.avail_in = static_cast<uInt>(std::min<size_t>(src.size(), 0xFFFFFFFFu));
    std::array<uint8_t, 65536> buffer{};
    uint64_t produced = 0;
    bool ok = true;
    for (;;) {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        int rc = inflate(&stream, Z_NO_FLUSH);
        const size_t have = buffer.size() - stream.avail_out;
        if (have) {
            produced += have;
            if (produced > expected_size + 64) {
                if (error) *error = "zip entry inflates to more data than declared";
                ok = false;
                break;
            }
            if (!sink(std::span<const uint8_t>(buffer.data(), have))) {
                if (error) *error = "failed to write inflated data";
                ok = false;
                break;
            }
        }
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR && stream.avail_in == 0) {
            if (error) *error = "truncated deflate stream";
            ok = false;
            break;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            if (error) *error = std::string("inflate failed: ") + (stream.msg ? stream.msg : "inflate error");
            ok = false;
            break;
        }
        if (stream.avail_in == 0 && stream.avail_out == buffer.size()) {
            if (error) *error = "truncated deflate stream";
            ok = false;
            break;
        }
        if (stream.avail_in == 0 && src.size() > 0xFFFFFFFFu) {
            // Deflate streams we handle never exceed 4 GiB of input.
            if (error) *error = "zip entry too large to inflate";
            ok = false;
            break;
        }
    }
    inflateEnd(&stream);
    return ok;
}

}  // namespace

bool ZipFile::extract(const Entry& entry, std::vector<uint8_t>& out, std::string* error) const {
    size_t offset = 0, length = 0;
    if (!locate_payload(entry, &offset, &length, error)) return false;
    if (entry.uncompressed_size > kMaxEntrySize) {
        if (error) *error = "zip entry is larger than the supported maximum: " + entry.name;
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(entry.uncompressed_size));
    const auto src = zip_.subspan(offset, length);
    if (entry.method == 0) {
        out.assign(src.begin(), src.end());
        return true;
    }
    if (entry.method != 8) {
        if (error) *error = "unsupported zip compression method " + std::to_string(entry.method) +
                            " for " + entry.name;
        return false;
    }
    return inflate_raw(
        src, entry.uncompressed_size,
        [&out](std::span<const uint8_t> chunk) {
            out.insert(out.end(), chunk.begin(), chunk.end());
            return true;
        },
        error);
}

bool ZipFile::read(std::string_view name, std::vector<uint8_t>& out, std::string* error) const {
    const Entry* entry = find(name);
    if (!entry) {
        if (error) *error = "entry not found: " + std::string(name);
        return false;
    }
    return extract(*entry, out, error);
}

bool ZipFile::read_head(const Entry& entry, size_t max_bytes, std::vector<uint8_t>& out,
                        std::string* error) const {
    size_t offset = 0, length = 0;
    if (!locate_payload(entry, &offset, &length, error)) return false;
    out.clear();
    if (entry.method == 0) {
        const size_t take = std::min(max_bytes, length);
        out.assign(zip_.begin() + offset, zip_.begin() + offset + take);
        return true;
    }
    if (entry.method != 8) {
        if (error) *error = "unsupported zip compression method";
        return false;
    }
    const auto src = zip_.subspan(offset, length);
    return inflate_raw(
        src, entry.uncompressed_size,
        [&out, max_bytes](std::span<const uint8_t> chunk) {
            const size_t room = out.size() >= max_bytes ? 0 : max_bytes - out.size();
            const size_t take = std::min(room, chunk.size());
            out.insert(out.end(), chunk.begin(), chunk.begin() + take);
            return out.size() < max_bytes;  // stop early once we have enough
        },
        error);
}

bool ZipFile::extract_to_file(const Entry& entry, const std::wstring& path,
                              std::string* error) const {
    size_t offset = 0, length = 0;
    if (!locate_payload(entry, &offset, &length, error)) return false;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        if (error) *error = "cannot create temporary file";
        return false;
    }
    bool ok = true;
    const auto src = zip_.subspan(offset, length);
    if (entry.method == 0) {
        ok = fwrite(src.data(), 1, src.size(), file) == src.size();
        if (!ok && error) *error = "short write";
    } else if (entry.method == 8) {
        ok = inflate_raw(
            src, entry.uncompressed_size,
            [file](std::span<const uint8_t> chunk) {
                return fwrite(chunk.data(), 1, chunk.size(), file) == chunk.size();
            },
            error);
    } else {
        if (error) *error = "unsupported zip compression method";
        ok = false;
    }
    fclose(file);
    return ok;
}

}  // namespace apkinfo
