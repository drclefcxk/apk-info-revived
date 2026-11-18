// alpine.cpp - Alpine Package Keeper (.apk) reader: v2 (apk-tools 2.x) and
//              v3 (apk-tools 3.x "adb" container).
//
// Reverse-engineered against, and cross-checked with, the real apk-tools
// sources (src/adb.h, src/adb.c, src/adb_comp.c, src/apk_adb.c,
// src/extract_v3.c, src/extract_v2.c, src/package.c, src/version.c): those are
// the authority; comments below cite them where the wire format is subtle.
// Only zlib (and optionally zstd) is used.  No OS headers, no exceptions
// escape, every read from the input is bounds checked.
//
// ===========================================================================
// v3 container
// ===========================================================================
//   [0..2]  "ADB"
//   [3]     compression marker (see apk-tools adb_decompress()):
//             '.'  no compression - the adb stream starts AT OFFSET 0, i.e.
//                  the "ADB." magic of the adb file header doubles as marker
//                  (apk-tools' uncompressed .apk therefore begins "ADB.pckg")
//             'd'  raw deflate, windowBits -15 (no zlib/gzip wrapper), at 4
//             'c'  custom: [4] = alg, [5] = level (struct
//                  adb_compression_spec { uint8 alg; uint8 level; },
//                  alg 0 = none, 1 = deflate, 2 = zstd), data at 6
//           plus, accepted defensively because other writers/documentation
//           use them: 'z' = zstd at 4, 'n' = no compression at 4.
//
// The decompressed "adb stream" is
//     struct adb_file_header { uint32 magic = 0x2e424441 "ADB."; uint32 schema; }
//     block, block, ...                        (the first block starts at 8)
//
// A normal block starts with 4 bytes (type_size) only:
//     type    = type_size >> 30                  0 ADB, 1 SIG, 2 DATA, 3 EXT
//     rawsize = type_size & 0x3fffffff           header + payload
//     payload = rawsize - 4
// A block whose type_size>>30 == 3 (EXT) uses the full 16-byte
// struct adb_block { uint32 type_size; uint32 reserved; uint64 x_size; }:
//     type    = type_size & 0x3fffffff
//     rawsize = x_size                           header + payload
//     payload = rawsize - 16
// Every block is followed by zero padding up to a multiple of 8
// (ADB_BLOCK_ALIGNMENT), so walking always advances ROUND_UP(rawsize, 8).
// Order: exactly one ADB block, then optional SIG blocks, then DATA blocks.
//
// The ADB block payload is the "adb blob":
//     [ adb_file_header  8 bytes: "ADB." + schema ("pckg" = 0x676b6370) ]
//     [ struct adb_hdr   8 bytes: u8 adb_compat_ver; u8 adb_ver;
//                                 u16 reserved; u32 root ]
//     [ object tree ]
// !! All 28-bit value offsets are relative to the START OF struct adb_hdr,
// !! i.e. adb-blob offset sizeof(adb_file_header) = 8.  apk-tools does exactly
// !! that: __adb_m_mmap()/__adb_m_stream() hand `payload + 8` to
// !! __adb_m_parse(), which stores it as db->adb; adb_r_root() reads
// !! ((struct adb_hdr*)db->adb.ptr)->root and adb_r_deref() adds the 28-bit
// !! offset to db->adb.ptr.
// !! Sample package (v3, OpenWrt): the ADB block header is the 4 bytes at
// !! stream offset 8 (0x00001ce0 = type 0, rawsize 7392), so the adb blob
// !! starts at stream offset 12; its first 8 bytes are the adb_hdr
// !! (00 00 00 00 | root = 0xe0001ccc).  root therefore addresses
// !! payload offset 8 + 0x1ccc and the tree begins right after the header.
//
// adb wire values (src/adb.h): uint32le tag, high 4 bits type, low 28 bits
// either an immediate or an offset:
//     0x0 special (0 NULL, 1 TRUE, 2 FALSE)   0x1 Int    (immediate)
//     0x2 Int32  (offset -> u32)              0x3 Int64  (offset -> u64)
//     0x8 Blob8  (offset -> u8 len + bytes)   0x9 Blob16   0xa Blob32
//     0xd Array  (offset -> slots)            0xe Object (offset -> slots)
// A slots array starts with a uint32 count that INCLUDES itself, so an object
// has count-1 fields: field i (1-based, ADBI_FIRST = 1) lives at slot i.
//
// ===========================================================================
// v2 container
// ===========================================================================
// Concatenated gzip streams:
//   [ tar with .SIGN.RSA.<key> ][ tar with .PKGINFO + .<script>s ][ data.tar.gz ]
// The signature stream is optional and apk-tools tolerates any number of
// streams, so all of them are walked and each is classified by its members.
// ---------------------------------------------------------------------------
#include "alpine.h"

#include <zlib.h>

#ifdef APKINFO_HAVE_ZSTD
#include <zstd.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apkinfo::alpine {
namespace {

// ---------------------------------------------------------------------------
// limits (everything here is untrusted input)
// ---------------------------------------------------------------------------
constexpr uint64_t kMaxAdbBlockBytes = 256ull << 20;  // one ADB block payload
constexpr uint64_t kMaxDecodedBytes = 512ull << 20;   // everything we inflate
constexpr uint64_t kMaxSigBlockBytes = 64ull << 10;   // SIG block payload
constexpr size_t kMaxFileEntries = 200000;
constexpr size_t kMaxAdbSlots = 1u << 20;     // slots in one array/object
constexpr size_t kMaxScriptBytes = 1u << 20;  // one control script body
constexpr size_t kMaxPkgInfoBytes = 8u << 20;
constexpr size_t kMaxTarMetaBytes = 64u << 10;  // 'L'/'K'/'x' payloads
constexpr size_t kTarBlock = 512;

// ---------------------------------------------------------------------------
// little endian helpers
// ---------------------------------------------------------------------------
inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t rd64(const uint8_t* p) {
    return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32);
}
inline uint64_t round_up8(uint64_t v) { return (v + 7u) & ~uint64_t{7}; }

std::string hex_of(std::span<const uint8_t> b) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) {
        s.push_back(kDigits[c >> 4]);
        s.push_back(kDigits[c & 0xf]);
    }
    return s;
}

std::string to_string(std::span<const uint8_t> b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// ---------------------------------------------------------------------------
// dependency text form: [!]name[op version]
// (src/package.c apk_dep_parse(); APK_CTYPE_DEPENDENCY_COMPARER = '<' '>' '=' '~')
// ---------------------------------------------------------------------------
bool is_cmp(char c) { return c == '<' || c == '>' || c == '=' || c == '~'; }

bool split_dep_text(std::string_view s, Dependency& out) {
    out = Dependency{};
    s = trim(s);
    if (s.empty()) return false;

    bool conflict = false;
    if (s.front() == '!') {
        conflict = true;
        s.remove_prefix(1);
    }

    std::size_t i = 0;
    while (i < s.size() && !is_cmp(s[i])) ++i;
    out.name.assign(s.substr(0, i));
    if (out.name.empty()) return false;
    if (i < s.size()) {
        std::size_t e = i;
        while (e < s.size() && is_cmp(s[e])) ++e;
        out.op.assign(s.substr(i, e - i));
        out.version.assign(s.substr(e));
    }
    // The conflict marker is carried in `op` so to_string() can rebuild
    // "!name>=ver" exactly as apk_blob_push_dep() prints it.
    if (conflict) out.op.insert(out.op.begin(), '!');
    return true;
}

// APK_VERSION_* mask (src/apk_version.h) -> textual operator, mirroring
// apk_version_op_string().
std::string op_from_mask(uint64_t mask) {
    const bool conflict = (mask & 16u) != 0;  // APK_VERSION_CONFLICT
    std::string op;
    switch (mask & ~uint64_t{16}) {
        case 0:  // only used together with a version: treat as EQUAL
        case 1: op = "="; break;   // APK_VERSION_EQUAL
        case 2: op = "<"; break;   // APK_VERSION_LESS
        case 3: op = "<="; break;  // LESS|EQUAL
        case 4: op = ">"; break;   // APK_VERSION_GREATER
        case 5: op = ">="; break;  // GREATER|EQUAL
        case 8:
        case 9: op = "~="; break;   // FUZZY / EQUAL|FUZZY
        case 11: op = "<~"; break;  // LESS|EQUAL|FUZZY
        case 13: op = ">~"; break;  // GREATER|EQUAL|FUZZY
        case 6: op = "><"; break;   // APK_DEPMASK_CHECKSUM
        case 7: op = ""; break;     // APK_DEPMASK_ANY
        default: op = "?"; break;
    }
    if (conflict) op.insert(op.begin(), '!');
    return op;
}

// ---------------------------------------------------------------------------
// adb (v3 object tree) reader
// ---------------------------------------------------------------------------
constexpr uint32_t kAdbTypeInt = 0x1;
constexpr uint32_t kAdbTypeInt32 = 0x2;
constexpr uint32_t kAdbTypeInt64 = 0x3;
constexpr uint32_t kAdbTypeBlob8 = 0x8;
constexpr uint32_t kAdbTypeBlob16 = 0x9;
constexpr uint32_t kAdbTypeBlob32 = 0xa;
constexpr uint32_t kAdbTypeArray = 0xd;
constexpr uint32_t kAdbTypeObject = 0xe;
constexpr uint32_t kAdbValueMask = 0x0fffffffu;

inline uint32_t adb_type(uint32_t v) { return v >> 28; }
inline uint32_t adb_value(uint32_t v) { return v & kAdbValueMask; }
inline bool adb_is_obj(uint32_t v) {
    const uint32_t t = adb_type(v);
    return t == kAdbTypeArray || t == kAdbTypeObject;
}

class AdbReader {
public:
    explicit AdbReader(std::span<const uint8_t> blob) : blob_(blob) {}

    // Decode an Array/Object value into its field values: out[i-1] == field i.
    bool obj(uint32_t v, std::vector<uint32_t>& out) const {
        out.clear();
        if (!adb_is_obj(v)) return false;
        const uint64_t off = adb_value(v);
        if (off + 4 > blob_.size()) return false;
        const uint32_t count = rd32(blob_.data() + off);
        if (count == 0 || count > kMaxAdbSlots) return false;
        if (off + uint64_t{4} * count > blob_.size()) return false;
        out.resize(count - 1);
        for (uint32_t i = 1; i < count; ++i)
            out[i - 1] = rd32(blob_.data() + off + 4 * std::size_t{i});
        return true;
    }

    bool blob(uint32_t v, std::span<const uint8_t>& out) const {
        uint32_t len_bytes = 0;
        switch (adb_type(v)) {
            case kAdbTypeBlob8: len_bytes = 1; break;
            case kAdbTypeBlob16: len_bytes = 2; break;
            case kAdbTypeBlob32: len_bytes = 4; break;
            default: return false;
        }
        const uint64_t off = adb_value(v);
        if (off + len_bytes > blob_.size()) return false;
        uint64_t len = 0;
        if (len_bytes == 1) len = blob_[off];
        else if (len_bytes == 2) len = rd16(blob_.data() + off);
        else len = rd32(blob_.data() + off);
        const uint64_t start = off + len_bytes;
        if (start + len > blob_.size()) return false;
        out = blob_.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(len));
        return true;
    }

    std::string str(uint32_t v) const {
        std::span<const uint8_t> b;
        return blob(v, b) ? to_string(b) : std::string();
    }

    uint64_t num(uint32_t v) const {
        switch (adb_type(v)) {
            case kAdbTypeInt: return adb_value(v);
            case kAdbTypeInt32: {
                const uint64_t off = adb_value(v);
                return off + 4 <= blob_.size() ? rd32(blob_.data() + off) : 0;
            }
            case kAdbTypeInt64: {
                const uint64_t off = adb_value(v);
                return off + 8 <= blob_.size() ? rd64(blob_.data() + off) : 0;
            }
            default: return 0;
        }
    }

private:
    std::span<const uint8_t> blob_;
};

// field(obj, n) with n 1-based; a missing field reads as ADB_NULL (0).
inline uint32_t field(const std::vector<uint32_t>& obj, std::size_t n) {
    return (n >= 1 && n <= obj.size()) ? obj[n - 1] : 0u;
}

// ---------------------------------------------------------------------------
// v3 adb stream -> blocks.  Fed with already-decompressed bytes so a large
// DATA section never has to be buffered.
// ---------------------------------------------------------------------------
class AdbStreamBuilder {
public:
    bool feed(const uint8_t* p, std::size_t n) {
        while (n > 0 && st_ != St::Done) {
            switch (st_) {
                case St::FileHeader: {
                    const std::size_t take = std::min(n, 8 - file_hdr_.size());
                    file_hdr_.insert(file_hdr_.end(), p, p + take);
                    p += take;
                    n -= take;
                    if (file_hdr_.size() == 8) {
                        if (rd32(file_hdr_.data()) != 0x2e424441u) {  // ADB_FORMAT_MAGIC
                            error_ = "v3: bad ADB file header magic";
                            return false;
                        }
                        schema_ = rd32(file_hdr_.data() + 4);
                        // keep the header: it is the head of the block payload
                        adb_payload_.assign(file_hdr_.begin(), file_hdr_.end());
                        file_hdr_.clear();
                        have_file_header_ = true;
                        st_ = St::BlockHeader;
                        hdr_.clear();
                        hdr_need_ = 4;
                    }
                    break;
                }
                case St::BlockHeader: {
                    const std::size_t take = std::min(n, hdr_need_ - hdr_.size());
                    hdr_.insert(hdr_.end(), p, p + take);
                    p += take;
                    n -= take;
                    if (hdr_.size() < hdr_need_) break;
                    if (!on_header()) return false;
                    break;
                }
                case St::Payload: {
                    const std::size_t take =
                        static_cast<std::size_t>(std::min<uint64_t>(n, payload_left_));
                    if (dst_ != nullptr) {
                        const uint64_t room = dst_cap_ > dst_->size() ? dst_cap_ - dst_->size() : 0;
                        const std::size_t keep = static_cast<std::size_t>(
                            std::min<uint64_t>(room, take));
                        dst_->insert(dst_->end(), p, p + keep);
                        if (keep < take) dst_truncated_ = true;
                    }
                    p += take;
                    n -= take;
                    payload_left_ -= take;
                    if (payload_left_ == 0) {
                        if (dst_truncated_ && !dst_note_.empty()) warnings_.push_back(dst_note_);
                        dst_truncated_ = false;
                        st_ = St::Padding;
                        pad_left_ = pad_;
                    }
                    break;
                }
                case St::Padding: {
                    const std::size_t take =
                        static_cast<std::size_t>(std::min<uint64_t>(n, pad_left_));
                    p += take;
                    n -= take;
                    pad_left_ -= take;
                    if (pad_left_ == 0) {
                        st_ = St::BlockHeader;
                        hdr_.clear();
                        hdr_need_ = 4;
                    }
                    break;
                }
                case St::Done:
                    break;
            }
        }
        return true;
    }

    bool finish(std::string& err) {
        if (!error_.empty()) {
            err = error_;
            return false;
        }
        if (!have_file_header_ || file_hdr_.size() != 0) {
            err = "v3: truncated adb stream (no ADB file header)";
            return false;
        }
        if (!have_adb_block_ || adb_payload_.empty()) {
            err = "v3: adb stream contains no ADB block";
            return false;
        }
        if (st_ == St::Payload || (st_ == St::Padding && pad_left_ != 0))
            warnings_.push_back("truncated trailing block in adb stream");
        return true;
    }

    const std::vector<uint8_t>& adb_payload() const { return adb_payload_; }
    uint32_t schema() const { return schema_; }
    std::vector<std::vector<uint8_t>> sig_blobs;
    uint64_t data_bytes = 0;  // bytes occupied by DATA blocks (incl. headers/pad)
    unsigned data_blocks = 0;
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    enum class St { FileHeader, BlockHeader, Payload, Padding, Done };

    bool on_header() {
        const uint32_t ts = rd32(hdr_.data());
        uint32_t type = ts >> 30;
        uint64_t rawsize = ts & 0x3fffffffu;
        uint32_t hdrsize = 4;
        if (type == 3) {  // ADB_BLOCK_EXT: need the rest of struct adb_block
            if (hdr_need_ == 4) {
                hdr_need_ = 16;
                return true;
            }
            type = ts & 0x3fffffffu;
            rawsize = rd64(hdr_.data() + 8);
            hdrsize = 16;
        }
        if (rawsize < hdrsize) {
            error_ = "v3: bad block size";
            return false;
        }
        if (type > 2) {  // ADB_BLOCK_MAX - 1; a DATAX block is illegal in v3
            error_ = "v3: unsupported ADB block type " + std::to_string(type);
            return false;
        }
        payload_left_ = rawsize - hdrsize;
        pad_ = round_up8(rawsize) - rawsize;
        dst_truncated_ = false;

        if (type == 0) {  // ADB
            if (have_adb_block_) {
                error_ = "v3: more than one ADB block";
                return false;
            }
            if (saw_data_) {
                error_ = "v3: ADB block after a DATA block";
                return false;
            }
            if (payload_left_ < 16 || payload_left_ > kMaxAdbBlockBytes) {
                error_ = "v3: ADB block size out of range";
                return false;
            }
            have_adb_block_ = true;
            dst_ = &adb_payload_;
            dst_cap_ = kMaxAdbBlockBytes + 8;
            dst_note_.clear();
        } else if (type == 1) {  // SIG
            if (!have_adb_block_ || saw_data_) {
                error_ = "v3: out-of-order SIG block";
                return false;
            }
            if (sig_blobs.size() >= 32) {
                warnings_.push_back("too many SIG blocks, ignoring the rest");
                dst_ = nullptr;
            } else if (payload_left_ > kMaxSigBlockBytes) {
                warnings_.push_back("oversized SIG block ignored");
                dst_ = nullptr;
            } else {
                sig_blobs.emplace_back();
                dst_ = &sig_blobs.back();
                dst_cap_ = kMaxSigBlockBytes;
                dst_note_ = "SIG block truncated";
            }
        } else {  // DATA
            if (!have_adb_block_) {
                error_ = "v3: DATA block before the ADB block";
                return false;
            }
            saw_data_ = true;
            dst_ = nullptr;
            data_bytes += round_up8(rawsize);
            ++data_blocks;
        }
        st_ = St::Payload;
        return true;
    }

    St st_ = St::FileHeader;
    std::vector<uint8_t> file_hdr_;
    std::vector<uint8_t> hdr_;
    std::size_t hdr_need_ = 4;
    uint64_t payload_left_ = 0;
    uint64_t pad_left_ = 0;
    uint64_t pad_ = 0;
    std::vector<uint8_t>* dst_ = nullptr;
    uint64_t dst_cap_ = 0;
    bool dst_truncated_ = false;
    std::string dst_note_;
    std::vector<uint8_t> adb_payload_;
    uint32_t schema_ = 0;
    bool have_file_header_ = false;
    bool have_adb_block_ = false;
    bool saw_data_ = false;
    std::string error_;
    std::vector<std::string> warnings_;
};

// ---------------------------------------------------------------------------
// zlib pumps
// ---------------------------------------------------------------------------
using SinkFn = std::function<bool(const uint8_t*, std::size_t)>;

// One raw deflate (window_bits -15) or gzip (15+16) stream.
bool inflate_pump(std::span<const uint8_t> in, int window_bits, const SinkFn& sink,
                  uint64_t max_out, std::string& err) {
    z_stream zs{};
    if (inflateInit2(&zs, window_bits) != Z_OK) {
        err = "zlib: inflateInit2 failed";
        return false;
    }
    std::vector<uint8_t> out(1u << 16);
    const uint8_t* cur = in.data();
    std::size_t rem = in.size();
    uint64_t total = 0;
    bool ok = true;
    for (;;) {
        const std::size_t feed = std::min<std::size_t>(rem, 0x7fffffffu);
        zs.next_in = const_cast<Bytef*>(cur);
        zs.avail_in = static_cast<uInt>(feed);
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        const int r = inflate(&zs, Z_NO_FLUSH);
        const std::size_t used = feed - static_cast<std::size_t>(zs.avail_in);
        const std::size_t produced = out.size() - zs.avail_out;
        cur += used;
        rem -= used;
        if (produced > 0) {
            total += produced;
            if (total > max_out) {
                err = "zlib: decoded data exceeds the 512 MiB limit";
                ok = false;
                break;
            }
            if (!sink(out.data(), produced)) break;  // consumer is satisfied
        }
        if (r == Z_STREAM_END) break;
        if (r != Z_OK) {
            err = std::string("zlib: ") + (zs.msg ? zs.msg : "corrupt deflate stream");
            ok = false;
            break;
        }
        if (used == 0 && produced == 0) {
            err = "zlib: truncated deflate stream";
            ok = false;
            break;
        }
    }
    inflateEnd(&zs);
    return ok;
}

// Concatenated gzip streams (v2).  on_data(stream, bytes) returning false stops
// the pump (a consumer-side failure, not a decode failure).  on_end(stream,
// compressed_size) reports the exact compressed length of that stream.
bool pump_gzip_streams(
    std::span<const uint8_t> in,
    const std::function<bool(std::size_t, const uint8_t*, std::size_t)>& on_data,
    const std::function<void(std::size_t, uint64_t)>& on_end, uint64_t max_out,
    std::string& err, bool& truncated) {
    truncated = false;
    if (in.size() < 2 || in[0] != 0x1f || in[1] != 0x8b) {
        err = "v2: not a gzip stream";
        return false;
    }
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {
        err = "zlib: inflateInit2 failed";
        return false;
    }
    std::vector<uint8_t> out(1u << 16);
    std::size_t rem = in.size();
    std::size_t stream_idx = 0;
    uint64_t stream_start = 0;
    uint64_t total = 0;
    bool ok = true;
    for (;;) {
        const std::size_t feed = std::min<std::size_t>(rem, 0x7fffffffu);
        const uint8_t* cur = in.data() + (in.size() - rem);
        zs.next_in = const_cast<Bytef*>(cur);
        zs.avail_in = static_cast<uInt>(feed);
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        const int r = inflate(&zs, Z_NO_FLUSH);
        const std::size_t produced = out.size() - zs.avail_out;
        const std::size_t used = feed - static_cast<std::size_t>(zs.avail_in);
        rem -= used;
        if (produced > 0) {
            total += produced;
            if (total > max_out) {
                err = "zlib: decoded data exceeds the 512 MiB limit";
                ok = false;
                break;
            }
            if (!on_data(stream_idx, out.data(), produced)) break;
        }
        if (r == Z_STREAM_END) {
            const uint64_t consumed = in.size() - rem;
            on_end(stream_idx, consumed - stream_start);
            stream_start = consumed;
            ++stream_idx;
            if (stream_idx > 4096) {  // untrusted input: bound the stream count
                err = "v2: too many gzip streams";
                ok = false;
                break;
            }
            if (rem < 2) break;
            const uint8_t* next = in.data() + (in.size() - rem);
            if (next[0] != 0x1f || next[1] != 0x8b) break;  // trailing data
            if (inflateReset(&zs) != Z_OK) {
                err = "zlib: inflateReset failed";
                ok = false;
                break;
            }
            continue;
        }
        if (r != Z_OK) {
            err = std::string("zlib: ") + (zs.msg ? zs.msg : "corrupt gzip stream");
            ok = false;
            break;
        }
        if (used == 0 && produced == 0) {
            truncated = true;
            break;
        }
    }
    inflateEnd(&zs);
    return ok;
}

#ifdef APKINFO_HAVE_ZSTD
bool pump_zstd(std::span<const uint8_t> in, const SinkFn& sink, uint64_t max_out,
               std::string& err) {
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (ds == nullptr) {
        err = "zstd: cannot create a decompression stream";
        return false;
    }
    ZSTD_initDStream(ds);
    std::vector<uint8_t> out(1u << 16);
    ZSTD_inBuffer ib{in.data(), in.size(), 0};
    uint64_t total = 0;
    bool ok = true;
    for (;;) {
        ZSTD_outBuffer ob{out.data(), out.size(), 0};
        const size_t r = ZSTD_decompressStream(ds, &ob, &ib);
        if (ZSTD_isError(r)) {
            err = std::string("zstd: ") + ZSTD_getErrorName(r);
            ok = false;
            break;
        }
        if (ob.pos > 0) {
            total += ob.pos;
            if (total > max_out) {
                err = "zstd: decoded data exceeds the 512 MiB limit";
                ok = false;
                break;
            }
            if (!sink(out.data(), ob.pos)) break;
        }
        if (r == 0) {
            if (ib.pos >= ib.size) break;  // frame finished and input exhausted
            ZSTD_initDStream(ds);          // concatenated frame
            continue;
        }
        if (ib.pos >= ib.size && ob.pos == 0) {
            err = "zstd: truncated frame";
            ok = false;
            break;
        }
    }
    ZSTD_freeDStream(ds);
    return ok;
}
#endif

// ---------------------------------------------------------------------------
// streaming tar walker (v2); file payloads are skipped, never buffered
// ---------------------------------------------------------------------------
class TarWalker {
public:
    struct Entry {
        std::string name;
        std::string linkname;
        std::string uname;
        std::string gname;
        uint64_t size = 0;
        uint64_t mtime = 0;
        uint32_t mode = 0;
        char typeflag = '0';
    };

    std::function<bool(const Entry&)> want_body;
    std::function<void(const Entry&, const uint8_t*, std::size_t)> on_body;
    std::function<void(const Entry&)> on_entry;
    std::size_t body_cap = kMaxScriptBytes;

    std::string error;
    bool saw_end = false;

    bool feed(const uint8_t* p, std::size_t n) {
        while (n > 0 && !failed_) {
            if (st_ == St::Done) return !failed_;
            if (st_ == St::Header) {
                const std::size_t take = std::min(n, kTarBlock - hdr_.size());
                hdr_.insert(hdr_.end(), p, p + take);
                p += take;
                n -= take;
                if (hdr_.size() == kTarBlock && !parse_header()) return false;
                continue;
            }
            if (st_ == St::Skip) {
                const std::size_t take = static_cast<std::size_t>(std::min<uint64_t>(n, skip_left_));
                p += take;
                n -= take;
                skip_left_ -= take;
                if (skip_left_ == 0) st_ = St::Header;
                continue;
            }
            // St::Body
            const std::size_t take = static_cast<std::size_t>(std::min<uint64_t>(n, body_left_));
            if (meta_active_) {
                if (meta_target_ != nullptr && meta_target_->size() < meta_cap_) {
                    const std::size_t room = meta_cap_ - meta_target_->size();
                    meta_target_->append(reinterpret_cast<const char*>(p), std::min(room, take));
                }
            } else if (on_body && body_kept_ < body_cap) {
                const std::size_t keep = static_cast<std::size_t>(
                    std::min<uint64_t>(take, body_cap - body_kept_));
                on_body(cur_entry_, p, keep);
                body_kept_ += keep;
            }
            p += take;
            n -= take;
            body_left_ -= take;
            if (body_left_ == 0) {
                body_kept_ = 0;
                meta_active_ = false;
                begin_skip(body_pad_);
            }
        }
        return !failed_;
    }

private:
    enum class St { Header, Skip, Body, Done };

    void begin_skip(uint64_t bytes) {
        if (bytes == 0) {
            st_ = St::Header;
            hdr_.clear();
        } else {
            skip_left_ = bytes;
            st_ = St::Skip;
        }
    }

    void begin_body(const Entry& e, bool meta) {
        cur_entry_ = e;
        meta_active_ = meta;
        body_left_ = e.size;
        body_kept_ = 0;
        body_pad_ = (kTarBlock - e.size % kTarBlock) % kTarBlock;
        if (body_left_ == 0) {
            meta_active_ = false;
            begin_skip(body_pad_);
        } else {
            st_ = St::Body;
        }
    }

    static uint64_t octal_field(const uint8_t* p, std::size_t len) {
        if (len > 0 && (p[0] & 0x80) != 0) {  // GNU base-256
            uint64_t v = p[0] & 0x7fu;
            for (std::size_t i = 1; i < len; ++i) v = (v << 8) | p[i];
            return v;
        }
        uint64_t v = 0;
        std::size_t i = 0;
        while (i < len && (p[i] == ' ' || p[i] == '0')) ++i;
        for (; i < len; ++i) {
            if (p[i] < '0' || p[i] > '7') break;
            v = v * 8 + static_cast<uint64_t>(p[i] - '0');
        }
        return v;
    }

    static std::string str_field(const uint8_t* p, std::size_t len) {
        std::size_t n = 0;
        while (n < len && p[n] != 0) ++n;
        return std::string(reinterpret_cast<const char*>(p), n);
    }

    static uint64_t digits_to_u64(std::string_view v, bool& ok) {
        uint64_t r = 0;
        ok = !v.empty();
        for (char c : v) {
            if (c < '0' || c > '9') {
                ok = false;
                break;
            }
            r = r * 10 + static_cast<uint64_t>(c - '0');
        }
        return r;
    }

    void apply_pax(std::string_view s, Entry& e) {
        // records: "<len> <key>=<value>\n"
        while (!s.empty()) {
            const std::size_t sp = s.find(' ');
            if (sp == std::string_view::npos) break;
            bool oklen = false;
            const uint64_t len = digits_to_u64(s.substr(0, sp), oklen);
            if (!oklen || len == 0 || len > s.size()) break;
            const std::string_view rec = s.substr(sp + 1, static_cast<std::size_t>(len) - sp - 1);
            const std::size_t eq = rec.find('=');
            if (eq != std::string_view::npos) {
                const std::string_view key = rec.substr(0, eq);
                std::string_view val = rec.substr(eq + 1);
                while (!val.empty() && (val.back() == '\n' || val.back() == '\0'))
                    val.remove_suffix(1);
                bool oknum = false;
                const uint64_t n = digits_to_u64(val, oknum);
                if (key == "path") e.name = std::string(val);
                else if (key == "linkpath") e.linkname = std::string(val);
                else if (key == "uname") e.uname = std::string(val);
                else if (key == "gname") e.gname = std::string(val);
                else if (key == "size" && oknum) e.size = n;
                else if (key == "mtime" && oknum) e.mtime = n;
            }
            if (static_cast<std::size_t>(len) >= s.size()) break;
            s.remove_prefix(static_cast<std::size_t>(len));
        }
    }

    bool parse_header() {
        const uint8_t* h = hdr_.data();
        bool all_zero = true;
        for (std::size_t i = 0; i < kTarBlock; ++i)
            if (h[i] != 0) {
                all_zero = false;
                break;
            }
        if (all_zero) {  // end-of-archive marker
            saw_end = true;
            hdr_.clear();
            st_ = St::Done;
            return true;
        }
        const uint32_t want = static_cast<uint32_t>(octal_field(h + 148, 8));
        uint32_t got = 0;
        for (std::size_t i = 0; i < kTarBlock; ++i)
            got += (i >= 148 && i < 156) ? 0x20u : static_cast<uint32_t>(h[i]);
        if (want != got) {
            failed_ = true;
            error = "tar: header checksum mismatch";
            return false;
        }

        Entry e;
        e.name = str_field(h, 100);
        e.mode = static_cast<uint32_t>(octal_field(h + 100, 8) & 07777u);
        e.size = octal_field(h + 124, 12);
        e.mtime = octal_field(h + 136, 12);
        e.typeflag = static_cast<char>(h[156]);
        e.linkname = str_field(h + 157, 100);
        e.uname = str_field(h + 265, 32);
        e.gname = str_field(h + 297, 32);
        if (std::memcmp(h + 257, "ustar", 5) == 0) {
            const std::string prefix = str_field(h + 345, 155);
            if (!prefix.empty()) e.name = prefix + "/" + e.name;
        }
        hdr_.clear();

        meta_target_ = nullptr;
        switch (e.typeflag) {
            case 'L':  // GNU long name for the next member
                meta_target_ = &long_name_;
                meta_cap_ = kMaxTarMetaBytes;
                long_name_.clear();
                begin_body(e, true);
                return true;
            case 'K':  // GNU long link name
                meta_target_ = &long_link_;
                meta_cap_ = kMaxTarMetaBytes;
                long_link_.clear();
                begin_body(e, true);
                return true;
            case 'x':
            case 'X':  // pax extended header for the next member
                meta_target_ = &pax_next_;
                meta_cap_ = kMaxTarMetaBytes;
                pax_next_.clear();
                begin_body(e, true);
                return true;
            case 'g':  // pax global header
                meta_target_ = &pax_global_;
                meta_cap_ = kMaxTarMetaBytes;
                begin_body(e, true);
                return true;
            default: break;
        }

        if (!long_name_.empty()) {
            e.name = long_name_;
            long_name_.clear();
        }
        if (!long_link_.empty()) {
            e.linkname = long_link_;
            long_link_.clear();
        }
        if (!pax_global_.empty()) apply_pax(pax_global_, e);
        if (!pax_next_.empty()) {
            apply_pax(pax_next_, e);
            pax_next_.clear();
        }

        if (on_entry) on_entry(e);
        const bool capture = want_body ? want_body(e) : false;
        if (capture)
            begin_body(e, false);
        else
            begin_skip(e.size + (kTarBlock - e.size % kTarBlock) % kTarBlock);
        return true;
    }

    St st_ = St::Header;
    bool failed_ = false;
    bool meta_active_ = false;
    std::vector<uint8_t> hdr_;
    uint64_t skip_left_ = 0;
    uint64_t body_left_ = 0;
    uint64_t body_pad_ = 0;
    std::size_t body_kept_ = 0;
    Entry cur_entry_;
    std::string long_name_;
    std::string long_link_;
    std::string pax_next_;
    std::string pax_global_;
    std::string* meta_target_ = nullptr;
    std::size_t meta_cap_ = 0;
};

// ---------------------------------------------------------------------------
// v2 script member names
// ---------------------------------------------------------------------------
const char* script_member_name(std::string_view name) {
    // Strip "./" and the mandatory leading dot, then match apk-tools'
    // apk_script_types[]; "<name>.trigger" members also map to "trigger".
    if (name.rfind("./", 0) == 0) name.remove_prefix(2);
    if (!name.empty() && name.front() == '.') name.remove_prefix(1);
    static const char* kNames[] = {"pre-install",  "post-install", "pre-deinstall",
                                  "post-deinstall", "pre-upgrade",  "post-upgrade",
                                  "trigger"};
    for (const char* n : kNames)
        if (name == n) return n;
    constexpr std::string_view kTrig = ".trigger";
    if (name.size() > kTrig.size() &&
        name.compare(name.size() - kTrig.size(), kTrig.size(), kTrig) == 0)
        return "trigger";
    return nullptr;
}

bool is_sign_member(std::string_view name) { return name.rfind(".SIGN.", 0) == 0; }

bool is_pkginfo_member(std::string_view name) {
    return name == ".PKGINFO" || name == "./.PKGINFO";
}

// ---------------------------------------------------------------------------
// .PKGINFO parsing
// ---------------------------------------------------------------------------
void add_deps_from_text(std::string_view value, std::vector<Dependency>& out,
                        std::vector<std::string>& warnings) {
    while (!value.empty()) {
        std::size_t b = 0;
        while (b < value.size() && (value[b] == ' ' || value[b] == '\t' || value[b] == '\n' ||
                                    value[b] == '\r' || value[b] == ','))
            ++b;
        value.remove_prefix(b);
        if (value.empty()) break;
        std::size_t e = value.find_first_of(" \t\n\r");
        const std::string_view tok = value.substr(0, e);
        value.remove_prefix(e == std::string_view::npos ? value.size() : e);

        Dependency dep;
        if (split_dep_text(tok, dep)) {
            out.push_back(std::move(dep));
        } else if (!tok.empty()) {
            dep = Dependency{};
            dep.name.assign(tok);
            out.push_back(std::move(dep));
            if (warnings.size() < 16)
                warnings.push_back("unparsable dependency: " + std::string(tok));
        }
    }
}

bool parse_uint(std::string_view s, uint64_t& out) {
    s = trim(s);
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        if (v > (UINT64_MAX - 9) / 10) return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v;
    return true;
}

void parse_pkginfo(std::string_view text, Package& pkg, std::vector<std::string>& warnings) {
    std::vector<std::string> unmapped;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line.front() == '#') continue;

        const std::size_t sep = line.find(" = ");
        if (sep == std::string_view::npos) {
            if (unmapped.size() < 24) unmapped.emplace_back(line);
            continue;
        }
        const std::string_view key = trim(line.substr(0, sep));
        const std::string_view value = trim(line.substr(sep + 3));
        const std::string vs(value);
        uint64_t num = 0;

        if (key == "pkgname") pkg.name = vs;
        else if (key == "pkgver") pkg.version = vs;
        else if (key == "pkgdesc") pkg.description = vs;
        else if (key == "url") pkg.url = vs;
        else if (key == "arch") pkg.arch = vs;
        else if (key == "license") pkg.license = vs;
        else if (key == "origin") pkg.origin = vs;
        else if (key == "maintainer") pkg.maintainer = vs;
        else if (key == "commit") pkg.commit = vs;
        else if (key == "datahash") pkg.hash = vs;
        else if (key == "size") {
            if (parse_uint(value, num)) pkg.installed_size = static_cast<int64_t>(num);
        } else if (key == "builddate") {
            if (parse_uint(value, num)) pkg.build_time = static_cast<int64_t>(num);
        } else if (key == "provider_priority") {
            if (parse_uint(value, num) && num <= 0xffffffffull)
                pkg.provider_priority = static_cast<uint32_t>(num);
        } else if (key == "depend") {
            add_deps_from_text(value, pkg.depends, warnings);
        } else if (key == "provides") {
            add_deps_from_text(value, pkg.provides, warnings);
        } else if (key == "replaces") {
            add_deps_from_text(value, pkg.replaces, warnings);
        } else if (key == "install_if") {
            add_deps_from_text(value, pkg.install_if, warnings);
        } else if (key == "recommends") {
            add_deps_from_text(value, pkg.recommends, warnings);
        } else if (key == "triggers") {
            // trigger paths have no home in Package: surface them as a note
            warnings.push_back("triggers (v2): " + vs);
        } else {
            if (unmapped.size() < 24)
                unmapped.push_back(vs.empty() ? std::string(key) : std::string(key) + "=" + vs);
        }
    }
    if (!unmapped.empty()) {
        std::string note = "unmapped .PKGINFO keys:";
        for (const auto& u : unmapped) {
            note += ' ';
            note += u;
        }
        warnings.push_back(note);
    }
}

// ---------------------------------------------------------------------------
// v2 whole-file parse
// ---------------------------------------------------------------------------
struct V2Stream {
    std::vector<FileEntry> files;
    std::string pkginfo;
    std::vector<std::pair<std::string, std::string>> scripts;  // name, body
    bool control = false;
    std::string tar_error;
};

Result parse_v2(std::span<const uint8_t> file) {
    Result res;
    Package& pkg = res.pkg;
    pkg.format = Format::V2;
    pkg.file_size = static_cast<int64_t>(file.size());

    std::vector<V2Stream> streams;
    // never reallocate: the per-stream walkers keep a pointer into this vector
    streams.reserve(4098);
    std::size_t active = std::size_t(-1);
    std::size_t total_entries = 0;  // global cap across all streams
    uint64_t data_stream_size = 0;
    TarWalker walker;
    std::vector<uint64_t> stream_sizes;

    auto make_walker_for = [&streams, &pkg, &total_entries](std::size_t idx) {
        if (streams.size() <= idx) streams.resize(idx + 1);
        V2Stream* s = &streams[idx];
        TarWalker w;
        w.want_body = [s](const TarWalker::Entry& e) {
            const std::string_view name(e.name);
            if (is_pkginfo_member(name)) {
                s->pkginfo.clear();
                return true;
            }
            if (is_sign_member(name)) return false;  // size only
            if (script_member_name(name) != nullptr) {
                s->scripts.emplace_back(std::string(), std::string());
                return true;
            }
            return false;
        };
        w.on_body = [s](const TarWalker::Entry& e, const uint8_t* p, std::size_t n) {
            const std::string_view name(e.name);
            if (is_pkginfo_member(name)) {
                if (s->pkginfo.size() < kMaxPkgInfoBytes)
                    s->pkginfo.append(reinterpret_cast<const char*>(p), n);
                return;
            }
            if (s->scripts.empty()) return;
            auto& back = s->scripts.back();
            if (back.first.empty()) back.first = script_member_name(name);
            if (back.second.size() < kMaxScriptBytes)
                back.second.append(reinterpret_cast<const char*>(p), n);
        };
        w.on_entry = [s, &pkg, &total_entries](const TarWalker::Entry& e) {
            const std::string_view name(e.name);
            if (is_sign_member(name)) {
                s->control = true;
                // .SIGN.<ALG>.<keyname...>
                std::string_view rest = name.substr(6);
                Signature sig;
                const std::size_t dot = rest.find('.');
                if (dot == std::string_view::npos) {
                    sig.algorithm = std::string(rest);
                } else {
                    sig.algorithm = std::string(rest.substr(0, dot));
                    sig.key_id = std::string(rest.substr(dot + 1));
                }
                sig.size = static_cast<uint32_t>(std::min<uint64_t>(e.size, 0xffffffffu));
                pkg.signatures.push_back(std::move(sig));
                return;
            }
            if (is_pkginfo_member(name) || script_member_name(name) != nullptr) {
                s->control = true;
                return;
            }
            if (e.typeflag == 'L' || e.typeflag == 'K' || e.typeflag == 'x' ||
                e.typeflag == 'X' || e.typeflag == 'g')
                return;
            if (total_entries >= kMaxFileEntries) return;
            ++total_entries;

            std::string_view p(name);
            while (p.rfind("./", 0) == 0) p.remove_prefix(2);
            if (p.empty() || p == ".") return;
            if (p.front() == '/') p.remove_prefix(1);

            FileEntry fe;
            fe.path = "/" + std::string(p);
            fe.mtime = e.mtime;
            fe.mode = e.mode;
            fe.size = (e.typeflag == '5') ? 0 : e.size;
            fe.user = e.uname;
            fe.group = e.gname;
            if (e.typeflag == '5') {
                fe.is_dir = true;
                if (fe.path.back() != '/') fe.path.push_back('/');
            } else if (e.typeflag == '2') {
                fe.is_symlink = true;
                fe.target = e.linkname;
            } else if (e.typeflag == '1' || e.typeflag == '3' || e.typeflag == '4') {
                fe.target = e.linkname;  // hard link / device node hint
            }
            s->files.push_back(std::move(fe));
        };
        return w;
    };

    walker = make_walker_for(0);
    active = 0;

    auto on_data = [&](std::size_t idx, const uint8_t* p, std::size_t n) {
        if (idx != active || active == std::size_t(-1)) {
            if (active != std::size_t(-1) && !walker.error.empty())
                streams[active].tar_error = walker.error;
            active = idx;
            walker = make_walker_for(idx);
        }
        return walker.feed(p, n);
    };
    auto on_end = [&](std::size_t idx, uint64_t size) {
        stream_sizes.push_back(size);
        if (active != std::size_t(-1) && !walker.error.empty())
            streams[active].tar_error = walker.error;
        // the compressed size of the last non-control (= data) stream is what
        // the UI shows as the payload size
        if (idx < streams.size() && !streams[idx].control) data_stream_size = size;
    };

    std::string zerr;
    bool truncated = false;
    const bool zok = pump_gzip_streams(file, on_data, on_end, kMaxDecodedBytes, zerr, truncated);
    if (active != std::size_t(-1) && !walker.error.empty())
        streams[active].tar_error = walker.error;

    bool have_pkginfo = false;
    for (const auto& s : streams) {
        if (!s.pkginfo.empty()) {
            have_pkginfo = true;
            parse_pkginfo(s.pkginfo, pkg, pkg.warnings);
        }
        for (const auto& sc : s.scripts) {
            if (sc.first.empty() || sc.second.empty()) continue;
            pkg.scripts.push_back(Script{sc.first, sc.second});
        }
        if (!s.control) {
            for (const auto& f : s.files) {
                if (pkg.files.size() >= kMaxFileEntries) {
                    pkg.warnings.push_back("file list truncated at 200000 entries");
                    break;
                }
                pkg.files.push_back(f);
            }
        }
        if (!s.tar_error.empty()) pkg.warnings.push_back(s.tar_error);
    }

    if (!have_pkginfo) {
        res.error = zok ? "v2: no .PKGINFO found (not an Alpine package)"
                        : (zerr + " (and no .PKGINFO found)");
        return res;
    }
    if (!zok) pkg.warnings.push_back("v2: " + zerr + " (metadata kept, data stream incomplete)");
    if (truncated) pkg.warnings.push_back("v2: truncated gzip stream");
    if (pkg.name.empty()) {
        res.error = "v2: .PKGINFO has no pkgname";
        return res;
    }
    if (data_stream_size != 0)
        pkg.data_section_size = data_stream_size;
    else if (!stream_sizes.empty())
        pkg.data_section_size = stream_sizes.back();

    std::sort(pkg.files.begin(), pkg.files.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
    res.ok = true;
    return res;
}

// ---------------------------------------------------------------------------
// v3 package tree -> Package
// ---------------------------------------------------------------------------
void fill_deps(const AdbReader& db, uint32_t arr_val, std::vector<Dependency>& out) {
    std::vector<uint32_t> arr;
    if (!db.obj(arr_val, arr)) return;
    out.reserve(arr.size());
    for (uint32_t dep_val : arr) {
        std::vector<uint32_t> d;
        if (!db.obj(dep_val, d)) continue;
        Dependency dep;
        dep.name = db.str(field(d, 1));
        dep.version = db.str(field(d, 2));
        if (dep.name.empty()) continue;
        const uint64_t mask = db.num(field(d, 3));
        if (dep.version.empty()) {
            if ((mask & 16u) != 0) dep.op = "!";
        } else {
            dep.op = op_from_mask(mask == 0 ? 1u : mask);  // src/package.c: op ||= EQUAL
        }
        out.push_back(std::move(dep));
    }
}

void fill_string_array(const AdbReader& db, uint32_t arr_val, std::vector<std::string>& out) {
    std::vector<uint32_t> arr;
    if (!db.obj(arr_val, arr)) return;
    out.reserve(arr.size());
    for (uint32_t v : arr) {
        std::string s = db.str(v);
        if (!s.empty()) out.push_back(std::move(s));
    }
}

void fill_scripts(const AdbReader& db, uint32_t scripts_val, Package& pkg) {
    static const std::pair<unsigned, const char*> kNames[] = {
        {1, "trigger"},       {2, "pre-install"},    {3, "post-install"}, {4, "pre-deinstall"},
        {5, "post-deinstall"}, {6, "pre-upgrade"},   {7, "post-upgrade"},
    };
    std::vector<uint32_t> obj;
    if (!db.obj(scripts_val, obj)) return;
    for (const auto& entry : kNames) {
        if (entry.first > obj.size()) break;
        std::string body = db.str(field(obj, entry.first));
        if (body.empty()) continue;
        pkg.scripts.push_back(Script{entry.second, std::move(body)});
    }
}

// v3 file target blob: uint16le mode, then either a path (S_IFLNK/S_IFREG) or a
// uint64le device number (src/extract_v3.c apk_extract_v3_file()).
void apply_target(std::span<const uint8_t> target, FileEntry& fe,
                  std::vector<std::string>& warnings) {
    if (target.size() < 2) return;
    const uint16_t mode = rd16(target.data());
    const std::span<const uint8_t> rest = target.subspan(2);
    switch (mode) {
        case 0xa000:  // S_IFLNK
            fe.is_symlink = true;
            fe.target = to_string(rest);
            break;
        case 0x8000:  // S_IFREG (hard link style entry)
            fe.target = to_string(rest);
            break;
        case 0x2000:  // S_IFCHR
        case 0x6000:  // S_IFBLK
        case 0x1000:  // S_IFIFO
            if (rest.size() >= 8) fe.target = "device " + std::to_string(rd64(rest.data()));
            else warnings.push_back("short device metadata for " + fe.path);
            break;
        default:
            warnings.push_back("unknown file target mode " + std::to_string(mode) + " for " +
                               fe.path);
            break;
    }
}

// Rebuild "/dir/file" the way apk_pathbuilder does: the directory name, then
// '/' and the file name.  Packages store directory names without a leading
// slash, so it is added here (FileEntry::path is documented as absolute).
std::string join_path(std::string_view dir, std::string_view file) {
    std::string p;
    p.reserve(dir.size() + file.size() + 2);
    if (!dir.empty() && dir.front() != '/') p.push_back('/');
    p.append(dir);
    if (!file.empty()) {
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p.append(file);
    }
    return p;
}

Result build_v3(AdbStreamBuilder& stream, int64_t file_size) {
    Result res;
    Package& pkg = res.pkg;
    pkg.format = Format::V3;
    pkg.file_size = file_size;
    pkg.data_section_size = stream.data_bytes;

    std::string perr;
    if (!stream.finish(perr)) {
        res.error = perr;
        return res;
    }
    for (const auto& w : stream.warnings()) pkg.warnings.push_back(w);

    const std::vector<uint8_t>& payload = stream.adb_payload();
    if (payload.size() < 16 || rd32(payload.data()) != 0x2e424441u) {
        res.error = "v3: malformed ADB block payload";
        return res;
    }
    if (rd32(payload.data() + 4) != 0x676b6370u) {  // "pckg"
        res.error = "v3: ADB schema is not a package";
        return res;
    }
    // The adb blob is everything after the 8-byte file header; its offsets are
    // relative to the start of struct adb_hdr, which is the blob start.
    const std::span<const uint8_t> blob(payload.data() + 8, payload.size() - 8);
    if (blob.size() < 8) {
        res.error = "v3: truncated adb header";
        return res;
    }
    const uint8_t compat_ver = blob[0];
    const uint8_t adb_ver = blob[1];
    if (compat_ver != 0) {
        res.error = "v3: unsupported adb compat version " + std::to_string(compat_ver);
        return res;
    }
    if (adb_ver != 0) pkg.warnings.push_back("adb version " + std::to_string(adb_ver));
    const uint32_t root = rd32(blob.data() + 4);

    AdbReader db(blob);
    std::vector<uint32_t> pkgobj;
    if (!db.obj(root, pkgobj)) {
        res.error = "v3: root object is not an object";
        return res;
    }

    // --- pkginfo (ADBI_PKG_PKGINFO) ---------------------------------------
    std::vector<uint32_t> info;
    if (!db.obj(field(pkgobj, 1), info)) {
        res.error = "v3: package object has no pkginfo";
        return res;
    }
    pkg.name = db.str(field(info, 1));         // ADBI_PI_NAME
    pkg.version = db.str(field(info, 2));      // ADBI_PI_VERSION
    {
        std::span<const uint8_t> h;
        if (db.blob(field(info, 3), h)) pkg.hash = hex_of(h);  // ADBI_PI_HASHES (sha1/sha256)
    }
    pkg.description = db.str(field(info, 4));
    pkg.arch = db.str(field(info, 5));
    pkg.license = db.str(field(info, 6));
    pkg.origin = db.str(field(info, 7));
    pkg.maintainer = db.str(field(info, 8));
    pkg.url = db.str(field(info, 9));
    {
        std::span<const uint8_t> h;
        if (db.blob(field(info, 10), h)) pkg.commit = hex_of(h);  // repo-commit hexblob
    }
    pkg.build_time = static_cast<int64_t>(db.num(field(info, 11)));
    pkg.installed_size = static_cast<int64_t>(db.num(field(info, 12)));
    {
        const uint64_t fs = db.num(field(info, 13));
        if (file_size > 0) {
            if (fs != 0 && static_cast<int64_t>(fs) != file_size)
                pkg.warnings.push_back("pkginfo file-size " + std::to_string(fs) +
                                       " differs from the real file size " +
                                       std::to_string(file_size));
        } else if (fs <= static_cast<uint64_t>(INT64_MAX)) {
            pkg.file_size = static_cast<int64_t>(fs);
        }
    }
    pkg.provider_priority = static_cast<uint32_t>(db.num(field(info, 14)));
    fill_deps(db, field(info, 15), pkg.depends);      // ADBI_PI_DEPENDS
    fill_deps(db, field(info, 16), pkg.provides);     // ADBI_PI_PROVIDES
    fill_deps(db, field(info, 17), pkg.replaces);     // ADBI_PI_REPLACES
    fill_deps(db, field(info, 18), pkg.install_if);   // ADBI_PI_INSTALL_IF
    fill_deps(db, field(info, 19), pkg.recommends);   // ADBI_PI_RECOMMENDS
    fill_string_array(db, field(info, 21), pkg.tags); // ADBI_PI_TAGS
    if (db.num(field(info, 20)) != 0)                 // ADBI_PI_LAYER
        pkg.warnings.push_back("pkginfo layer = " + std::to_string(db.num(field(info, 20))));
    if (pkg.name.empty()) {
        res.error = "v3: pkginfo has no name";
        return res;
    }

    // --- scripts (ADBI_PKG_SCRIPTS) ---------------------------------------
    fill_scripts(db, field(pkgobj, 3), pkg);

    // --- triggers have no field in Package: report them --------------------
    {
        std::vector<uint32_t> trig;
        if (db.obj(field(pkgobj, 4), trig) && !trig.empty()) {
            std::string note = "triggers (v3):";
            std::size_t shown = 0;
            for (uint32_t v : trig) {
                if (shown++ >= 8) {
                    note += " ...";
                    break;
                }
                note += ' ';
                note += db.str(v);
            }
            pkg.warnings.push_back(note);
        }
    }
    if (db.num(field(pkgobj, 5)) != 0)
        pkg.warnings.push_back("replaces-priority = " + std::to_string(db.num(field(pkgobj, 5))));

    // --- file index (ADBI_PKG_PATHS -> dir -> file) -----------------------
    std::vector<uint32_t> paths;
    if (!db.obj(field(pkgobj, 2), paths)) {
        pkg.warnings.push_back("v3: package has no paths array");
    } else {
        bool truncated = false;
        for (std::size_t pi = 0; pi < paths.size() && !truncated; ++pi) {
            std::vector<uint32_t> dir;
            if (!db.obj(paths[pi], dir)) continue;
            const std::string dir_name = db.str(field(dir, 1));  // ADBI_DI_NAME

            // the directory itself; an empty name is the package root, which is
            // not part of a file listing
            if (!dir_name.empty() && pkg.files.size() < kMaxFileEntries) {
                FileEntry fe;
                fe.path = join_path(dir_name, std::string_view());
                if (fe.path.empty() || fe.path.back() != '/') fe.path.push_back('/');
                fe.is_dir = true;
                std::vector<uint32_t> acl;
                if (db.obj(field(dir, 2), acl)) {  // ADBI_DI_ACL
                    fe.mode = static_cast<uint32_t>(db.num(field(acl, 1)) & 07777u);
                    fe.user = db.str(field(acl, 2));
                    fe.group = db.str(field(acl, 3));
                }
                pkg.files.push_back(std::move(fe));
            }

            std::vector<uint32_t> files;
            if (!db.obj(field(dir, 3), files)) continue;  // ADBI_DI_FILES
            for (std::size_t fi = 0; fi < files.size(); ++fi) {
                if (pkg.files.size() >= kMaxFileEntries) {
                    truncated = true;
                    pkg.warnings.push_back("file list truncated at 200000 entries");
                    break;
                }
                std::vector<uint32_t> f;
                if (!db.obj(files[fi], f)) continue;
                const std::string fname = db.str(field(f, 1));  // ADBI_FI_NAME
                if (fname.empty()) continue;

                FileEntry fe;
                fe.path = join_path(dir_name, fname);
                fe.size = db.num(field(f, 3));   // ADBI_FI_SIZE
                fe.mtime = db.num(field(f, 4));  // ADBI_FI_MTIME
                std::span<const uint8_t> h;
                if (db.blob(field(f, 5), h)) fe.sha1 = hex_of(h);  // ADBI_FI_HASHES
                std::vector<uint32_t> acl;
                if (db.obj(field(f, 2), acl)) {  // ADBI_FI_ACL
                    fe.mode = static_cast<uint32_t>(db.num(field(acl, 1)) & 07777u);
                    fe.user = db.str(field(acl, 2));
                    fe.group = db.str(field(acl, 3));
                }
                std::span<const uint8_t> target;
                if (db.blob(field(f, 6), target)) apply_target(target, fe, pkg.warnings);
                pkg.files.push_back(std::move(fe));
            }
        }
    }

    // --- signatures (SIG blocks) ------------------------------------------
    // struct adb_sign_v0 { uint8 sign_ver, hash_alg; uint8 id[16]; uint8 sig[]; }
    for (const auto& sig : stream.sig_blobs) {
        if (sig.size() < 2) {
            pkg.warnings.push_back("v3: short SIG block ignored");
            continue;
        }
        const uint8_t sign_ver = sig[0];
        const uint8_t hash_alg = sig[1];
        Signature s;
        s.algorithm = "RSA";
        s.size = static_cast<uint32_t>(sig.size());
        if (sign_ver == 0 && sig.size() >= 18) {
            s.key_id = hex_of(std::span<const uint8_t>(sig.data() + 2, 16));
        } else {
            pkg.warnings.push_back("v3: unknown signature version " +
                                   std::to_string(sign_ver));
        }
        if (hash_alg != 2 && hash_alg != 3 && hash_alg != 4)
            pkg.warnings.push_back("v3: unexpected signature digest id " +
                                   std::to_string(hash_alg));
        pkg.signatures.push_back(std::move(s));
    }

    // --- integrity hint: the DATA blocks must hold at least the file payload
    // (each DATA payload is an 8-byte struct adb_data_package plus file bytes)
    {
        uint64_t payload_bytes = 0;
        for (const auto& f : pkg.files)
            if (!f.is_dir && !f.is_symlink && f.size != 0) payload_bytes += f.size;
        if (stream.data_blocks > 0 && pkg.data_section_size < payload_bytes)
            pkg.warnings.push_back("DATA section (" + std::to_string(pkg.data_section_size) +
                                   " bytes) is smaller than the file payload (" +
                                   std::to_string(payload_bytes) + " bytes); package truncated?");
    }

    std::sort(pkg.files.begin(), pkg.files.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
    res.ok = true;
    return res;
}

// ---------------------------------------------------------------------------
// v3 container -> decompressed adb stream -> build_v3
// ---------------------------------------------------------------------------
Result parse_v3(std::span<const uint8_t> file) {
    Result res;
    if (file.size() < 4 || std::memcmp(file.data(), "ADB", 3) != 0) {
        res.error = "v3: missing ADB container magic";
        return res;
    }
    enum class Comp { None, Deflate, Zstd };
    Comp comp = Comp::None;
    std::span<const uint8_t> stream;
    switch (file[3]) {
        case '.':
            stream = file;  // no compression: the adb stream starts at offset 0
            break;
        case 'd':
            comp = Comp::Deflate;
            stream = file.subspan(4);
            break;
        case 'z':
            comp = Comp::Zstd;
            stream = file.subspan(4);
            break;
        case 'n':
            stream = file.subspan(4);
            break;
        case 'c': {
            if (file.size() < 6) {
                res.error = "v3: truncated compression spec";
                return res;
            }
            switch (file[4]) {
                case 0: stream = file.subspan(6); break;
                case 1: comp = Comp::Deflate; stream = file.subspan(6); break;
                case 2: comp = Comp::Zstd; stream = file.subspan(6); break;
                default:
                    res.error = "v3: unknown compression algorithm " +
                                std::to_string(static_cast<int>(file[4]));
                    return res;
            }
            break;
        }
        default:
            res.error = "v3: unknown compression marker";
            return res;
    }

    AdbStreamBuilder builder;
    const SinkFn sink = [&builder](const uint8_t* p, std::size_t n) { return builder.feed(p, n); };
    std::string perr;

    if (comp == Comp::None) {
        builder.feed(stream.data(), stream.size());
    } else if (comp == Comp::Deflate) {
        if (!inflate_pump(stream, -15, sink, kMaxDecodedBytes, perr)) {
            std::string ferr;
            if (!builder.finish(ferr)) {
                res.error = perr;
                return res;
            }
            res.pkg.warnings.push_back(perr);
        }
    } else {
#ifdef APKINFO_HAVE_ZSTD
        if (!pump_zstd(stream, sink, kMaxDecodedBytes, perr)) {
            res.error = perr;
            return res;
        }
#else
        res.error =
            "v3: zstd-compressed package, but this build has no APKINFO_HAVE_ZSTD support";
        return res;
#endif
    }
    return build_v3(builder, static_cast<int64_t>(file.size()));
}

}  // namespace

// ---------------------------------------------------------------------------
// public interface
// ---------------------------------------------------------------------------
const char* format_name(Format f) {
    switch (f) {
        case Format::V2: return "v2";
        case Format::V3: return "v3";
        default: return "unknown";
    }
}

std::string Dependency::to_string() const {
    std::string out;
    std::string_view rest(op);
    if (!rest.empty() && rest.front() == '!') {
        // "!name>=ver": the conflict marker goes in front of the name, exactly
        // as apk_blob_push_dep() prints it.
        out.push_back('!');
        rest.remove_prefix(1);
    }
    out += name;
    out.append(rest);
    out += version;
    return out;
}

Result parse(std::span<const uint8_t> file) {
    Result res;
    try {
        if (file.size() >= 4 && std::memcmp(file.data(), "ADB", 3) == 0) return parse_v3(file);
        if (file.size() >= 2 && file[0] == 0x1f && file[1] == 0x8b) return parse_v2(file);
        res.error = "not an Alpine package (neither an ADB container nor a gzip stream)";
        return res;
    } catch (const std::bad_alloc&) {
        res = Result{};
        res.error = "out of memory while parsing";
        return res;
    } catch (...) {
        res = Result{};
        res.error = "internal error while parsing";
        return res;
    }
}

Result parse_v3_adb(std::span<const uint8_t> adb_stream) {
    Result res;
    try {
        AdbStreamBuilder builder;
        builder.feed(adb_stream.data(), adb_stream.size());
        return build_v3(builder, 0);
    } catch (const std::bad_alloc&) {
        res = Result{};
        res.error = "out of memory while parsing";
        return res;
    } catch (...) {
        res = Result{};
        res.error = "internal error while parsing";
        return res;
    }
}

bool looks_like_alpine(std::span<const uint8_t> head) {
    if (head.size() >= 4 && std::memcmp(head.data(), "ADB", 3) == 0) return true;
    // v2: a gzip stream.  .PKGINFO cannot be checked from a header alone; an
    // Android .apk is a zip ("PK") and never starts with 1f 8b.
    if (head.size() >= 2 && head[0] == 0x1f && head[1] == 0x8b) return true;
    return false;
}

}  // namespace apkinfo::alpine
