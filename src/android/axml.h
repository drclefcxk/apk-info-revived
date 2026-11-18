// Binary Android XML (AXML) parser.
//
// Android stores AndroidManifest.xml, res/**/*.xml (layout/drawable/adaptive
// icon resources) in a compiled binary form: a RES_XML_TYPE chunk wrapping a
// string pool, an (optional) resource-id map, and a stream of nodes
// (start namespace / start element / end element ...).
//
// This module turns such a buffer into a plain tree. It is deliberately
// allocation-simple and dependency-free (no zip, no android headers) so it can
// be unit tested on a raw *.xml extracted from an APK.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apkinfo::axml {

// Res_value::dataType values that can appear in a compiled XML attribute.
enum class ValueType : uint8_t {
    Null = 0x00,
    Reference = 0x01,         // data = resource id (@0x7f...)
    Attribute = 0x02,         // theme attribute reference
    String = 0x03,            // data = index into the XML string pool
    Float = 0x04,
    Dimension = 0x05,
    Fraction = 0x06,
    DynamicReference = 0x07,  // like Reference but may be resolved against the
                              // *package being loaded* (e.g. @*android:drawable/x)
    IntDec = 0x10,
    IntHex = 0x11,
    IntBoolean = 0x12,
    ColorArgb8 = 0x1c,
    ColorRgb8 = 0x1d,
    ColorArgb4 = 0x1e,
    ColorRgb4 = 0x1f,
};

const char* value_type_name(ValueType type);

// One typed value as stored in the XML.
struct Value {
    ValueType type = ValueType::Null;
    // Raw 32-bit payload as stored in Res_value.data. For Reference this is a
    // resource id; for String it is an index into the XML string pool.
    uint32_t data = 0;
    // Decoded text:
    //  * type == String   -> the resolved pooled string
    //  * numeric types    -> decimal representation ("26", "-1")
    //  * booleans         -> "true"/"false"
    //  * Reference        -> "@0x7f0f0001" (0x%08x form; 0x0100... ids keep
    //                        their leading zeroes)
    //  * anything else    -> empty
    std::string text;
    // Raw string from the attribute's "rawValue" index, when present. Many
    // typed attributes (e.g. android:versionName="@string/x") carry a raw value
    // that is the *unresolved* text; for plain strings it equals `text`.
    std::string raw;
};

struct Attribute {
    std::string ns;      // namespace URI, "" when none
    std::string prefix;  // "android" / "app" / "" when none
    std::string name;    // local name, e.g. "icon"
    uint32_t res_id = 0; // android attribute resource id (0x0101021c for versionName), 0 if unknown
    Value value;
};

struct Element {
    std::string ns;
    std::string prefix;
    std::string name;
    std::vector<Attribute> attributes;
    std::vector<Element> children;
    int line = 0;  // source line number as recorded by the compiler, 0 if unknown

    // Attribute lookup by local name (namespace-insensitive: an attribute named
    // "icon" matches whether or not it is android-namespaced). Empty view when
    // not found.
    const Attribute* attribute(std::string_view name) const;
    // Attribute lookup by local name restricted to a namespace URI.
    const Attribute* attribute(std::string_view name, std::string_view ns) const;
    // Attribute lookup by android resource id (0x0101xxxx).
    const Attribute* attribute_by_res(uint32_t res_id) const;
    // First direct child with this element name, or nullptr.
    const Element* child(std::string_view name) const;
    // All direct children with this element name.
    std::vector<const Element*> children_named(std::string_view name) const;
    // Depth-first search for the first descendant with this name (self
    // included). Returns nullptr when absent.
    const Element* find(std::string_view name) const;
    // Convenience: text of an attribute, or "" when missing.
    std::string attr_text(std::string_view name) const;
    // Convenience: integer of an attribute, `fallback` when missing/not numeric.
    int64_t attr_int(std::string_view name, int64_t fallback = 0) const;
};

struct Namespace {
    std::string prefix;
    std::string uri;
};

struct Document {
    std::vector<Element> roots;          // normally exactly one
    std::vector<Namespace> namespaces;   // all xmlns declarations seen
    const Element* root() const { return roots.empty() ? nullptr : &roots.front(); }
};

struct Result {
    bool ok = false;
    std::string error;   // empty on success
    Document doc;
};

// Parse a compiled (binary) XML buffer. `data` must outlive nothing: the
// returned Document owns copies of every string. Never throws.
Result parse(std::span<const uint8_t> data);

// Cheap magic check: does this buffer look like a compiled binary XML?
bool looks_like_axml(std::span<const uint8_t> data);

// Serialize a parsed document back to (debug) text form, "aapt xmltree" style:
//   E: manifest
//     A: android:versionCode(0x0101021b)=1500020300
//   N: android=http://schemas.android.com/apk/res/android
// Only used by tests / a --dump mode.
std::string to_text(const Document& doc);

}  // namespace apkinfo::axml
