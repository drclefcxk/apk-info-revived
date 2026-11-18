// resources.arsc (Android resource table) parser.
//
// An APK ships a compiled resource table mapping resource ids (0xPPTTEEEE) to
// values. Every resource id can have many entries, one per "configuration"
// (locale, density, screen size, ...). This module parses the table and
// resolves ids to values, which is what makes modern APKs readable: apps built
// with R8/AGP resource shrinking + obfuscation rename files to res/s31.xml, so
// the human-readable name of a resource only exists inside this table.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apkinfo::arsc {

// A resource configuration (the qualifiers on a res/ directory, e.g.
// "drawable-xxhdpi-v21" -> density=640, sdk_version=21).
struct Config {
    uint32_t density = 0;          // 0 == DENSITY_DEFAULT/ANY, e.g. 160, 240, 320, 480, 640, 0xFFFE (anydpi), 0xFFFF (nodpi)
    uint32_t sdk_version = 0;
    std::string language;          // "en", "zh", ... "" when unset
    std::string region;            // "US", "CN", ... "" when unset
    uint16_t mcc = 0;
    uint16_t mnc = 0;
    uint32_t screen_width_dp = 0;
    uint32_t screen_height_dp = 0;
    uint16_t smallest_width_dp = 0;
    uint8_t orientation = 0;       // 1=port 2=land 3=square
    uint8_t ui_mode_type = 0;      // 0=none 2=car 3=desk 4=television 5=appliance 6=watch 7=vrheadset
    uint8_t night_mode = 0;        // 1=notnight 2=night
    uint8_t grammar_gender = 0;

    // Human readable qualifier list, "" when the config is the default one.
    // Example: "en-rUS", "xxhdpi", "480dpi, v21".
    std::string to_string() const;
    bool is_default() const;

    // True when this config selects a specific screen density (not 0/anydpi/nodpi handling left to caller)
    bool has_density() const { return density != 0; }
};

// Res_value data types we care about (subset of android's).
enum class ValueType : uint8_t {
    Null = 0x00,
    Reference = 0x01,
    Attribute = 0x02,
    String = 0x03,
    Float = 0x04,
    Dimension = 0x05,
    Fraction = 0x06,
    DynamicReference = 0x07,
    IntDec = 0x10,
    IntHex = 0x11,
    IntBoolean = 0x12,
    ColorArgb8 = 0x1c,
    ColorRgb8 = 0x1d,
    ColorArgb4 = 0x1e,
    ColorRgb4 = 0x1f,
};

struct BagEntry {
    uint32_t key = 0;    // attr resource id
    uint32_t value = 0;  // raw data (usually a resource id or a string pool index)
    ValueType type = ValueType::Null;
    std::optional<std::string> string;  // when type == String
};

// One table entry for one configuration.
struct Entry {
    Config config;
    bool is_complex = false;              // bag / map (style, array, plurals ...)
    ValueType type = ValueType::Null;      // simple entry type
    uint32_t data = 0;                     // simple entry data
    std::optional<std::string> string;     // set when type == String
    std::vector<BagEntry> bag;             // set when is_complex
    uint32_t parent = 0;                   // bag parent resource id (0 = none)
};

// A fully resolved value: references followed to the final concrete value.
struct ResolvedValue {
    ValueType type = ValueType::Null;
    std::optional<std::string> string;   // for String values (both @string and file paths like "res/s31.xml")
    uint32_t data = 0;                   // raw data for numeric/dimension/color
    std::optional<uint32_t> redirected_from;  // when the id was an alias, the target id
    Config config;                       // configuration of the entry that produced it
    // True when the value is a resource id that could not be resolved (e.g. a
    // framework resource 0x01xxxxxx).
    bool unresolved_reference = false;
    bool is_complex = false;             // resolved to a bag (style/array/...)
    std::vector<BagEntry> bag;
};

// Preference used while picking between configurations of the same resource.
struct ConfigPreference {
    uint32_t density = 0;        // desired density, e.g. 640; 0 = pick the
                                 // "best default" (prefer 0xFFFE/0xFFFF then the
                                 // largest density)
    std::string language;        // desired locale, "" = default only
    std::string region;
    uint32_t sdk_version = 0;    // device sdk, 0 = ignore (highest wins)
    bool prefer_default = true;  // prefer the config-less entry when ambiguous
};

class Table {
public:
    // Parse a resources.arsc buffer. Returns nullopt on malformed input and
    // fills `error`.
    static std::optional<Table> parse(std::span<const uint8_t> data, std::string* error = nullptr);

    Table();
    ~Table();
    Table(Table&&) noexcept;
    Table& operator=(Table&&) noexcept;
    Table(const Table&);
    Table& operator=(const Table&);

    bool valid() const;
    const std::string& package_name() const;

    // Does this table know the id at all?
    bool has(uint32_t res_id) const;

    // All raw entries for a resource id (one per configuration), in table order.
    std::vector<Entry> entries(uint32_t res_id) const;

    // Best entry for the id under a config preference. References are NOT
    // followed here (see resolve()).
    std::optional<Entry> best_entry(uint32_t res_id, const ConfigPreference& pref = {}) const;

    // Resolve a resource id to a concrete value, following reference /
    // dynamic-reference chains. Returns nullopt when the id is unknown.
    std::optional<ResolvedValue> resolve(uint32_t res_id, const ConfigPreference& pref = {}) const;

    // Convenience wrappers: resolve and return the string payload, if any.
    // This is what turns "@0x7f0f0007" into e.g. "res/s31.xml" (a file path
    // inside the apk) or "@0x7f0e0001" into "Lawnchair".
    std::optional<std::string> resolve_string(uint32_t res_id, const ConfigPreference& pref = {}) const;

    // Convenience: resolve to an integer (IntDec/IntHex/IntBoolean), following
    // references. nullopt when absent or not numeric.
    std::optional<int64_t> resolve_int(uint32_t res_id, const ConfigPreference& pref = {}) const;

    // Debug: dump the whole table in a readable form (package/type/entry ids,
    // configs, values). Used by tests and a --dump CLI mode.
    std::string dump() const;

    // Every distinct configuration that occurs anywhere in this table, in table
    // order and without duplicates (compare on the fields of Config). Used to
    // show "screen densities" and "supported locales" for packages whose res/
    // file names are obfuscated (a shrunk APK has no res/values-de/ or
    // res/mipmap-xxhdpi/ directory names to scan).
    std::vector<Config> configurations() const;

    struct Impl;

private:
    Impl* impl_ = nullptr;
};

// Does this buffer start like a resource table?
bool looks_like_arsc(std::span<const uint8_t> data);

}  // namespace apkinfo::arsc
