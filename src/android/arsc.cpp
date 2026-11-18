// resources.arsc (Android resource table) parser -- implementation.
//
// Plain C++20, no third party code, no platform headers. Every offset, count
// and pool index read from the buffer is treated as untrusted: reads are
// bounded (the low level accessors return 0 for out of range addresses and
// every allocation is validated against the remaining chunk space), so a
// truncated or hostile file degrades to "resource not found" instead of
// crashing. parse() never throws.
#include "arsc.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace apkinfo::arsc {
namespace {

// ---------------------------------------------------------------------------
// Chunk types / flags (androidfw ResourceTypes.h)
// ---------------------------------------------------------------------------
constexpr uint16_t kChunkStringPool = 0x0001;
constexpr uint16_t kChunkTable = 0x0002;
constexpr uint16_t kChunkTablePackage = 0x0200;
constexpr uint16_t kChunkTableType = 0x0201;
constexpr uint16_t kChunkTableTypeSpec = 0x0202;

constexpr uint16_t kEntryFlagComplex = 0x0001;
constexpr uint16_t kEntryFlagPublic = 0x0002;
constexpr uint16_t kEntryFlagWeak = 0x0004;
constexpr uint16_t kEntryFlagCompact = 0x0008;

constexpr uint8_t kTypeFlagSparse = 0x01;
constexpr uint8_t kTypeFlagOffset16 = 0x02;

constexpr uint32_t kNoEntry32 = 0xFFFFFFFFu;
constexpr uint16_t kNoEntry16 = 0xFFFFu;

// Hard caps so a hostile file cannot ask for gigabytes.
constexpr uint32_t kMaxEntryCount = 5'000'000u;
constexpr uint32_t kMaxStringCount = 4'000'000u;
constexpr uint32_t kMaxBagCount = 200'000u;
constexpr int kMaxReferenceDepth = 16;

// ResTable_config byte offsets (see androidfw ResourceTypes.h).
constexpr uint32_t kCfgSize = 0;
constexpr uint32_t kCfgMcc = 4;
constexpr uint32_t kCfgMnc = 6;
constexpr uint32_t kCfgLanguage = 8;
constexpr uint32_t kCfgCountry = 10;
constexpr uint32_t kCfgOrientation = 12;
constexpr uint32_t kCfgDensity = 14;
constexpr uint32_t kCfgSdkVersion = 24;
constexpr uint32_t kCfgScreenLayout = 28;
constexpr uint32_t kCfgUiMode = 29;
constexpr uint32_t kCfgSmallestWidthDp = 30;
constexpr uint32_t kCfgGrammaticalInflection = 19;  // shares the input-pad byte
constexpr uint32_t kCfgWidthDp = 32;
constexpr uint32_t kCfgHeightDp = 34;

// ---------------------------------------------------------------------------
// Bounded little-endian reader.
// ---------------------------------------------------------------------------
struct Buf {
    std::span<const uint8_t> d;

    bool ok(size_t off, size_t len) const {
        return off <= d.size() && len <= d.size() - off;
    }
    uint8_t u8(size_t off) const { return ok(off, 1) ? d[off] : 0; }
    uint16_t u16(size_t off) const {
        if (!ok(off, 2)) return 0;
        return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
    }
    uint32_t u32(size_t off) const {
        if (!ok(off, 4)) return 0;
        return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
               (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
    }
    // Chunk header field readers that also bound the chunk itself.
    uint16_t type(size_t off) const { return u16(off); }
    uint16_t header_size(size_t off) const { return u16(off + 2); }
    uint32_t chunk_size(size_t off) const { return u32(off + 4); }
};

std::string num(uint32_t v) { return std::to_string(v); }

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Some builders store non-BMP characters in a UTF-8 pool as two separately
// encoded UTF-16 surrogates (CESU-8, e.g. "ed a0 bc ed be 89" instead of
// "f0 9f 8e 89"). That is invalid UTF-8, so decode and re-encode properly;
// malformed bytes are copied through unchanged rather than lost.
std::string normalize_utf8(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        const uint8_t b0 = p[i];
        if (b0 < 0x80) {
            out.push_back(static_cast<char>(b0));
            ++i;
            continue;
        }
        uint32_t cp = 0;
        size_t len = 0;
        if ((b0 & 0xE0) == 0xC0) {
            len = 2;
            cp = b0 & 0x1F;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3;
            cp = b0 & 0x0F;
        } else if ((b0 & 0xF8) == 0xF0) {
            len = 4;
            cp = b0 & 0x07;
        } else {
            out.push_back(static_cast<char>(b0));
            ++i;
            continue;
        }
        if (i + len > n) {
            out.push_back(static_cast<char>(b0));
            ++i;
            continue;
        }
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            const uint8_t c = p[i + k];
            if ((c & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok) {
            out.push_back(static_cast<char>(b0));
            ++i;
            continue;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= n && (p[i + 3] & 0xF0) == 0xE0) {
            uint32_t lo = p[i + 3] & 0x0F;
            bool ok2 = true;
            for (size_t k = 4; k < 6; ++k) {
                const uint8_t c = p[i + k];
                if ((c & 0xC0) != 0x80) {
                    ok2 = false;
                    break;
                }
                lo = (lo << 6) | (c & 0x3F);
            }
            if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                append_utf8(out, 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00));
                i += 6;
                continue;
            }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {  // lone surrogate: drop
            i += len;
            continue;
        }
        append_utf8(out, cp);
        i += len;
    }
    return out;
}

// UTF-16LE -> UTF-8, lone surrogates passed through as U+FFFD-ish (we simply
// skip them so the result stays valid UTF-8).
std::string utf16le_to_utf8(const uint8_t* p, size_t units) {
    std::string out;
    out.reserve(units);
    size_t i = 0;
    while (i < units) {
        uint32_t c = static_cast<uint32_t>(p[i * 2]) | (static_cast<uint32_t>(p[i * 2 + 1]) << 8);
        ++i;
        if (c >= 0xD800 && c <= 0xDBFF && i < units) {
            uint32_t c2 = static_cast<uint32_t>(p[i * 2]) | (static_cast<uint32_t>(p[i * 2 + 1]) << 8);
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                ++i;
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
            } else {
                continue;  // lone high surrogate
            }
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            continue;  // lone surrogate
        }
        append_utf8(out, c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// ResTable_config language/country field decoding.
//
// A two character qualifier is stored verbatim ('e','n'). A three character
// one (locale or UN M49 numeric region) is packed into 2 bytes with the high
// bit of the first byte as a marker:
//   in[0] = 0x80 | (c2 << 2) | (c1 >> 3)
//   in[1] = ((c1 & 7) << 5) | c0
// with 'a' as base for languages and '0' for numeric regions.
// ---------------------------------------------------------------------------
std::string decode_lang_or_region(uint8_t b0, uint8_t b1, char base) {
    if (b0 == 0 && b1 == 0) return {};
    if ((b0 & 0x80) != 0) {
        char out[4];
        out[0] = static_cast<char>(base + (b1 & 0x1F));
        out[1] = static_cast<char>(base + ((b1 & 0xE0) >> 5) + ((b0 & 0x03) << 3));
        out[2] = static_cast<char>(base + ((b0 & 0x7C) >> 2));
        out[3] = '\0';
        for (int i = 0; i < 3; ++i) {
            // Only keep it when it decodes to something printable; the odd
            // /reserved encodings become "" instead of junk.
            if (static_cast<unsigned char>(out[i]) < 0x20 || static_cast<unsigned char>(out[i]) > 0x7E)
                return {};
        }
        return std::string(out, 3);
    }
    if (b0 < 0x20 || b0 > 0x7E || b1 < 0x20 || b1 > 0x7E) return {};
    std::string s;
    s.push_back(static_cast<char>(b0));
    s.push_back(static_cast<char>(b1));
    return s;
}

// ---------------------------------------------------------------------------
// String pools
// ---------------------------------------------------------------------------
struct Pool {
    bool valid = false;
    bool utf8 = false;
    uint32_t count = 0;
    size_t offsets_at = 0;  // absolute offset of the uint32 offset table
    size_t strings_at = 0;  // absolute offset of the string data
    size_t end = 0;         // absolute end of the chunk
};

Pool parse_pool(const Buf& b, size_t off) {
    Pool p;
    if (!b.ok(off, 28)) return p;
    if (b.u16(off) != kChunkStringPool) return p;
    const uint16_t header_size = b.u16(off + 2);
    const uint32_t size = b.u32(off + 4);
    if (header_size < 28 || size < header_size) return p;

    const size_t end = std::min<size_t>(b.d.size(), off + size);
    uint32_t count = b.u32(off + 8);
    const uint32_t flags = b.u32(off + 16);
    const uint32_t strings_start = b.u32(off + 20);
    if (count > kMaxStringCount) return p;

    const size_t offsets_at = off + header_size;
    if (offsets_at > end) return p;
    const size_t room = (end - offsets_at) / 4;
    if (count > room) count = static_cast<uint32_t>(room);  // truncated: keep what fits

    const size_t strings_at = off + strings_start;
    if (count > 0 && strings_at >= end) return p;

    p.valid = true;
    p.utf8 = (flags & 0x100u) != 0;
    p.count = count;
    p.offsets_at = offsets_at;
    p.strings_at = strings_at;
    p.end = end;
    return p;
}

// Decodes a (1 or 2 byte) length prefix; UTF-8 pools store two of them.
bool read_pool_len(const Buf& b, const Pool& p, size_t& pos, uint32_t& out) {
    if (pos >= p.end) return false;
    uint8_t v = b.u8(pos++);
    if ((v & 0x80) != 0) {
        if (pos >= p.end) return false;
        uint8_t v2 = b.u8(pos++);
        out = (static_cast<uint32_t>(v & 0x7F) << 8) | v2;
    } else {
        out = v;
    }
    return true;
}

std::optional<std::string> pool_string(const Buf& b, const Pool& p, uint32_t index) {
    if (!p.valid || index >= p.count) return std::nullopt;
    const size_t ent = p.offsets_at + static_cast<size_t>(index) * 4;
    if (!b.ok(ent, 4)) return std::nullopt;
    const uint32_t rel = b.u32(ent);
    if (rel == kNoEntry32) return std::nullopt;
    size_t pos = p.strings_at + rel;
    if (pos >= p.end) return std::nullopt;

    if (p.utf8) {
        uint32_t len16 = 0;
        uint32_t len8 = 0;
        if (!read_pool_len(b, p, pos, len16)) return std::nullopt;
        if (!read_pool_len(b, p, pos, len8)) return std::nullopt;
        size_t avail = p.end - pos;
        if (len8 > avail) len8 = static_cast<uint32_t>(avail);
        size_t used = 0;
        while (used < len8 && b.u8(pos + used) != 0) ++used;
        return normalize_utf8(b.d.data() + pos, used);
    }

    // UTF-16 pools store the length as a plain uint16 (the one/two byte varint
    // scheme is UTF-8 only).
    if (!b.ok(pos, 2)) return std::nullopt;
    uint32_t units = b.u16(pos);
    pos += 2;
    const size_t avail_units = (p.end - pos) / 2;
    if (units > avail_units) units = static_cast<uint32_t>(avail_units);
    return utf16le_to_utf8(b.d.data() + pos, units);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
Config parse_config(const Buf& b, size_t off, size_t limit) {
    Config c;
    if (off >= limit) return c;
    uint32_t size = b.u32(off + kCfgSize);
    const size_t room = limit - off;
    if (size > room) size = static_cast<uint32_t>(room);
    auto has = [size](uint32_t field_off, uint32_t field_len) {
        return static_cast<uint64_t>(field_off) + field_len <= size;
    };

    if (has(kCfgMcc, 2)) c.mcc = b.u16(off + kCfgMcc);
    if (has(kCfgMnc, 2)) c.mnc = b.u16(off + kCfgMnc);
    if (has(kCfgLanguage, 2)) c.language = decode_lang_or_region(b.u8(off + kCfgLanguage), b.u8(off + kCfgLanguage + 1), 'a');
    if (has(kCfgCountry, 2)) c.region = decode_lang_or_region(b.u8(off + kCfgCountry), b.u8(off + kCfgCountry + 1), '0');
    if (has(kCfgOrientation, 1)) c.orientation = b.u8(off + kCfgOrientation);
    // Android 15+: the grammaticalInflection byte reuses the input padding byte.
    // 1 = neuter, 2 = feminine, 3 = masculine (GRAMMATICAL_INFLECTION_GENDER_MASK).
    if (has(kCfgGrammaticalInflection, 1))
        c.grammar_gender = static_cast<uint8_t>(b.u8(off + kCfgGrammaticalInflection) & 0x03);
    if (has(kCfgDensity, 2)) c.density = b.u16(off + kCfgDensity);
    if (has(kCfgSdkVersion, 2)) c.sdk_version = b.u16(off + kCfgSdkVersion);
    if (has(kCfgUiMode, 1)) {
        const uint8_t ui_mode = b.u8(off + kCfgUiMode);
        c.ui_mode_type = static_cast<uint8_t>(ui_mode & 0x0F);
        c.night_mode = static_cast<uint8_t>((ui_mode & 0x30) >> 4);
    }
    if (has(kCfgSmallestWidthDp, 2)) c.smallest_width_dp = b.u16(off + kCfgSmallestWidthDp);
    if (has(kCfgWidthDp, 2)) c.screen_width_dp = b.u16(off + kCfgWidthDp);
    if (has(kCfgHeightDp, 2)) c.screen_height_dp = b.u16(off + kCfgHeightDp);
    // screenLayout at 28 is intentionally not surfaced: struct Config has no
    // field for it (ported verbatim in to_string as a penalty only).
    return c;
}

std::string density_qualifier(uint32_t density) {
    switch (density) {
        case 0: return {};
        case 120: return "ldpi";
        case 160: return "mdpi";
        case 213: return "tvdpi";
        case 240: return "hdpi";
        case 320: return "xhdpi";
        case 480: return "xxhdpi";
        case 640: return "xxxhdpi";
        case 0xFFFE: return "anydpi";
        case 0xFFFF: return "nodpi";
        default: return num(density) + "dpi";
    }
}

// ---------------------------------------------------------------------------
// Type chunks / entries
// ---------------------------------------------------------------------------
struct TypeChunk {
    uint32_t off = 0;          // absolute offset of the chunk header
    size_t end = 0;            // absolute end of the chunk (clamped to buffer)
    uint32_t entries_start = 0;  // absolute offset of the entry data
    uint32_t entry_count = 0;
    uint32_t offsets_at = 0;   // absolute offset of the entry offset table
    uint8_t flags = 0;
    Config config;
};

struct Data {
    std::vector<uint8_t> bytes;
    bool ok = false;
    uint32_t package_id = 0;
    std::string package_name;
    Pool value_pool;   // the table's global value string pool
    Pool type_pool;    // package typeStrings (names only, used by dump())
    Pool key_pool;     // package keyStrings (unused but parsed)
    std::array<std::vector<TypeChunk>, 256> types{};
    std::array<std::string, 256> type_names{};
    // Type chunks in the order they appear in the table: (type id, index).
    std::vector<std::pair<uint8_t, uint32_t>> order;
};

// Absolute offset of entry `idx` inside a type chunk, honouring the three
// offset encodings (uint32, uint16/4, sparse pairs).
bool entry_offset(const Buf& b, const TypeChunk& tc, uint32_t idx, uint32_t& out) {
    if ((tc.flags & kTypeFlagSparse) != 0) {
        // Array of {uint16 index, uint16 offset/4}, sorted by index.
        uint32_t lo = 0;
        uint32_t hi = tc.entry_count;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            const size_t pos = static_cast<size_t>(tc.offsets_at) + static_cast<size_t>(mid) * 4;
            if (!b.ok(pos, 4)) return false;
            const uint32_t sparse_idx = b.u16(pos);
            const uint32_t sparse_off = b.u16(pos + 2);
            if (sparse_idx == idx) {
                if (sparse_off == kNoEntry16) return false;
                out = tc.entries_start + 4u * sparse_off;
                return out < tc.end;
            }
            if (sparse_idx < idx) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return false;
    }
    if (idx >= tc.entry_count) return false;
    if ((tc.flags & kTypeFlagOffset16) != 0) {
        const size_t pos = static_cast<size_t>(tc.offsets_at) + static_cast<size_t>(idx) * 2;
        if (!b.ok(pos, 2)) return false;
        const uint32_t v = b.u16(pos);
        if (v == kNoEntry16) return false;
        out = tc.entries_start + 4u * v;
        return out < tc.end;
    }
    const size_t pos = static_cast<size_t>(tc.offsets_at) + static_cast<size_t>(idx) * 4;
    if (!b.ok(pos, 4)) return false;
    const uint32_t v = b.u32(pos);
    if (v == kNoEntry32) return false;
    out = tc.entries_start + v;
    return out < tc.end;
}

// Decodes a ResTable_entry (simple, bag, or the compact 8 byte form).
bool entry_at(const Buf& b, const Data& d, const TypeChunk& tc, uint32_t idx, Entry& out) {
    uint32_t off = 0;
    if (!entry_offset(b, tc, idx, off)) return false;
    if (!b.ok(off, 8)) return false;

    const uint32_t word = b.u32(off);
    const uint16_t flags = static_cast<uint16_t>(word >> 16);

    out.config = tc.config;

    if ((flags & kEntryFlagCompact) != 0) {
        // 8 byte entry: low 24 bits of the first word are the key index, the
        // high byte is the value type, the second word is the data.
        out.is_complex = false;
        out.type = static_cast<ValueType>((word >> 24) & 0xFF);
        out.data = b.u32(off + 4);
        if (out.type == ValueType::String) out.string = pool_string(b, d.value_pool, out.data);
        return true;
    }

    const uint16_t size = static_cast<uint16_t>(word & 0xFFFF);
    if (size < 8) return false;
    if (!b.ok(off, size)) return false;
    const size_t payload = static_cast<size_t>(off) + size;
    if (!b.ok(payload, 8)) return false;

    if ((flags & kEntryFlagComplex) != 0) {
        // ResTable_map_entry: the 8 byte entry header, then ResTable_ref parent
        // and uint32 count, then `count` ResTable_map (12 bytes each) that
        // start at entry + size (size is 16 for bag entries, 0 for odd ones).
        if (size < 16) return false;
        if (!b.ok(off, 16)) return false;
        out.is_complex = true;
        out.parent = b.u32(off + 8);
        uint32_t count = b.u32(off + 12);
        const size_t maps_at = static_cast<size_t>(off) + std::max<uint32_t>(size, 16);
        const size_t room = tc.end > maps_at ? tc.end - maps_at : 0;
        const uint32_t max_maps = static_cast<uint32_t>(room / 12);
        if (count > max_maps) count = max_maps;
        if (count > kMaxBagCount) count = kMaxBagCount;
        out.bag.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const size_t pos = maps_at + static_cast<size_t>(i) * 12;
            if (!b.ok(pos, 12)) break;
            BagEntry be;
            be.key = b.u32(pos);
            be.type = static_cast<ValueType>(b.u8(pos + 7));
            be.value = b.u32(pos + 8);
            if (be.type == ValueType::String) be.string = pool_string(b, d.value_pool, be.value);
            out.bag.push_back(std::move(be));
        }
        return true;
    }

    // Simple entry: Res_value {size, res0, dataType, data} right after it.
    out.is_complex = false;
    out.type = static_cast<ValueType>(b.u8(payload + 3));
    out.data = b.u32(payload + 4);
    if (out.type == ValueType::String) out.string = pool_string(b, d.value_pool, out.data);
    return true;
}

// ---------------------------------------------------------------------------
// Selection scoring
// ---------------------------------------------------------------------------
std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

int locale_score(const Config& c, const ConfigPreference& p) {
    const std::string pl = lower(p.language);
    const std::string pr = lower(p.region);
    const std::string cl = lower(c.language);
    const std::string cr = lower(c.region);
    if (pl.empty()) return cl.empty() ? 0 : 1;
    if (cl.empty()) return 3;
    if (cl != pl) return 4;
    if (pr.empty()) return cr.empty() ? 0 : 1;
    if (cr.empty()) return 1;
    return cr == pr ? 0 : 2;
}

int density_class(uint32_t cd, uint32_t pref) {
    // Lower is better; the second component of the returned key orders inside
    // a class (see density_key).
    if (pref == 0) {
        if (cd == 0xFFFE) return 0;
        if (cd == 0xFFFF) return 1;
        if (cd == 0) return 2;
        return 3;  // a real density, largest wins
    }
    if (cd == pref) return 0;
    if (cd == 0xFFFE) return 2;
    if (cd == 0xFFFF) return 3;
    if (cd == 0) return 4;
    if (cd > pref) return 1;  // smallest density >= requested wins
    return 5;                 // largest available below the request wins
}

int density_detail(uint32_t cd, uint32_t pref) {
    const int cls = density_class(cd, pref);
    if (cls == 1) return static_cast<int>(cd);            // ascending
    if (cls == 3 && pref == 0) return -static_cast<int>(cd);  // descending
    if (cls == 5) return -static_cast<int>(cd);           // descending
    return 0;
}

int other_penalty(const Config& c) {
    int n = 0;
    if (c.mcc != 0) ++n;
    if (c.mnc != 0) ++n;
    if (c.smallest_width_dp != 0) ++n;
    if (c.screen_width_dp != 0) ++n;
    if (c.screen_height_dp != 0) ++n;
    if (c.orientation != 0) ++n;
    if (c.ui_mode_type != 0 && c.ui_mode_type != 1) ++n;
    if (c.night_mode != 0) ++n;
    if (c.grammar_gender != 0) ++n;
    return n;
}

struct Score {
    int loc = 0;
    int other = 0;
    int sdk = 0;
    int dens_class = 0;
    int dens_detail = 0;
    int def = 0;
};

bool better(const Score& a, const Score& b) {
    if (a.loc != b.loc) return a.loc < b.loc;
    if (a.other != b.other) return a.other < b.other;
    if (a.dens_class != b.dens_class) return a.dens_class < b.dens_class;
    if (a.dens_detail != b.dens_detail) return a.dens_detail < b.dens_detail;
    if (a.sdk != b.sdk) return a.sdk < b.sdk;
    return a.def < b.def;
}

// ---------------------------------------------------------------------------
// Value formatting (dump() / debug)
// ---------------------------------------------------------------------------
const char* type_name(ValueType t) {
    switch (t) {
        case ValueType::Null: return "null";
        case ValueType::Reference: return "reference";
        case ValueType::Attribute: return "attribute";
        case ValueType::String: return "string";
        case ValueType::Float: return "float";
        case ValueType::Dimension: return "dimension";
        case ValueType::Fraction: return "fraction";
        case ValueType::DynamicReference: return "dynamic-ref";
        case ValueType::IntDec: return "int";
        case ValueType::IntHex: return "hex";
        case ValueType::IntBoolean: return "boolean";
        case ValueType::ColorArgb8: return "argb8";
        case ValueType::ColorRgb8: return "rgb8";
        case ValueType::ColorArgb4: return "argb4";
        case ValueType::ColorRgb4: return "rgb4";
    }
    return "unknown";
}

float complex_to_float(uint32_t data) {
    static const double kRadix[] = {1.0 / (1 << 23), 1.0 / (1 << 15), 1.0 / (1 << 7), 1.0};
    const int radix = static_cast<int>((data >> 4) & 0x3);
    const double m = static_cast<double>(static_cast<int32_t>(data & 0xFFFFFF00u));
    return static_cast<float>(m * kRadix[radix]);
}

std::string value_text(const Entry& e) {
    char buf[64];
    if (e.is_complex) {
        std::string s = "bag(parent=0x";
        std::snprintf(buf, sizeof buf, "%08x", e.parent);
        s += buf;
        s += ", " + num(static_cast<uint32_t>(e.bag.size())) + " maps)";
        return s;
    }
    switch (e.type) {
        case ValueType::String:
            return e.string ? "\"" + *e.string + "\"" : std::string("\"<bad pool index>\"");
        case ValueType::Reference:
        case ValueType::DynamicReference:
        case ValueType::Attribute: {
            std::snprintf(buf, sizeof buf, "@0x%08x", e.data);
            return buf;
        }
        case ValueType::IntDec:
            return std::to_string(static_cast<int32_t>(e.data));
        case ValueType::IntHex: {
            std::snprintf(buf, sizeof buf, "0x%08x", e.data);
            return buf;
        }
        case ValueType::IntBoolean:
            return e.data ? "true" : "false";
        case ValueType::Float: {
            std::snprintf(buf, sizeof buf, "%g", static_cast<double>(std::bit_cast<float>(e.data)));
            return buf;
        }
        case ValueType::Dimension: {
            static const char* kUnit[] = {"px", "dip", "sp", "pt", "in", "mm"};
            const uint32_t unit = e.data & 0xF;
            std::snprintf(buf, sizeof buf, "%g%s", static_cast<double>(complex_to_float(e.data)),
                          unit < 6 ? kUnit[unit] : "?");
            return buf;
        }
        case ValueType::Fraction: {
            const uint32_t unit = (e.data >> 4) & 0x3;  // 0 = %p, 1 = %p, others
            const float v = complex_to_float(static_cast<uint32_t>(e.data & 0xFFFFFF00u)) * 100.0f;
            std::snprintf(buf, sizeof buf, "%g%%%s", static_cast<double>(v), unit <= 1 ? "p" : "");
            return buf;
        }
        case ValueType::ColorArgb8:
        case ValueType::ColorArgb4:
            std::snprintf(buf, sizeof buf, "#%08x", e.data);
            return buf;
        case ValueType::ColorRgb8:
        case ValueType::ColorRgb4:
            std::snprintf(buf, sizeof buf, "#%06x", e.data & 0xFFFFFF);
            return buf;
        case ValueType::Null:
        default:
            break;
    }
    return "<" + std::string(type_name(e.type)) + " 0x" + num(e.data) + ">";
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
bool parse_package(const Buf& b, size_t off, Data& out);

bool parse_type_chunk(const Buf& b, size_t off, Data& out) {
    if (!b.ok(off, 20)) return false;
    const uint16_t header_size = b.u16(off + 2);
    const uint32_t size = b.u32(off + 4);
    if (header_size < 20 || size < header_size) return false;

    const uint8_t type_id = b.u8(off + 8);
    const uint8_t flags = b.u8(off + 9);
    const uint32_t entry_count_raw = b.u32(off + 12);
    const uint32_t entries_start_rel = b.u32(off + 16);
    if (type_id == 0) return false;
    if (entry_count_raw > kMaxEntryCount) return false;

    const size_t end = std::min<size_t>(b.d.size(), off + size);
    const size_t config_off = off + 20;
    if (config_off >= end) return false;

    TypeChunk tc;
    tc.off = static_cast<uint32_t>(off);
    tc.end = end;
    tc.flags = flags;
    tc.config = parse_config(b, config_off, end);
    tc.entry_count = entry_count_raw;

    const uint32_t cfg_size = b.u32(config_off);
    size_t offsets_at = off + header_size;
    if (cfg_size >= 4 && config_off + cfg_size > offsets_at) {
        // Some writers align the header size past the config; trust the larger.
        offsets_at = std::max(offsets_at, config_off + cfg_size);
    }
    if (offsets_at > end) return false;
    tc.offsets_at = static_cast<uint32_t>(offsets_at);

    const size_t room = end - offsets_at;
    uint32_t per_entry = 4;
    if ((flags & kTypeFlagSparse) != 0) {
        per_entry = 4;
    } else if ((flags & kTypeFlagOffset16) != 0) {
        per_entry = 2;
    }
    const uint32_t max_entries = static_cast<uint32_t>(room / per_entry);
    if (tc.entry_count > max_entries) tc.entry_count = max_entries;

    const uint64_t es = static_cast<uint64_t>(off) + entries_start_rel;
    if (es > end) return false;
    tc.entries_start = static_cast<uint32_t>(es);
    if (tc.entry_count == 0) return false;

    out.types[type_id].push_back(tc);
    out.order.emplace_back(type_id, static_cast<uint32_t>(out.types[type_id].size() - 1));
    return true;
}

bool parse_package(const Buf& b, size_t off, Data& out) {
    if (!b.ok(off, 284)) return false;
    const uint16_t header_size = b.u16(off + 2);
    const uint32_t size = b.u32(off + 4);
    if (header_size < 284 || size < header_size) return false;
    const size_t end = std::min<size_t>(b.d.size(), off + size);

    out.package_id = b.u32(off + 8);
    // Package name: 128 UTF-16 code units.
    std::string name;
    for (int i = 0; i < 128; ++i) {
        const uint16_t cu = b.u16(off + 12 + static_cast<size_t>(i) * 2);
        if (cu == 0) break;
        append_utf8(name, cu);
    }
    out.package_name = std::move(name);

    const uint32_t type_strings = b.u32(off + 268);
    const uint32_t key_strings = b.u32(off + 276);
    if (type_strings != 0 && static_cast<uint64_t>(off) + type_strings < end)
        out.type_pool = parse_pool(b, off + type_strings);
    if (key_strings != 0 && static_cast<uint64_t>(off) + key_strings < end)
        out.key_pool = parse_pool(b, off + key_strings);

    // Walk the sub chunks.
    size_t pos = off + header_size;
    int guard = 0;
    while (pos + 8 <= end && ++guard < 200000) {
        const uint16_t ctype = b.u16(pos);
        const uint32_t csize = b.u32(pos + 4);
        if (csize < 8) break;
        const size_t next = std::min<size_t>(end, pos + csize);
        switch (ctype) {
            case kChunkTableType:
                parse_type_chunk(b, pos, out);
                break;
            case kChunkTableTypeSpec:
                // Flags only, nothing we need; entryCount already lives in the
                // type chunks.
                break;
            default:
                break;
        }
        if (next <= pos) break;
        pos = next;
    }

    // Type names, for dump() only.
    for (int t = 1; t < 256; ++t) {
        if (out.types[static_cast<size_t>(t)].empty()) continue;
        out.type_names[static_cast<size_t>(t)] =
            pool_string(b, out.type_pool, static_cast<uint32_t>(t) - 1).value_or(std::string());
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
std::string Config::to_string() const {
    std::string s;
    auto add = [&s](const std::string& part) {
        if (part.empty()) return;
        if (!s.empty()) s += ", ";
        s += part;
    };

    if (mcc != 0) add("mcc" + num(mcc));
    if (mnc != 0) add("mnc" + num(mnc));
    if (!language.empty()) {
        std::string loc = language;
        if (!region.empty()) loc += "-r" + region;
        add(loc);
    } else if (!region.empty()) {
        add(region);
    }
    if (smallest_width_dp != 0) add("sw" + num(smallest_width_dp) + "dp");
    if (screen_width_dp != 0) add("w" + num(screen_width_dp) + "dp");
    if (screen_height_dp != 0) add("h" + num(screen_height_dp) + "dp");
    switch (orientation) {
        case 1: add("port"); break;
        case 2: add("land"); break;
        case 3: add("square"); break;
        default: break;
    }
    switch (ui_mode_type) {
        case 2: add("desk"); break;
        case 3: add("car"); break;
        case 4: add("television"); break;
        case 5: add("appliance"); break;
        case 6: add("watch"); break;
        case 7: add("vrheadset"); break;
        default: break;
    }
    switch (night_mode) {
        case 1: add("notnight"); break;
        case 2: add("night"); break;
        default: break;
    }
    switch (grammar_gender) {
        case 1: add("neuter"); break;
        case 2: add("feminine"); break;
        case 3: add("masculine"); break;
        default: break;
    }
    add(density_qualifier(density));
    if (sdk_version != 0) add("v" + num(sdk_version));
    return s;
}

bool Config::is_default() const {
    return density == 0 && sdk_version == 0 && language.empty() && region.empty() && mcc == 0 &&
           mnc == 0 && screen_width_dp == 0 && screen_height_dp == 0 && smallest_width_dp == 0 &&
           orientation == 0 && ui_mode_type == 0 && night_mode == 0 && grammar_gender == 0;
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------
struct Table::Impl {
    std::shared_ptr<const Data> data;
};

Table::Table() : impl_(new Impl()) {}
Table::~Table() { delete impl_; }

Table::Table(Table&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }

Table& Table::operator=(Table&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

Table::Table(const Table& other) : impl_(other.impl_ ? new Impl(*other.impl_) : nullptr) {}

Table& Table::operator=(const Table& other) {
    if (this != &other) {
        Impl* fresh = other.impl_ ? new Impl(*other.impl_) : nullptr;
        delete impl_;
        impl_ = fresh;
    }
    return *this;
}

std::optional<Table> Table::parse(std::span<const uint8_t> data, std::string* error) {
    auto fail = [error](std::string msg) -> std::optional<Table> {
        if (error) *error = std::move(msg);
        return std::nullopt;
    };
    if (error) error->clear();
    if (data.size() < 12) return fail("buffer too small for a resource table");

    Buf b{data};
    if (b.u16(0) != kChunkTable) return fail("not a RES_TABLE_TYPE chunk");
    const uint16_t table_header = b.u16(2);
    const uint32_t table_size = b.u32(4);
    if (table_header < 12) return fail("bad resource table header size");
    if (table_size < table_header) return fail("bad resource table size");

    auto d = std::make_shared<Data>();
    d->bytes.assign(data.begin(), data.end());
    const Buf cb{std::span<const uint8_t>(d->bytes.data(), d->bytes.size())};

    const size_t table_end = std::min<size_t>(cb.d.size(), table_size);
    size_t pos = table_header;
    bool have_package = false;
    bool have_value_pool = false;
    int guard = 0;
    while (pos + 8 <= table_end && ++guard < 200000) {
        const uint16_t ctype = cb.u16(pos);
        const uint32_t csize = cb.u32(pos + 4);
        if (csize < 8) break;
        const size_t next = std::min<size_t>(table_end, pos + csize);
        if (ctype == kChunkStringPool && !have_value_pool) {
            d->value_pool = parse_pool(cb, pos);
            have_value_pool = d->value_pool.valid;
        } else if (ctype == kChunkTablePackage && !have_package) {
            have_package = parse_package(cb, pos, *d);
        }
        if (next <= pos) break;
        pos = next;
    }

    if (!have_package) return fail("no RES_TABLE_PACKAGE_TYPE chunk found");
    if (!have_value_pool) return fail("no global value string pool found");
    if (d->package_id == 0) return fail("package chunk has an invalid id");

    d->ok = true;
    Table t;
    if (!t.impl_) return fail("out of memory");
    t.impl_->data = std::move(d);
    return t;
}

bool Table::valid() const { return impl_ != nullptr && impl_->data && impl_->data->ok; }

const std::string& Table::package_name() const {
    static const std::string kEmpty;
    if (!valid()) return kEmpty;
    return impl_->data->package_name;
}

namespace {

const Data* data_of(const Table::Impl* impl) {
    if (!impl || !impl->data || !impl->data->ok) return nullptr;
    return impl->data.get();
}

// Collects every entry of `res_id` in table order (chunk order).
std::vector<Entry> collect_entries(const Data& d, uint32_t res_id) {
    std::vector<Entry> out;
    if (d.package_id != 0 && ((res_id >> 24) & 0xFF) != d.package_id) return out;
    const uint32_t type_id = (res_id >> 16) & 0xFF;
    const uint32_t idx = res_id & 0xFFFF;
    if (type_id == 0 || type_id > 255) return out;
    const Buf b{std::span<const uint8_t>(d.bytes.data(), d.bytes.size())};
    const auto& chunks = d.types[type_id];
    for (const auto& tc : chunks) {
        Entry e;
        if (entry_at(b, d, tc, idx, e)) out.push_back(std::move(e));
    }
    return out;
}

bool any_entry(const Data& d, uint32_t res_id) {
    if (d.package_id != 0 && ((res_id >> 24) & 0xFF) != d.package_id) return false;
    const uint32_t type_id = (res_id >> 16) & 0xFF;
    const uint32_t idx = res_id & 0xFFFF;
    if (type_id == 0 || type_id > 255) return false;
    const Buf b{std::span<const uint8_t>(d.bytes.data(), d.bytes.size())};
    for (const auto& tc : d.types[type_id]) {
        uint32_t off = 0;
        if (entry_offset(b, tc, idx, off)) return true;
    }
    return false;
}

}  // namespace

bool Table::has(uint32_t res_id) const {
    const Data* d = data_of(impl_);
    return d != nullptr && any_entry(*d, res_id);
}

std::vector<Entry> Table::entries(uint32_t res_id) const {
    const Data* d = data_of(impl_);
    if (!d) return {};
    return collect_entries(*d, res_id);
}

std::optional<Entry> Table::best_entry(uint32_t res_id, const ConfigPreference& pref) const {
    const Data* d = data_of(impl_);
    if (!d) return std::nullopt;
    const std::vector<Entry> all = collect_entries(*d, res_id);

    const Entry* best = nullptr;
    Score best_score;
    for (const Entry& e : all) {
        if (pref.sdk_version > 0 && e.config.sdk_version > pref.sdk_version) continue;
        Score sc;
        sc.loc = locale_score(e.config, pref);
        sc.other = other_penalty(e.config);
        sc.sdk = pref.sdk_version > 0 ? -static_cast<int>(e.config.sdk_version)  // highest wins
                                      : -static_cast<int>(e.config.sdk_version);
        sc.dens_class = density_class(e.config.density, pref.density);
        sc.dens_detail = density_detail(e.config.density, pref.density);
        sc.def = (pref.prefer_default && e.config.is_default()) ? 0 : 1;
        if (best == nullptr || better(sc, best_score)) {
            best = &e;
            best_score = sc;
        }
    }
    if (best == nullptr) return std::nullopt;
    return *best;
}

std::optional<ResolvedValue> Table::resolve(uint32_t res_id, const ConfigPreference& pref) const {
    const Data* d = data_of(impl_);
    if (!d) return std::nullopt;
    if (!any_entry(*d, res_id)) return std::nullopt;

    uint32_t current = res_id;
    std::optional<uint32_t> redirected;
    for (int depth = 0; depth < kMaxReferenceDepth; ++depth) {
        std::optional<Entry> e = best_entry(current, pref);
        if (!e) {
            // A link in the chain is not in this table (typically a framework
            // resource): report the dangling id instead of failing.
            ResolvedValue rv;
            rv.type = ValueType::Reference;
            rv.data = current;
            rv.unresolved_reference = true;
            rv.redirected_from = redirected;
            return rv;
        }
        if (e->is_complex) {
            ResolvedValue rv;
            rv.type = ValueType::Null;
            rv.config = e->config;
            rv.is_complex = true;
            rv.bag = e->bag;
            rv.redirected_from = redirected;
            return rv;
        }
        if (e->type == ValueType::Reference || e->type == ValueType::DynamicReference) {
            if (!redirected.has_value()) redirected = e->data;
            current = e->data;
            continue;
        }
        ResolvedValue rv;
        rv.type = e->type;
        rv.data = e->data;
        rv.string = e->string;
        rv.config = e->config;
        rv.redirected_from = redirected;
        return rv;
    }

    ResolvedValue rv;
    rv.type = ValueType::Reference;
    rv.data = current;
    rv.unresolved_reference = true;
    rv.redirected_from = redirected;
    return rv;
}

std::optional<std::string> Table::resolve_string(uint32_t res_id, const ConfigPreference& pref) const {
    std::optional<ResolvedValue> rv = resolve(res_id, pref);
    if (!rv || !rv->string) return std::nullopt;
    return rv->string;
}

std::optional<int64_t> Table::resolve_int(uint32_t res_id, const ConfigPreference& pref) const {
    std::optional<ResolvedValue> rv = resolve(res_id, pref);
    if (!rv) return std::nullopt;
    switch (rv->type) {
        case ValueType::IntDec:
            return static_cast<int64_t>(static_cast<int32_t>(rv->data));
        case ValueType::IntHex:
            return static_cast<int64_t>(rv->data);
        case ValueType::IntBoolean:
            return rv->data ? 1 : 0;
        default:
            return std::nullopt;
    }
}

std::string Table::dump() const {
    const Data* d = data_of(impl_);
    if (!d) return "<invalid table>\n";

    char buf[128];
    std::string out;
    out += "package " + d->package_name + " (id=0x";
    std::snprintf(buf, sizeof buf, "%02x", d->package_id);
    out += buf;
    out += ")\n";

    const Buf b{std::span<const uint8_t>(d->bytes.data(), d->bytes.size())};
    for (int t = 1; t < 256; ++t) {
        const auto& chunks = d->types[static_cast<size_t>(t)];
        if (chunks.empty()) continue;
        out += "\ntype 0x";
        std::snprintf(buf, sizeof buf, "%02x", t);
        out += buf;
        const std::string& tn = d->type_names[static_cast<size_t>(t)];
        if (!tn.empty()) out += " " + tn;
        out += " (" + num(static_cast<uint32_t>(chunks.size())) + " configs)\n";

        std::set<uint32_t> ids;
        for (const auto& tc : chunks) {
            if ((tc.flags & kTypeFlagSparse) != 0) {
                for (uint32_t i = 0; i < tc.entry_count; ++i) {
                    const size_t pos = static_cast<size_t>(tc.offsets_at) + static_cast<size_t>(i) * 4;
                    if (!b.ok(pos, 4)) break;
                    if (b.u16(pos + 2) != kNoEntry16) ids.insert(b.u16(pos));
                }
            } else if ((tc.flags & kTypeFlagOffset16) != 0) {
                for (uint32_t i = 0; i < tc.entry_count; ++i) {
                    const size_t pos = static_cast<size_t>(tc.offsets_at) + static_cast<size_t>(i) * 2;
                    if (!b.ok(pos, 2)) break;
                    if (b.u16(pos) != kNoEntry16) ids.insert(i);
                }
            } else {
                for (uint32_t i = 0; i < tc.entry_count; ++i) {
                    const size_t pos = static_cast<size_t>(tc.offsets_at) + static_cast<size_t>(i) * 4;
                    if (!b.ok(pos, 4)) break;
                    if (b.u32(pos) != kNoEntry32) ids.insert(i);
                }
            }
        }
        for (const uint32_t idx : ids) {
            if (idx > 0xFFFF) continue;  // not addressable, cannot appear in a resource id
            const uint32_t res_id = (d->package_id << 24) | (static_cast<uint32_t>(t) << 16) | idx;
            bool printed_header = false;
            for (const auto& tc : chunks) {
                Entry e;
                if (!entry_at(b, *d, tc, idx, e)) continue;
                if (!printed_header) {
                    std::snprintf(buf, sizeof buf, "  0x%08x", res_id);
                    out += buf;
                    out += ":\n";
                    printed_header = true;
                }
                out += "    (" + e.config.to_string() + ") = " + value_text(e) + "\n";
                for (const auto& be : e.bag) {
                    std::snprintf(buf, sizeof buf, "      0x%08x -> ", be.key);
                    out += buf;
                    Entry sub;
                    sub.type = be.type;
                    sub.data = be.value;
                    sub.string = be.string;
                    out += value_text(sub);
                    out += " [" + std::string(type_name(be.type)) + "]\n";
                }
            }
        }
    }
    return out;
}

namespace {

constexpr size_t kMaxConfigurations = 4096;

// Canonical key over every field of Config (to_string() is user facing and
// deliberately not injective: e.g. ui_mode_type 1 prints nothing).
std::string config_key(const Config& c) {
    std::string k;
    k.reserve(48);
    const uint32_t nums[] = {c.density,          c.sdk_version,      c.mcc,           c.mnc,
                             c.screen_width_dp,  c.screen_height_dp, c.smallest_width_dp,
                             c.orientation,      c.ui_mode_type,     c.night_mode,    c.grammar_gender};
    for (const uint32_t v : nums) {
        k += std::to_string(v);
        k += ',';
    }
    k += c.language;
    k += '\x1f';
    k += c.region;
    return k;
}

}  // namespace

std::vector<Config> Table::configurations() const {
    std::vector<Config> out;
    const Data* d = data_of(impl_);
    if (!d) return out;
    out.reserve(64);
    std::set<std::string> seen;
    for (const auto& ref : d->order) {
        const auto& chunks = d->types[ref.first];
        if (ref.second >= chunks.size()) continue;
        const Config& c = chunks[ref.second].config;
        if (!seen.insert(config_key(c)).second) continue;
        out.push_back(c);
        if (out.size() >= kMaxConfigurations) break;
    }
    return out;
}

bool looks_like_arsc(std::span<const uint8_t> data) {
    if (data.size() < 12) return false;
    const Buf b{data};
    if (b.u16(0) != kChunkTable) return false;
    const uint16_t header_size = b.u16(2);
    const uint32_t size = b.u32(4);
    return header_size >= 12 && size >= header_size;
}

}  // namespace apkinfo::arsc
