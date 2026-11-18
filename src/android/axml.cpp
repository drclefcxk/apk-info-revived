// Implementation of the binary Android XML (AXML) parser declared in axml.h.
//
// Layout recap (all little endian):
//   ResChunk_header { uint16 type; uint16 headerSize; uint32 size; }
//   RES_XML_TYPE (0x0003) wraps:
//     RES_STRING_POOL_TYPE (0x0001)
//     RES_XML_RESOURCE_MAP_TYPE (0x0180)  optional
//     ResXMLTree_node chunks (0x0100..0x0104) with { lineNumber, comment }
//     followed by node specific ext data.
//
// Everything here is defensive: the input is arbitrary untrusted APK content, so
// every offset, count and index is bounds-checked against the buffer before it is
// dereferenced, nesting depth and total element count are capped, and no
// exception is allowed to escape parse().

#include "axml.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace apkinfo::axml {
namespace {

// ---------------------------------------------------------------------------
// chunk types
// ---------------------------------------------------------------------------
constexpr uint16_t kResNull = 0x0000;
constexpr uint16_t kResStringPoolType = 0x0001;
constexpr uint16_t kResXmlType = 0x0003;
constexpr uint16_t kResXmlStartNamespace = 0x0100;
constexpr uint16_t kResXmlEndNamespace = 0x0101;
constexpr uint16_t kResXmlStartElement = 0x0102;
constexpr uint16_t kResXmlEndElement = 0x0103;
constexpr uint16_t kResXmlCData = 0x0104;
constexpr uint16_t kResXmlResourceMapType = 0x0180;

constexpr uint32_t kNoIndex = 0xFFFFFFFFu;
constexpr uint32_t kUtf8Flag = 1u << 8;  // ResStringPool_header::UTF8_FLAG

// hard limits against hostile input
constexpr int kMaxDepth = 256;
constexpr size_t kMaxElements = 200000;
constexpr uint32_t kMaxStrings = 1000000;
constexpr uint32_t kMaxResIds = 1000000;

// sizes of the fixed structs we read
constexpr size_t kNodeHeaderSize = 16;   // ResChunk_header + lineNumber + comment
constexpr size_t kAttrExtSize = 20;      // ResXMLTree_attrExt
constexpr size_t kAttributeSize = 20;    // ResXMLTree_attribute
constexpr size_t kNamespaceExtSize = 8;  // prefix + uri
constexpr size_t kElementExtSize = 8;    // ns + name (ResXMLTree_endElementExt)

// ---------------------------------------------------------------------------
// bounds-checked little endian readers
// ---------------------------------------------------------------------------
bool in_range(std::span<const uint8_t> d, size_t off, size_t n) {
    return off <= d.size() && n <= d.size() - off;
}

bool read_u8(std::span<const uint8_t> d, size_t off, uint8_t& out) {
    if (!in_range(d, off, 1)) return false;
    out = d[off];
    return true;
}

bool read_u16(std::span<const uint8_t> d, size_t off, uint16_t& out) {
    if (!in_range(d, off, 2)) return false;
    out = static_cast<uint16_t>(static_cast<uint32_t>(d[off]) |
                                (static_cast<uint32_t>(d[off + 1]) << 8));
    return true;
}

bool read_u32(std::span<const uint8_t> d, size_t off, uint32_t& out) {
    if (!in_range(d, off, 4)) return false;
    out = static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
          (static_cast<uint32_t>(d[off + 2]) << 16) |
          (static_cast<uint32_t>(d[off + 3]) << 24);
    return true;
}

struct ChunkHeader {
    uint16_t type = 0;
    uint16_t header_size = 0;
    uint32_t size = 0;
};

bool read_chunk_header(std::span<const uint8_t> d, size_t off, ChunkHeader& out) {
    if (!read_u16(d, off, out.type)) return false;
    if (!read_u16(d, off + 2, out.header_size)) return false;
    if (!read_u32(d, off + 4, out.size)) return false;
    return true;
}

// A chunk is usable when its declared size is sane, its header fits inside its
// size, and the whole chunk lies inside the buffer (bounded by `limit`).
bool chunk_fits(const ChunkHeader& h, size_t off, size_t limit) {
    if (h.size < 8) return false;
    if (h.header_size < 8) return false;
    if (h.header_size > h.size) return false;
    if (off > limit) return false;
    return static_cast<size_t>(h.size) <= limit - off;
}

// ---------------------------------------------------------------------------
// UTF helpers
// ---------------------------------------------------------------------------
void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// Convert `units` UTF-16 code units (already bounds-checked by the caller) to
// UTF-8. Unpaired surrogates become U+FFFD so we never emit invalid UTF-8.
std::string utf16_to_utf8(std::span<const uint8_t> d, size_t off, size_t units) {
    std::string out;
    out.reserve(units);
    size_t i = 0;
    while (i < units) {
        uint16_t hi = 0;
        if (!read_u16(d, off + i * 2, hi)) break;
        ++i;
        uint32_t cp = hi;
        if (hi >= 0xD800u && hi <= 0xDBFFu && i < units) {
            uint16_t lo = 0;
            if (read_u16(d, off + i * 2, lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                ++i;
                cp = 0x10000u + ((static_cast<uint32_t>(hi - 0xD800u) << 10) |
                                 static_cast<uint32_t>(lo - 0xDC00u));
            } else {
                cp = 0xFFFDu;
            }
        } else if (hi >= 0xD800u && hi <= 0xDFFFu) {
            cp = 0xFFFDu;
        }
        append_utf8(out, cp);
    }
    return out;
}

// ---------------------------------------------------------------------------
// value rendering
// ---------------------------------------------------------------------------
void append_hex8(std::string& out, uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    out = buf;
}

std::string hex8(uint32_t v) {
    std::string s;
    append_hex8(s, v);
    return s;
}

std::string hex_lower(uint32_t v, int min_digits) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*x", min_digits, v);
    return buf;
}

// Res_value dimension/fraction packing:
//   bits 0..3  unit, bits 4..5 radix, bits 8..31 signed 24-bit mantissa
// AOSP: value = (data & 0xffffff00) * RADIX_MULTS[(data >> 4) & 3] where
// RADIX_MULTS = { 2^-8, 2^-15, 2^-23, 2^-31 } (i.e. raw24 * 2^0/-7/-15/-23).
double complex_to_double(uint32_t data) {
    static const double kRadixMults[4] = {1.0 / 256.0, 1.0 / 32768.0, 1.0 / 8388608.0,
                                          1.0 / 2147483648.0};
    int32_t signed_data = 0;
    std::memcpy(&signed_data, &data, sizeof signed_data);
    int32_t mantissa = signed_data & static_cast<int32_t>(0xFFFFFF00u);
    int radix = (data >> 4) & 0x3u;
    double v = static_cast<double>(mantissa) * kRadixMults[radix];
    return v;
}

std::string format_dimension(uint32_t data) {
    // aapt2 renders dimensions/fractions with "%f" (six decimals) plus the unit
    // name, and "???" for a unit it does not know.
    static const char* const kUnits[6] = {"px", "dp", "sp", "pt", "in", "mm"};
    uint32_t unit = data & 0x0Fu;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%f", complex_to_double(data));
    std::string out(buf);
    out += (unit < 6) ? kUnits[unit] : "???";
    return out;
}

std::string format_fraction(uint32_t data) {
    uint32_t unit = data & 0x0Fu;  // 0 = %, 1 = %p
    char buf[64];
    std::snprintf(buf, sizeof buf, "%f", complex_to_double(data));
    std::string out(buf);
    out += (unit == 0) ? "%" : (unit == 1) ? "%p" : "???";
    return out;
}

// Colors are printed by aapt2 as the raw packed word zero padded to 8 digits.
std::string format_color(uint32_t data) { return "#" + hex_lower(data, 8); }

std::string format_float(uint32_t data) {
    float f = 0.0f;
    std::memcpy(&f, &data, sizeof f);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(f));
    return buf;
}

// ---------------------------------------------------------------------------
// string pool
// ---------------------------------------------------------------------------
// Eagerly decodes every string. `off` points at the pool chunk header.
bool parse_string_pool(std::span<const uint8_t> d, size_t off, const ChunkHeader& h,
                       std::vector<std::string>& out, std::string& err) {
    if (h.header_size < 28) {
        err = "string pool header too small";
        return false;
    }
    uint32_t string_count = 0, style_count = 0, flags = 0, strings_start = 0,
             styles_start = 0;
    if (!read_u32(d, off + 8, string_count) || !read_u32(d, off + 12, style_count) ||
        !read_u32(d, off + 16, flags) || !read_u32(d, off + 20, strings_start) ||
        !read_u32(d, off + 24, styles_start)) {
        err = "truncated string pool header";
        return false;
    }
    if (string_count > kMaxStrings || style_count > kMaxStrings) {
        err = "string pool count out of range";
        return false;
    }
    // offsets array (index table) must fit in the chunk
    const uint64_t index_bytes = static_cast<uint64_t>(string_count) * 4 +
                                 static_cast<uint64_t>(style_count) * 4;
    if (static_cast<uint64_t>(h.header_size) + index_bytes > h.size) {
        err = "string pool index table exceeds chunk";
        return false;
    }
    if (strings_start > h.size || (string_count > 0 && strings_start < h.header_size)) {
        err = "string pool stringsStart out of range";
        return false;
    }
    const size_t chunk_begin = off;
    const size_t chunk_end = off + h.size;
    const size_t strings_base = chunk_begin + strings_start;
    const bool utf8 = (flags & kUtf8Flag) != 0;

    out.clear();
    out.reserve(string_count);

    for (uint32_t i = 0; i < string_count; ++i) {
        uint32_t str_off = 0;
        if (!read_u32(d, off + h.header_size + static_cast<size_t>(i) * 4, str_off) ||
            str_off == kNoIndex) {
            out.emplace_back();
            continue;
        }
        size_t p = strings_base + str_off;
        if (p >= chunk_end || p < strings_base) {
            out.emplace_back();
            continue;
        }
        if (utf8) {
            // 1 or 2 bytes of UTF-16 character count, then 1 or 2 bytes of byte
            // count, then that many bytes. (The trailing NUL and the 4-byte
            // padding are skipped implicitly: we index strings directly.)
            uint8_t b0 = 0;
            if (!read_u8(d, p, b0) || p >= chunk_end) {
                out.emplace_back();
                continue;
            }
            size_t len_bytes = 1;
            if (b0 & 0x80u) {
                uint8_t b1 = 0;
                if (!read_u8(d, p + 1, b1)) {
                    out.emplace_back();
                    continue;
                }
                len_bytes = 2;
            }
            size_t q = p + len_bytes;
            uint8_t c0 = 0;
            if (!read_u8(d, q, c0) || q >= chunk_end) {
                out.emplace_back();
                continue;
            }
            size_t byte_len = 0;
            if (c0 & 0x80u) {
                uint8_t c1 = 0;
                if (!read_u8(d, q + 1, c1)) {
                    out.emplace_back();
                    continue;
                }
                byte_len = (static_cast<size_t>(c0 & 0x7Fu) << 8) | c1;
                q += 2;
            } else {
                byte_len = c0;
                q += 1;
            }
            if (q > chunk_end || byte_len > chunk_end - q) {
                out.emplace_back();
                continue;
            }
            out.emplace_back(reinterpret_cast<const char*>(d.data() + q), byte_len);
        } else {
            uint16_t l0 = 0;
            if (!read_u16(d, p, l0) || p + 2 > chunk_end) {
                out.emplace_back();
                continue;
            }
            size_t q = p + 2;
            size_t char_len = 0;
            if (l0 & 0x8000u) {
                uint16_t l1 = 0;
                if (!read_u16(d, p + 2, l1)) {
                    out.emplace_back();
                    continue;
                }
                char_len = (static_cast<size_t>(l0 & 0x7FFFu) << 16) | l1;
                q = p + 4;
            } else {
                char_len = l0;
            }
            if (q > chunk_end || char_len > (chunk_end - q) / 2) {
                out.emplace_back();
                continue;
            }
            out.emplace_back(utf16_to_utf8(d, q, char_len));
        }
    }
    (void)styles_start;
    return true;
}

// ---------------------------------------------------------------------------
// value decoding
// ---------------------------------------------------------------------------
Value decode_value(const std::vector<std::string>& strings, uint8_t data_type,
                   uint32_t data) {
    Value v;
    v.type = static_cast<ValueType>(data_type);
    v.data = data;
    switch (v.type) {
        case ValueType::Null:
            // Res_value TYPE_NULL: data 0 = null, data 1 = "@empty" (aapt2
            // prints exactly these tokens).
            v.text = (data == 1u) ? "@empty" : "@null";
            break;
        case ValueType::String:
            if (data < strings.size()) v.text = strings[data];
            break;
        case ValueType::Reference:
        case ValueType::DynamicReference:
            v.text = "@" + hex8(data);
            break;
        case ValueType::Attribute:
            v.text = "?" + hex8(data);
            break;
        case ValueType::Float:
            v.text = format_float(data);
            break;
        case ValueType::Dimension:
            v.text = format_dimension(data);
            break;
        case ValueType::Fraction:
            v.text = format_fraction(data);
            break;
        case ValueType::IntDec: {
            int32_t s = 0;
            std::memcpy(&s, &data, sizeof s);
            v.text = std::to_string(s);
            break;
        }
        case ValueType::IntHex:
            // aapt2 renders hex ints zero padded to 8 digits ("0x00000012").
            v.text = hex8(data);
            break;
        case ValueType::IntBoolean:
            v.text = (data != 0) ? "true" : "false";
            break;
        case ValueType::ColorArgb8:
        case ValueType::ColorRgb8:
        case ValueType::ColorArgb4:
        case ValueType::ColorRgb4:
            // "#aarrggbb": the packed word, zero padded (this is what aapt2
            // prints for all four color encodings).
            v.text = format_color(data);
            break;
        default:
            break;
    }
    return v;
}

// ---------------------------------------------------------------------------
// parser
// ---------------------------------------------------------------------------
class Parser {
public:
    Parser(std::span<const uint8_t> data, Document& doc) : d_(data), doc_(doc) {}

    bool run(std::string& err) {
        ChunkHeader root;
        if (!read_chunk_header(d_, 0, root)) {
            err = "buffer too small for a chunk header";
            return false;
        }
        if (root.type != kResXmlType) {
            err = "not a compiled XML document (unexpected chunk type)";
            return false;
        }
        if (root.size < 8 || root.size > d_.size()) {
            err = "XML chunk size out of range";
            return false;
        }
        // The XML chunk body starts after its own header; tolerate a header
        // larger than 8 (some writers pad it) as long as it stays inside.
        size_t pos = (root.header_size >= 8 && root.header_size <= root.size) ? root.header_size
                                                                            : 8;
        xml_end_ = root.size;
        if (pos > xml_end_) {
            err = "XML chunk header exceeds chunk size";
            return false;
        }
        while (pos + 8 <= xml_end_) {
            ChunkHeader h;
            if (!read_chunk_header(d_, pos, h)) {
                err = "truncated chunk header";
                return false;
            }
            if (!chunk_fits(h, pos, xml_end_)) {
                err = "chunk extends past the document";
                return false;
            }
            switch (h.type) {
                case kResStringPoolType:
                    if (!pool_done_) {
                        if (!parse_string_pool(d_, pos, h, strings_, err)) return false;
                        pool_done_ = true;
                    }
                    break;
                case kResXmlResourceMapType:
                    if (!parse_resource_map(pos, h, err)) return false;
                    break;
                case kResXmlStartNamespace:
                case kResXmlEndNamespace:
                case kResXmlStartElement:
                case kResXmlEndElement:
                case kResXmlCData:
                case kResNull:
                    break;  // handled by parse_nodes
                default:
                    break;  // unknown chunk: skip by size
            }
            pos += h.size;
        }
        if (!pool_done_) {
            err = "document has no string pool";
            return false;
        }
        node_pos_ = (root.header_size >= 8 && root.header_size <= root.size)
                        ? root.header_size
                        : 8;
        if (!parse_nodes(doc_.roots, 1, err)) return false;
        return true;
    }

private:
    bool parse_resource_map(size_t off, const ChunkHeader& h, std::string& err) {
        const size_t payload = h.size - h.header_size;
        const uint32_t count = static_cast<uint32_t>(payload / 4);
        if (count > kMaxResIds) {
            err = "resource map too large";
            return false;
        }
        res_ids_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = 0;
            if (!read_u32(d_, off + h.header_size + static_cast<size_t>(i) * 4, id)) break;
            res_ids_.push_back(id);
        }
        return true;
    }

    std::string str(uint32_t idx) const {
        if (idx == kNoIndex) return std::string();
        if (idx < strings_.size()) return strings_[idx];
        return std::string();
    }

    // "android" for the android namespace URI, "*android" prefix kept as-is.
    std::string prefix_for(const std::string& uri) const {
        if (uri.empty()) return std::string();
        for (size_t i = ns_stack_.size(); i > 0; --i) {
            if (ns_stack_[i - 1].uri == uri) return ns_stack_[i - 1].prefix;
        }
        return std::string();
    }

    void record_namespace(const Namespace& ns) {
        for (const auto& existing : doc_.namespaces) {
            if (existing.prefix == ns.prefix && existing.uri == ns.uri) return;
        }
        doc_.namespaces.push_back(ns);
    }

    bool parse_nodes(std::vector<Element>& out, int depth, std::string& err) {
        if (depth > kMaxDepth) {
            err = "element nesting deeper than the supported maximum";
            return false;
        }
        for (;;) {
            if (node_pos_ + 8 > xml_end_) {
                if (depth > 1) {
                    err = "unexpected end of document inside an element";
                    return false;
                }
                return true;
            }
            ChunkHeader h;
            if (!read_chunk_header(d_, node_pos_, h)) {
                err = "truncated chunk header";
                return false;
            }
            if (!chunk_fits(h, node_pos_, xml_end_)) {
                err = "node chunk extends past the document";
                return false;
            }
            const size_t node_start = node_pos_;
            const size_t node_end = node_start + h.size;
            // Non-node chunks (string pool, resource map, anything unknown) are
            // skipped by their size; only real XML node chunks need the bigger
            // node header.
            const bool is_node = h.type >= kResXmlStartNamespace && h.type <= kResXmlCData;
            if (!is_node && h.type != kResNull) {
                node_pos_ = node_end;
                continue;
            }
            if (h.header_size < kNodeHeaderSize) {
                err = "node header too small";
                return false;
            }

            switch (h.type) {
                case kResXmlStartNamespace: {
                    if (node_end - node_start < kNodeHeaderSize + kNamespaceExtSize) {
                        err = "truncated start-namespace node";
                        return false;
                    }
                    uint32_t prefix_idx = 0, uri_idx = 0;
                    read_u32(d_, node_start + kNodeHeaderSize, prefix_idx);
                    read_u32(d_, node_start + kNodeHeaderSize + 4, uri_idx);
                    Namespace ns;
                    ns.prefix = str(prefix_idx);
                    ns.uri = str(uri_idx);
                    ns_stack_.push_back(ns);
                    record_namespace(ns);
                    break;
                }
                case kResXmlEndNamespace: {
                    if (node_end - node_start < kNodeHeaderSize + kNamespaceExtSize) {
                        err = "truncated end-namespace node";
                        return false;
                    }
                    uint32_t uri_idx = 0;
                    read_u32(d_, node_start + kNodeHeaderSize + 4, uri_idx);
                    const std::string uri = str(uri_idx);
                    for (size_t i = ns_stack_.size(); i > 0; --i) {
                        if (ns_stack_[i - 1].uri == uri) {
                            ns_stack_.erase(ns_stack_.begin() + static_cast<long>(i - 1));
                            break;
                        }
                    }
                    break;
                }
                case kResXmlStartElement: {
                    if (out.size() >= kMaxElements || element_count_ >= kMaxElements) {
                        err = "document has too many elements";
                        return false;
                    }
                    if (node_end - node_start <
                        kNodeHeaderSize + kAttrExtSize) {
                        err = "truncated start-element node";
                        return false;
                    }
                    Element el;
                    {
                        uint32_t line = 0;
                        read_u32(d_, node_start + 8, line);  // ResXMLTree_node.lineNumber
                        el.line = (line <= static_cast<uint32_t>(std::numeric_limits<int>::max()))
                                      ? static_cast<int>(line)
                                      : 0;
                    }
                    uint32_t ns_idx = 0, name_idx = 0;
                    uint16_t attribute_start = kAttrExtSize, attribute_size = kAttrExtSize,
                             attribute_count = 0;
                    read_u32(d_, node_start + kNodeHeaderSize, ns_idx);
                    read_u32(d_, node_start + kNodeHeaderSize + 4, name_idx);
                    read_u16(d_, node_start + kNodeHeaderSize + 8, attribute_start);
                    read_u16(d_, node_start + kNodeHeaderSize + 10, attribute_size);
                    read_u16(d_, node_start + kNodeHeaderSize + 12, attribute_count);
                    el.ns = str(ns_idx);
                    el.name = str(name_idx);
                    el.prefix = prefix_for(el.ns);
                    ++element_count_;

                    const size_t eff_size =
                        (attribute_size >= kAttributeSize) ? attribute_size : kAttributeSize;
                    const size_t attr_base =
                        node_start + kNodeHeaderSize + (attribute_start ? attribute_start
                                                                       : kAttrExtSize);
                    if (attr_base > node_end) {
                        err = "attribute block starts past the element node";
                        return false;
                    }
                    if (static_cast<uint64_t>(attribute_count) * eff_size >
                        static_cast<uint64_t>(node_end - attr_base)) {
                        err = "attribute block exceeds the element node";
                        return false;
                    }
                    el.attributes.reserve(attribute_count);
                    for (uint16_t i = 0; i < attribute_count; ++i) {
                        const size_t a = attr_base + static_cast<size_t>(i) * eff_size;
                        if (!in_range(d_, a, kAttributeSize) || a + kAttributeSize > node_end) {
                            err = "attribute lies outside its element node";
                            return false;
                        }
                        uint32_t a_ns = 0, a_name = 0, a_raw = 0;
                        uint16_t tv_size = 0;
                        uint8_t tv_res0 = 0, tv_type = 0;
                        uint32_t tv_data = 0;
                        read_u32(d_, a, a_ns);
                        read_u32(d_, a + 4, a_name);
                        read_u32(d_, a + 8, a_raw);
                        read_u16(d_, a + 12, tv_size);
                        read_u8(d_, a + 14, tv_res0);
                        read_u8(d_, a + 15, tv_type);
                        read_u32(d_, a + 16, tv_data);
                        (void)tv_size;
                        (void)tv_res0;

                        Attribute attr;
                        attr.ns = str(a_ns);
                        attr.name = str(a_name);
                        attr.prefix = prefix_for(attr.ns);
                        if (a_name != kNoIndex && a_name < res_ids_.size()) {
                            attr.res_id = res_ids_[a_name];
                        }
                        attr.value = decode_value(strings_, tv_type, tv_data);
                        if (a_raw != kNoIndex) attr.value.raw = str(a_raw);
                        el.attributes.push_back(std::move(attr));
                    }

                    out.push_back(std::move(el));
                    node_pos_ = node_end;
                    if (!parse_nodes(out.back().children, depth + 1, err)) return false;
                    // parse_nodes consumed the matching end-element chunk (and
                    // possibly following nodes at this level).
                    continue;
                }
                case kResXmlEndElement: {
                    if (node_end - node_start < kNodeHeaderSize + kElementExtSize) {
                        err = "truncated end-element node";
                        return false;
                    }
                    node_pos_ = node_end;
                    return true;
                }
                default:
                    // CDATA, unknown chunk types: skip by size.
                    break;
            }
            node_pos_ = node_end;
            if (node_pos_ <= node_start) {  // size 0 would loop forever
                err = "zero-sized chunk";
                return false;
            }
        }
    }

    std::span<const uint8_t> d_;
    Document& doc_;
    size_t xml_end_ = 0;
    size_t node_pos_ = 0;
    bool pool_done_ = false;
    size_t element_count_ = 0;
    std::vector<std::string> strings_;
    std::vector<uint32_t> res_ids_;
    std::vector<Namespace> ns_stack_;  // currently open xmlns declarations
};

// ---------------------------------------------------------------------------
// text dump helpers
// ---------------------------------------------------------------------------
bool value_is_quoted(ValueType t) { return t == ValueType::String; }

std::string render_value(const Value& v) {
    std::string s = v.text;
    if (value_is_quoted(v.type)) {
        s.insert(s.begin(), '"');
        s.push_back('"');
    }
    return s;
}

void append_element_text(std::string& out, const Element& e, int depth) {
    out.append(static_cast<size_t>(depth) * 2, ' ');
    out += "E: ";
    out += e.name;
    // aapt2 always appends the recorded line number, even "(line=0)" when the
    // compiler did not record one.
    out += " (line=";
    out += std::to_string(e.line);
    out += ')';
    out += '\n';

    const size_t attr_indent = static_cast<size_t>(depth) * 2 + 2;
    for (const Attribute& a : e.attributes) {
        out.append(attr_indent, ' ');
        out += "A: ";
        if (!a.ns.empty()) {
            out += a.ns;
            out += ':';
        }
        out += a.name;
        if (a.res_id != 0) {
            out += '(';
            out += hex8(a.res_id);
            out += ')';
        }
        out += '=';
        out += render_value(a.value);
        if (!a.value.raw.empty()) {
            out += " (Raw: \"";
            out += a.value.raw;
            out += "\")";
        }
        out += '\n';
    }
    for (const Element& c : e.children) append_element_text(out, c, depth + 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
const char* value_type_name(ValueType type) {
    switch (type) {
        case ValueType::Null: return "Null";
        case ValueType::Reference: return "Reference";
        case ValueType::Attribute: return "Attribute";
        case ValueType::String: return "String";
        case ValueType::Float: return "Float";
        case ValueType::Dimension: return "Dimension";
        case ValueType::Fraction: return "Fraction";
        case ValueType::DynamicReference: return "DynamicReference";
        case ValueType::IntDec: return "IntDec";
        case ValueType::IntHex: return "IntHex";
        case ValueType::IntBoolean: return "IntBoolean";
        case ValueType::ColorArgb8: return "ColorArgb8";
        case ValueType::ColorRgb8: return "ColorRgb8";
        case ValueType::ColorArgb4: return "ColorArgb4";
        case ValueType::ColorRgb4: return "ColorRgb4";
    }
    return "Unknown";
}

const Attribute* Element::attribute(std::string_view attr_name) const {
    for (const Attribute& a : attributes) {
        if (a.name == attr_name) return &a;
    }
    return nullptr;
}

const Attribute* Element::attribute(std::string_view attr_name, std::string_view ns_uri) const {
    for (const Attribute& a : attributes) {
        if (a.name == attr_name && a.ns == ns_uri) return &a;
    }
    return nullptr;
}

const Attribute* Element::attribute_by_res(uint32_t res_id) const {
    if (res_id == 0) return nullptr;
    for (const Attribute& a : attributes) {
        if (a.res_id == res_id) return &a;
    }
    return nullptr;
}

const Element* Element::child(std::string_view child_name) const {
    for (const Element& c : children) {
        if (c.name == child_name) return &c;
    }
    return nullptr;
}

std::vector<const Element*> Element::children_named(std::string_view child_name) const {
    std::vector<const Element*> out;
    for (const Element& c : children) {
        if (c.name == child_name) out.push_back(&c);
    }
    return out;
}

const Element* Element::find(std::string_view wanted) const {
    if (this->name == wanted) return this;
    for (const Element& c : children) {
        if (const Element* found = c.find(wanted)) return found;
    }
    return nullptr;
}

std::string Element::attr_text(std::string_view attr_name) const {
    const Attribute* a = attribute(attr_name);
    return a ? a->value.text : std::string();
}

int64_t Element::attr_int(std::string_view attr_name, int64_t fallback) const {
    const Attribute* a = attribute(attr_name);
    if (!a) return fallback;
    const std::string& s = a->value.text;
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = (s[i] == '-');
        ++i;
    }
    int base = 10;
    if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i >= s.size()) return fallback;
    int64_t value = 0;
    bool any = false;
    for (; i < s.size(); ++i) {
        int digit;
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            break;
        }
        if (value > (std::numeric_limits<int64_t>::max() - digit) / base) {
            return fallback;  // overflow
        }
        value = value * base + digit;
        any = true;
    }
    if (!any) return fallback;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i != s.size()) return fallback;  // trailing junk
    return negative ? -value : value;
}

Result parse(std::span<const uint8_t> data) {
    Result result;
    if (!looks_like_axml(data)) {
        result.ok = false;
        result.error = "buffer does not look like compiled Android XML";
        return result;
    }
    Parser parser(data, result.doc);
    std::string err;
    if (!parser.run(err)) {
        result.ok = false;
        result.error = err.empty() ? std::string("malformed compiled XML") : err;
        result.doc = Document{};
        return result;
    }
    result.ok = true;
    return result;
}

bool looks_like_axml(std::span<const uint8_t> data) {
    if (data.size() < 8) return false;
    const uint16_t type = static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
    if (type != kResXmlType) return false;
    const uint32_t size = static_cast<uint32_t>(data[4]) |
                          (static_cast<uint32_t>(data[5]) << 8) |
                          (static_cast<uint32_t>(data[6]) << 16) |
                          (static_cast<uint32_t>(data[7]) << 24);
    return size >= 8 && static_cast<size_t>(size) <= data.size();
}

std::string to_text(const Document& doc) {
    std::string out;
    for (const Namespace& ns : doc.namespaces) {
        out += "N: ";
        out += ns.prefix;
        out += '=';
        out += ns.uri;
        out += '\n';
    }
    for (const Element& e : doc.roots) append_element_text(out, e, 1);
    return out;
}

}  // namespace apkinfo::axml
