// Test harness for apkinfo::arsc -- lives OUTSIDE ApkInfo/src on purpose.
//
// Verifies the Lawnchair expectations, cross-checks every id against
// `aapt2 dump resources` ground truth, times the GMS table and sanity-resolves
// thousands of random ids.
#include "arsc.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace apkinfo::arsc;

static int g_fail = 0;
static int g_pass = 0;

static void check(bool ok, const std::string& what, const std::string& detail = {}) {
    if (ok) {
        ++g_pass;
        std::printf("  ok   %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("  FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : " -> ", detail.c_str());
    }
}

static std::vector<uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("cannot open %s\n", path);
        std::exit(2);
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// "480dpi, v21" -> "480dpi-v21" so it can be compared with aapt2's join style.
static std::string norm_cfg(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ',' && i + 1 < s.size() && s[i + 1] == ' ') {
            out += '-';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Rewrites aapt2 qualifier strings into what struct Config can represent:
//   b+sr+Latn -> sr        (script subtag dropped, struct has no script field)
//   b+es+419  -> es-r419   (numeric region kept, it is a language+region pair)
// then drops the qualifiers Config simply has no field for (layout direction,
// screen size/long, input devices, colour mode), so the result can be compared
// one to one with Config::to_string().
static std::string strip_unrepresentable(const std::string& cfg) {
    static const std::set<std::string> kDrop = {
        "ldltr",   "ldrtl",     "small",    "normal",   "large",    "xlarge",
        "long",    "notlong",   "finger",   "trackball", "stylus",  "nokeys",
        "qwerty",  "12key",     "navexposed", "navhidden", "nonav", "dpad",
        "notouch", "keysexposed", "widecg", "nowidecg",  "highdr",  "lowdr",
        "round",   "notround"};
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i <= cfg.size()) {
        size_t j = cfg.find('-', i);
        if (j == std::string::npos) j = cfg.size();
        const std::string tok = cfg.substr(i, j - i);
        if (!tok.empty()) tokens.push_back(tok);
        if (j == cfg.size()) break;
        i = j + 1;
    }
    std::string out;
    for (size_t k = 0; k < tokens.size(); ++k) {
        std::string tok = tokens[k];
        if (tok.rfind("b+", 0) == 0) {
            // BCP47: b+<lang>[+<script>][+<region>][+<variant>]
            std::vector<std::string> sub;
            size_t p = 0;
            while (p <= tok.size()) {
                size_t q = tok.find('+', p);
                if (q == std::string::npos) q = tok.size();
                sub.push_back(tok.substr(p, q - p));
                if (q == tok.size()) break;
                p = q + 1;
            }
            if (sub.size() < 2) continue;
            tok = sub[1];
            for (size_t s = 2; s < sub.size(); ++s) {
                const bool is_region = sub[s].size() == 2 || (sub[s].size() == 3 && sub[s].find_first_not_of("0123456789") == std::string::npos);
                if (is_region) {
                    tok += "-r" + sub[s];
                    break;
                }
            }
        }
        if (kDrop.count(tok) != 0) continue;
        // "v36.1": the minor sdk version has no field in struct Config.
        if (tok.size() > 2 && tok[0] == 'v' && tok.find('.') != std::string::npos) tok = tok.substr(0, tok.find('.'));
        if (!out.empty()) out += '-';
        out += tok;
    }
    return out;
}

struct AaptLine {
    std::string config;
    std::string rest;
};

static std::multimap<uint32_t, AaptLine> load_dump(const char* path) {
    std::multimap<uint32_t, AaptLine> out;
    std::ifstream f(path);
    std::string line;
    uint32_t cur = 0;
    bool have = false;
    while (std::getline(f, line)) {
        if (line.rfind("    resource 0x", 0) == 0 && line.size() > 16) {
            cur = static_cast<uint32_t>(std::stoul(line.substr(15, 8), nullptr, 16));
            have = true;
        } else if (have && line.rfind("      (", 0) == 0) {
            const size_t close = line.find(')', 7);
            if (close == std::string::npos) continue;
            AaptLine l;
            l.config = line.substr(7, close - 7);
            // aapt2 wraps long values onto the next line(s); such a continuation
            // looks like a config line, so drop anything with a space in it
            // (real qualifier lists never contain spaces).
            if (l.config.find(' ') != std::string::npos) continue;
            l.rest = close + 2 <= line.size() ? line.substr(close + 2) : std::string();
            out.emplace(cur, l);
        }
    }
    return out;
}

// Counts how many type chunks use each offset encoding (independent probe of
// the raw bytes, to show what the parser had to deal with).
static void probe_encodings(const std::vector<uint8_t>& bytes, const std::string& label) {
    auto u16 = [&](size_t o) -> uint32_t {
        return o + 1 < bytes.size() ? (bytes[o] | (bytes[o + 1] << 8)) : 0u;
    };
    auto u32 = [&](size_t o) -> uint32_t {
        return o + 3 < bytes.size()
                   ? (static_cast<uint32_t>(bytes[o]) | (static_cast<uint32_t>(bytes[o + 1]) << 8) |
                      (static_cast<uint32_t>(bytes[o + 2]) << 16) | (static_cast<uint32_t>(bytes[o + 3]) << 24))
                   : 0u;
    };
    size_t pos = 12;
    int plain = 0, sparse = 0, off16 = 0, compact = 0, chunks = 0;
    while (pos + 8 <= bytes.size()) {
        const uint32_t csize = u32(pos + 4);
        if (csize < 8) break;
        if (u16(pos) == 0x0200) {  // package: descend
            const uint32_t hsz = u16(pos + 2);
            size_t sub = pos + hsz;
            const size_t end = std::min(bytes.size(), pos + csize);
            while (sub + 8 <= end) {
                const uint32_t ssize = u32(sub + 4);
                if (ssize < 8) break;
                if (u16(sub) == 0x0201) {
                    ++chunks;
                    const uint8_t flags = bytes[sub + 9];
                    if (flags & 0x01) ++sparse;
                    else if (flags & 0x02) ++off16;
                    else ++plain;
                    // walk entries to count FLAG_COMPACT
                    const uint32_t count = u32(sub + 12);
                    const uint32_t estart = u32(sub + 16);
                    const uint32_t hs = u16(sub + 2);
                    if (!(flags & 0x01) && count < 200000) {
                        for (uint32_t i = 0; i < count; ++i) {
                            uint32_t eo = 0;
                            if (flags & 0x02) {
                                const uint32_t v = u16(sub + hs + i * 2);
                                if (v == 0xFFFF) continue;
                                eo = estart + v * 4;
                            } else {
                                const uint32_t v = u32(sub + hs + i * 4);
                                if (v == 0xFFFFFFFFu) continue;
                                eo = estart + v;
                            }
                            const size_t at = static_cast<size_t>(sub) + eo;
                            if (at + 4 > bytes.size()) continue;
                            if ((u32(at) >> 16) & 0x0008) ++compact;
                        }
                    }
                }
                sub += ssize;
            }
        }
        pos += csize;
    }
    std::printf("  [%s] type chunks=%d  uint32=%d sparse=%d offset16=%d  compact entries=%d\n",
                label.c_str(), chunks, plain, sparse, off16, compact);
}

struct CrossStats {
    int ids = 0;
    int cfg_mismatch = 0;
    int cfg_fold_only = 0;   // equal once non-representable qualifiers are dropped
    int values_checked = 0;
    int values_bad = 0;
    int values_skipped = 0;
    std::vector<std::string> examples;
    std::vector<std::string> value_examples;
};

static CrossStats cross_check(const Table& t, const char* dump_path) {
    CrossStats st;
    const auto truth = load_dump(dump_path);
    std::set<uint32_t> ids;
    for (const auto& kv : truth) ids.insert(kv.first);

    for (const uint32_t id : ids) {
        ++st.ids;
        const std::vector<Entry> mine = t.entries(id);
        std::multiset<std::string> mine_cfg;
        std::map<std::string, const Entry*> by_cfg;
        for (const Entry& e : mine) {
            const std::string c = norm_cfg(e.config.to_string());
            mine_cfg.insert(c);
            by_cfg.emplace(c, &e);
        }
        std::multiset<std::string> truth_raw, truth_stripped;
        auto range = truth.equal_range(id);
        for (auto it = range.first; it != range.second; ++it) {
            truth_raw.insert(it->second.config);
            truth_stripped.insert(strip_unrepresentable(it->second.config));
        }

        if (mine_cfg != truth_raw) {
            if (mine_cfg == truth_stripped) {
                ++st.cfg_fold_only;
            } else {
                ++st.cfg_mismatch;
                if (st.examples.size() < 12) {
                    std::string ex = "id 0x";
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "%08x", id);
                    ex += buf;
                    ex += " mine=[";
                    for (const auto& c : mine_cfg) ex += c + " ";
                    ex += "] aapt2=[";
                    for (const auto& c : truth_raw) ex += c + " ";
                    ex += "]";
                    st.examples.push_back(ex);
                }
            }
        }

        for (auto it = range.first; it != range.second; ++it) {
            const std::string& rest = it->second.rest;
            const auto mit = by_cfg.find(it->second.config);
            if (mit == by_cfg.end()) {
                ++st.values_skipped;
                continue;
            }
            const Entry& e = *mit->second;
            std::string mine_text;
            if (e.is_complex) {
                mine_text = "bag:" + std::to_string(e.bag.size());
            } else if (e.string) {
                mine_text = "\"" + *e.string + "\"";
            } else {
                char buf[32];
                std::snprintf(buf, sizeof buf, "@0x%08x", e.data);
                mine_text = buf;
            }
            std::string expect;
            bool cmp = false;
            if (rest.rfind("(file) ", 0) == 0) {
                const size_t sp = rest.find(' ', 7);
                expect = "\"" + rest.substr(7, sp == std::string::npos ? std::string::npos : sp - 7) + "\"";
                cmp = true;
            } else if (rest.rfind("(style) size=", 0) == 0 || rest.rfind("(array) size=", 0) == 0 ||
                       rest.rfind("(plurals) size=", 0) == 0) {
                const size_t eq = rest.find("size=");
                const size_t sp = rest.find(' ', eq);
                expect = "bag:" + rest.substr(eq + 5, sp == std::string::npos ? std::string::npos : sp - eq - 5);
                cmp = true;
            } else if (!rest.empty() && rest[0] == '"' && rest.find('\\') == std::string::npos) {
                expect = rest;
                cmp = true;
            } else if (rest.rfind("@0x", 0) == 0) {
                expect = rest;
                cmp = true;
            }
            if (!cmp) {
                ++st.values_skipped;
                continue;
            }
            ++st.values_checked;
            // aapt2 prints a string's embedded newlines verbatim, so a multi
            // line string only reaches us up to its first line: compare prefixes.
            const bool val_ok = (!expect.empty() && expect[0] == '"')
                                    ? (mine_text.rfind(expect, 0) == 0 &&
                                       (mine_text.size() == expect.size() || mine_text[expect.size()] == '\n'))
                                    : (mine_text == expect);
            if (!val_ok) {
                ++st.values_bad;
                if (st.value_examples.size() < 12) {
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "%08x", id);
                    std::string ex = std::string("value id 0x") + buf + " cfg (" + it->second.config + ")";
                    ex += " mine=" + mine_text + " aapt2=" + expect;
                    st.value_examples.push_back(ex);
                }
            }
        }
    }
    return st;
}

// ---------------------------------------------------------------------------
// Hand built 744 byte resource table: exercises the encodings the real
// fixtures do not use (FLAG_OFFSET16, FLAG_SPARSE, FLAG_COMPACT entries), plus
// references, bags and multi config selection.
// ---------------------------------------------------------------------------
namespace synth {

struct W {
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v)); }
    void u16(uint32_t v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16((v >> 16) & 0xFFFF); }
    void u16s(const char* s) { for (; *s; ++s) u16(static_cast<uint8_t>(*s)); }
    size_t size() const { return b.size(); }
    void pad_to(size_t n) { while (b.size() < n) u8(0); }
};

std::vector<uint8_t> utf8_pool(const std::vector<std::string>& strs) {
    W w;
    std::vector<uint8_t> data;
    std::vector<uint32_t> offs;
    for (const auto& s : strs) {
        offs.push_back(static_cast<uint32_t>(data.size()));
        data.push_back(static_cast<uint8_t>(s.size()));
        data.push_back(static_cast<uint8_t>(s.size()));
        for (char c : s) data.push_back(static_cast<uint8_t>(c));
        data.push_back(0);
    }
    const uint32_t strings_start = 28 + static_cast<uint32_t>(offs.size()) * 4;
    w.u16(0x0001); w.u16(28); w.u32(strings_start + static_cast<uint32_t>(data.size()));
    w.u32(static_cast<uint32_t>(strs.size())); w.u32(0); w.u32(0x100); w.u32(strings_start); w.u32(0);
    for (const uint32_t o : offs) w.u32(o);
    w.b.insert(w.b.end(), data.begin(), data.end());
    return w.b;
}

std::vector<uint8_t> utf16_pool(const std::vector<std::string>& strs) {
    W w;
    std::vector<uint8_t> data;
    std::vector<uint32_t> offs;
    for (const auto& s : strs) {
        offs.push_back(static_cast<uint32_t>(data.size()));
        W e;
        e.u16(static_cast<uint32_t>(s.size()));
        e.u16s(s.c_str());
        e.u16(0);
        data.insert(data.end(), e.b.begin(), e.b.end());
    }
    const uint32_t strings_start = 28 + static_cast<uint32_t>(offs.size()) * 4;
    w.u16(0x0001); w.u16(28); w.u32(strings_start + static_cast<uint32_t>(data.size()));
    w.u32(static_cast<uint32_t>(strs.size())); w.u32(0); w.u32(0); w.u32(strings_start); w.u32(0);
    for (const uint32_t o : offs) w.u32(o);
    w.b.insert(w.b.end(), data.begin(), data.end());
    return w.b;
}

std::vector<uint8_t> simple_entry(uint8_t data_type, uint32_t data) {
    W w;
    w.u16(8); w.u16(0); w.u32(0);              // ResTable_entry (size, flags, key)
    w.u16(8); w.u8(0); w.u8(data_type); w.u32(data);  // Res_value
    return w.b;
}

std::vector<uint8_t> compact_entry(uint8_t data_type, uint32_t data) {
    W w;
    w.u32((static_cast<uint32_t>(data_type) << 24) | (0x0008u << 16));  // key=0, FLAG_COMPACT
    w.u32(data);
    return w.b;
}

std::vector<uint8_t> bag_entry(uint32_t parent, const std::vector<std::pair<uint32_t, std::pair<uint8_t, uint32_t>>>& maps) {
    W w;
    w.u16(16); w.u16(0x0001); w.u32(0);  // entry header, FLAG_COMPLEX
    w.u32(parent); w.u32(static_cast<uint32_t>(maps.size()));
    for (const auto& m : maps) {
        w.u32(m.first);
        w.u16(8); w.u8(0); w.u8(m.second.first); w.u32(m.second.second);
    }
    return w.b;
}

// offsets: raw offset-table words (per flags encoding), entries: concatenated
// entry bytes.
std::vector<uint8_t> type_chunk(uint8_t type_id, uint8_t flags, uint32_t entry_count, uint32_t density,
                                uint32_t sdk, const std::vector<uint32_t>& offsets,
                                const std::vector<uint8_t>& entries) {
    const uint32_t cfg_size = 64;
    const uint32_t hsz = 20 + cfg_size;
    uint32_t off_bytes = entry_count * 4;
    if ((flags & 0x01) != 0 || (flags & 0x02) != 0) off_bytes = entry_count * ((flags & 0x02) ? 2 : 4);
    uint32_t entries_start = (hsz + off_bytes + 3) & ~3u;

    W w;
    w.u16(0x0201); w.u16(hsz); w.u32(entries_start + static_cast<uint32_t>(entries.size()));
    w.u8(type_id); w.u8(flags); w.u16(0);
    w.u32(entry_count); w.u32(entries_start);
    w.u32(cfg_size);
    w.pad_to(20 + 14); w.u16(density);   // ResTable_config.density
    w.pad_to(20 + 24); w.u16(sdk);       // ResTable_config.sdkVersion
    w.pad_to(hsz);
    for (const uint32_t o : offsets) {
        if ((flags & 0x02) != 0) {
            w.u16(o);
        } else if ((flags & 0x01) != 0) {
            w.u32(o);  // packed as {uint16 idx, uint16 offset/4}
        } else {
            w.u32(o);
        }
    }
    w.pad_to(entries_start);
    w.b.insert(w.b.end(), entries.begin(), entries.end());
    return w.b;
}

std::vector<uint8_t> build() {
    const auto gpool = utf8_pool({"res/x.xml", "Hello", "V21"});
    const auto tpool = utf16_pool({"string"});
    const auto kpool = utf8_pool({"k"});

    std::vector<std::vector<uint8_t>> chunks;
    // type 1 (string): default, 480dpi, v21
    chunks.push_back(type_chunk(1, 0, 1, 0, 0, {0}, simple_entry(0x03, 0)));
    chunks.push_back(type_chunk(1, 0, 1, 480, 0, {0}, simple_entry(0x03, 1)));
    chunks.push_back(type_chunk(1, 0, 1, 0, 21, {0}, simple_entry(0x03, 2)));
    // type 2: FLAG_OFFSET16, entry only at index 1, and it is a COMPACT entry
    chunks.push_back(type_chunk(2, 0x02, 3, 0, 0, {0xFFFF, 0, 0xFFFF}, compact_entry(0x10, 42)));
    // type 3: FLAG_SPARSE, single entry at index 5, a reference to 0x7f010000
    chunks.push_back(type_chunk(3, 0x01, 1, 0, 0, {5u | (0u << 16)}, simple_entry(0x01, 0x7f010000)));
    // type 4: a bag (style) with 2 maps
    chunks.push_back(type_chunk(4, 0, 1, 0, 0, {0},
                                bag_entry(0x01030010, {{0x01010001u, {uint8_t{0x03}, 1u}}, {0x01010002u, {uint8_t{0x10}, 7u}}})));
    // type 5: a reference to a framework resource that is not in this table
    chunks.push_back(type_chunk(5, 0, 1, 0, 0, {0}, simple_entry(0x01, 0x01040000)));

    size_t sub_total = tpool.size() + kpool.size();
    for (const auto& c : chunks) sub_total += c.size();

    W p;
    p.u16(0x0200); p.u16(284); p.u32(284 + static_cast<uint32_t>(sub_total));
    p.u32(0x7f);
    p.u16s("test.pkg");
    p.pad_to(268);
    p.u32(284);                                    // typeStrings
    p.u32(0);                                      // lastPublicType
    p.u32(284 + static_cast<uint32_t>(tpool.size()));  // keyStrings
    p.u32(0);                                      // lastPublicKey
    p.b.insert(p.b.end(), tpool.begin(), tpool.end());
    p.b.insert(p.b.end(), kpool.begin(), kpool.end());
    for (const auto& c : chunks) p.b.insert(p.b.end(), c.begin(), c.end());

    W t;
    t.u16(0x0002); t.u16(12); t.u32(12 + static_cast<uint32_t>(gpool.size() + p.size()));
    t.u32(1);
    t.b.insert(t.b.end(), gpool.begin(), gpool.end());
    t.b.insert(t.b.end(), p.b.begin(), p.b.end());
    return t.b;
}

}  // namespace synth

static void synth_test() {
    std::printf("== synthetic table (OFFSET16 / sparse / compact / refs) ==\n");
    const auto bytes = synth::build();
    std::printf("  table: %zu bytes\n", bytes.size());
    std::string err;
    auto t = Table::parse(bytes, &err);
    check(t.has_value(), "parse(synthetic) succeeds", err);
    if (!t) return;
    check(t->package_name() == "test.pkg", "synthetic package_name()", t->package_name());

    // With no sdk preference the highest sdk wins (ConfigPreference doc).
    check(t->resolve_string(0x7f010000).value_or("") == "V21", "no preference -> highest sdk variant",
          t->resolve_string(0x7f010000).value_or("<none>"));

    ConfigPreference d480;
    d480.density = 480;
    check(t->resolve_string(0x7f010000, d480).value_or("") == "Hello", "480dpi config picked over default/v21",
          t->resolve_string(0x7f010000, d480).value_or("<none>"));

    ConfigPreference v21;
    v21.sdk_version = 21;
    check(t->resolve_string(0x7f010000, v21).value_or("") == "V21", "v21 config picked with sdk 21",
          t->resolve_string(0x7f010000, v21).value_or("<none>"));
    ConfigPreference v19;
    v19.sdk_version = 19;
    check(t->resolve_string(0x7f010000, v19).value_or("") == "res/x.xml", "v21 ignored when sdk is 19",
          t->resolve_string(0x7f010000, v19).value_or("<none>"));

    const auto cfgs = t->entries(0x7f010000);
    std::printf("  entries(0x7f010000): %zu [%s]\n", cfgs.size(),
                cfgs.size() == 3 ? (cfgs[0].config.to_string() + "," + cfgs[1].config.to_string() + "," +
                                    cfgs[2].config.to_string()).c_str() : "");
    check(cfgs.size() == 3 && cfgs[1].config.to_string() == "xxhdpi" && cfgs[2].config.to_string() == "v21",
          "entries() lists 3 configs: \"\", \"480dpi\", \"v21\"");

    check(!t->has(0x7f020000) && !t->has(0x7f020002), "OFFSET16: NO_ENTRY slots are absent");
    check(t->has(0x7f020001), "OFFSET16: entry at index 1 is present");
    check(t->resolve_int(0x7f020001).value_or(-1) == 42, "COMPACT entry decodes to int 42",
          std::to_string(t->resolve_int(0x7f020001).value_or(-999)));

    check(!t->has(0x7f030004), "SPARSE: absent index is absent");
    {
        auto chain = t->resolve(0x7f030005);
        check(chain && chain->string.value_or("") == "V21" && chain->redirected_from.value_or(0) == 0x7f010000,
              "SPARSE entry resolves through its reference chain (redirected_from = 0x7f010000)",
              chain && chain->string ? *chain->string : "<none>");
    }

    auto bag = t->resolve(0x7f040000);
    check(bag && bag->is_complex && bag->bag.size() == 2, "bag entry has 2 maps");
    if (bag && bag->is_complex) {
        check(bag->bag[0].key == 0x01010001 && bag->bag[0].string.value_or("") == "Hello",
              "bag map 0 is a string value");
        check(bag->bag[1].key == 0x01010002 && bag->bag[1].value == 7, "bag map 1 is int 7");
    }

    auto fw = t->resolve(0x7f050000);
    check(fw && fw->unresolved_reference && fw->type == ValueType::Reference && fw->data == 0x01040000,
          "framework reference reported as unresolved_reference (not a failure)");
    check(!t->resolve(0x7f060000).has_value(), "unknown id resolves to nullopt");
    check(t->resolve(0x01040000).value_or(ResolvedValue()).type == ValueType::Null,
          "top-level framework id resolves to nullopt");

    {
        const auto synth_cfgs = t->configurations();
        std::string shown;
        for (const auto& c : synth_cfgs) shown += "[" + c.to_string() + "]";
        std::printf("  configurations(): %zu %s\n", synth_cfgs.size(), shown.c_str());
        check(synth_cfgs.size() == 3 && synth_cfgs[0].is_default() && synth_cfgs[1].has_density() && !synth_cfgs[2].has_density(),
              "configurations() dedupes to default + xxhdpi + v21");
    }
}


int main(int argc, char** argv) {
    synth_test();

    // argv[1] points at the prepared fixtures (tests/fixtures); a relative "fixtures"
    // keeps the test runnable from the test directory without any absolute path.
    const std::string base = argc > 1 ? argv[1] : "fixtures";

    // ------------------------------------------------------------------
    std::printf("== Lawnchair resources.arsc ==\n");
    const std::string lwn_path = base + "/lawnchair/resources.arsc";
    const auto lwn_bytes = read_file(lwn_path.c_str());
    std::printf("  file: %zu bytes\n", lwn_bytes.size());
    check(looks_like_arsc(lwn_bytes), "looks_like_arsc()");
    check(!looks_like_arsc(std::span<const uint8_t>()), "looks_like_arsc(empty) == false");

    std::string err = "x";
    auto lwn = Table::parse(lwn_bytes, &err);
    check(lwn.has_value(), "parse() succeeds", err);
    if (!lwn) return 1;
    check(lwn->valid(), "valid()");
    check(lwn->package_name() == "app.lawnchair", "package_name() == app.lawnchair", lwn->package_name());

    check(lwn->has(0x7f120133), "has(0x7f120133)");
    check(!lwn->has(0x0104000a), "has(framework id) == false");

    auto s1 = lwn->resolve_string(0x7f120133);
    check(s1 && *s1 == "Lawnchair", "resolve_string(0x7f120133) == \"Lawnchair\"", s1 ? *s1 : "<nullopt>");

    auto s2 = lwn->resolve_string(0x7f0802ce);
    check(s2 && *s2 == "res/s31.xml", "resolve_string(0x7f0802ce) == \"res/s31.xml\"", s2 ? *s2 : "<nullopt>");

    auto s3 = lwn->resolve_string(0x7f0f0001);
    check(s3 && s3->rfind("res/", 0) == 0 && (s3->find(".png") != std::string::npos || s3->find(".webp") != std::string::npos),
          "resolve_string(0x7f0f0001) is a drawable file path", s3 ? *s3 : "<nullopt>");
    auto s4 = lwn->resolve_string(0x7f0f0002);
    check(s4 && s4->rfind("res/", 0) == 0 && (s4->find(".png") != std::string::npos || s4->find(".webp") != std::string::npos),
          "resolve_string(0x7f0f0002) is a drawable file path", s4 ? *s4 : "<nullopt>");

    ConfigPreference xxhdpi;
    xxhdpi.density = 480;
    auto d480 = lwn->resolve_string(0x7f0f0002, xxhdpi);
    check(d480 && *d480 == "res/aZ.png", "0x7f0f0002 @480dpi == res/aZ.png (aapt2 says xxhdpi res/aZ.png)",
          d480 ? *d480 : "<nullopt>");

    // entries() of the app label: every config + value.
    {
        const auto es = lwn->entries(0x7f120133);
        std::printf("  entries(0x7f120133): %zu\n", es.size());
        for (const auto& e : es)
            std::printf("    (%s) = %s\n", e.config.to_string().c_str(),
                        e.string ? e.string->c_str() : "<no string>");
        check(!es.empty() && es.front().config.is_default(), "entries(0x7f120133) has the default config first");
    }
    {
        // Multi-locale entry: aapt2 lists () sr b+sr+Latn bs-rBA en-rCA ar-rSA uk-rUA en-rXA af-rZA ...
        const auto es = lwn->entries(0x7f120134);
        std::set<std::string> got;
        for (const auto& e : es) got.insert(norm_cfg(e.config.to_string()));
        const char* want[] = {"", "sr", "bs-rBA", "en-rCA", "ar-rSA", "uk-rUA", "en-rXA", "af-rZA", "zh-rCN", "pt-rBR"};
        int miss = 0;
        std::string missing;
        for (const char* w : want)
            if (!got.count(w)) {
                ++miss;
                missing += std::string("[") + w + "] ";
            }
        check(miss == 0, "entries(0x7f120134) configs match aapt2 (sr, bs-rBA, en-rCA, ar-rSA, en-rXA, af-rZA, ...)",
              missing);
        std::printf("  entries(0x7f120134): %zu configs, first 10:", es.size());
        for (size_t i = 0; i < es.size() && i < 10; ++i) std::printf(" [%s]", es[i].config.to_string().c_str());
        std::printf("\n");

        // resolve a few locale variants through the preference.
        ConfigPreference fr;
        fr.language = "fr";
        fr.region = "FR";
        auto frv = lwn->resolve_string(0x7f120134, fr);
        check(frv && *frv == "Tout désélectionner", "resolve_string(0x7f120134) with fr-FR pref",
              frv ? *frv : "<nullopt>");
    }

    // resolve_int / bags / dump sanity
    {
        auto any_style = lwn->resolve(0x7f130020);
        check(any_style.has_value(), "resolve(0x7f130020) (a style/bag)");
    }
    {
        ConfigPreference p;
        p.density = 640;
        p.sdk_version = 34;
        p.language = "en";
        p.region = "US";
        auto e = lwn->best_entry(0x7f120133, p);
        check(e.has_value(), "best_entry with full preference");
    }
    {
        const std::string dump = lwn->dump();
        check(dump.find("package app.lawnchair") == 0, "dump() starts with the package line");
        check(dump.find("0x7f120133") != std::string::npos, "dump() contains the app label id");
        check(dump.size() > 100000, "dump() is a full table dump (" + std::to_string(dump.size()) + " bytes)");
        std::printf("  dump() head:\n%s", dump.substr(0, 260).c_str());
    }

    probe_encodings(lwn_bytes, "lawnchair");

    // configurations(): the UI's "supported locales / densities" view.
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto cfgs = lwn->configurations();
        const auto t1 = std::chrono::steady_clock::now();
        std::set<std::string> keys, locs, dens;
        for (const auto& c : cfgs) {
            keys.insert(c.to_string());
            if (!c.language.empty()) locs.insert(c.language + (c.region.empty() ? "" : "-r" + c.region));
            if (c.has_density()) dens.insert(c.to_string());
        }
        std::printf("  configurations(): %zu distinct in %.2f ms\n", cfgs.size(),
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
        check(cfgs.size() == keys.size(), "configurations() has no duplicate config strings");
        check(!cfgs.empty() && cfgs.front().is_default(), "configurations() starts with the default config (table order)");
        std::string line;
        for (const auto& l : locs) line += l + " ";
        std::printf("  distinct locales (%zu):\n    %s\n", locs.size(), line.c_str());
        std::string dl;
        for (const auto& d : dens) dl += d + " ";
        std::printf("  configs with a density (%zu): %s\n", dens.size(), dl.c_str());
        std::string all;
        for (const auto& c : cfgs) all += (all.empty() ? "" : " | ") + c.to_string();
        std::printf("  all configs:\n    %s\n", all.c_str());
    }

    // Copy / move semantics.
    {
        std::optional<Table> copy;
        {
            Table tmp(*lwn);
            copy.emplace(std::move(tmp));
        }
        check(copy->valid() && copy->resolve_string(0x7f120133).value_or("") == "Lawnchair",
              "copy + move keep the table usable");
        Table deep(*lwn);
        lwn.reset();
        check(deep.resolve_string(0x7f0802ce).value_or("") == "res/s31.xml", "copy survives the original being freed");
        lwn = Table::parse(lwn_bytes, &err);
        check(lwn.has_value(), "re-parse for the remaining checks");
        if (!lwn) return 1;
    }

    const std::string lwn_dump = base + "/tests/arsc/lwn_dump.txt";
    {
        std::printf("== cross-check vs aapt2 (lawnchair) ==\n");
        const CrossStats st = cross_check(*lwn, lwn_dump.c_str());
        std::printf("  ids=%d  config-set mismatches=%d (exact-after-qualifier-fold=%d)\n", st.ids,
                    st.cfg_mismatch, st.cfg_fold_only);
        std::printf("  values compared=%d  bad=%d  skipped(non-comparable format)=%d\n", st.values_checked,
                    st.values_bad, st.values_skipped);
        check(st.cfg_mismatch == 0, "config sets identical to aapt2 for every id (qualifiers representable in Config)");
        check(st.values_bad == 0, "values identical to aapt2 for file refs / strings / references / bag sizes");
        for (const auto& ex : st.examples) std::printf("    cfg %s\n", ex.c_str());
        for (const auto& ex : st.value_examples) std::printf("    %s\n", ex.c_str());
    }

    // ------------------------------------------------------------------
    std::printf("== GMS resources.arsc ==\n");
    const std::string gms_path = base + "/gms/resources.arsc";
    const auto gms_bytes = read_file(gms_path.c_str());
    std::printf("  file: %zu bytes\n", gms_bytes.size());

    double best_ms = 1e30;
    std::optional<Table> gms;
    for (int i = 0; i < 5; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        gms = Table::parse(gms_bytes, &err);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < best_ms) best_ms = ms;
    }
    check(gms.has_value(), "parse(gms) succeeds", err);
    if (!gms) return 1;
    std::printf("  parse(): %.2f ms (best of 5)\n", best_ms);
    check(best_ms < 1000.0, "parse(gms) well under a second");
    check(!gms->package_name().empty(), "gms package_name() = " + gms->package_name());
    probe_encodings(gms_bytes, "gms");
    {
        const auto gcfgs = gms->configurations();
        size_t with_loc = 0, with_dens = 0;
        std::string sample;
        for (const auto& cc : gcfgs) {
            if (!cc.language.empty()) ++with_loc;
            if (cc.has_density()) ++with_dens;
            if (cc.grammar_gender != 0 && sample.size() < 90 && !cc.region.empty()) sample += "[" + cc.to_string() + "]";
        }
        std::printf("  configurations(): %zu (locale=%zu, density=%zu) gendered e.g. %s\n", gcfgs.size(), with_loc,
                    with_dens, sample.c_str());
    }

    {
        // Resolve every id the table actually has (from aapt2's listing).
        const auto truth = load_dump((base + "/tests/arsc/gms_dump.txt").c_str());
        std::vector<uint32_t> real;
        for (const auto& kv : truth)
            if (real.empty() || real.back() != kv.first) real.push_back(kv.first);
        const auto t0 = std::chrono::steady_clock::now();
        size_t resolved = 0, strings = 0, bags = 0, unresolved = 0;
        for (const uint32_t id : real) {
            auto rv = gms->resolve(id);
            if (!rv) continue;
            if (rv->unresolved_reference) {
                ++unresolved;
                continue;
            }
            ++resolved;
            if (rv->string) ++strings;
            if (rv->is_complex) ++bags;
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("  resolve() over %zu real ids: %zu concrete (%zu with strings, %zu bags), %zu framework-dangling, %.1f ms\n",
                    real.size(), resolved, strings, bags, unresolved,
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
        check(!real.empty() && resolved + unresolved == real.size(), "every real gms id resolves without crashing");

        // Plus 20000 pseudo-random (mostly non-existent) ids, which must return
        // nullopt rather than crash.
        std::mt19937 rng(12345);
        std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFFu);
        size_t hits = 0;
        for (int i = 0; i < 20000; ++i) {
            const uint32_t id = (dist(rng) & 0x00FFFFFFu) | 0x7f000000u;
            if (gms->resolve((dist(rng) & 1) ? id : (id | 0x01000000u)).has_value()) ++hits;
        }
        std::printf("  20000 random ids: %zu resolved (rest nullopt), no crash\n", hits);
        check(true, "20000 random resolve() calls did not crash");
    }
    {
        std::printf("== cross-check vs aapt2 (gms) ==\n");
        const CrossStats st = cross_check(*gms, (base + "/tests/arsc/gms_dump.txt").c_str());
        std::printf("  ids=%d  config-set mismatches=%d (folded=%d)\n", st.ids, st.cfg_mismatch, st.cfg_fold_only);
        std::printf("  values compared=%d  bad=%d  skipped=%d\n", st.values_checked, st.values_bad,
                    st.values_skipped);
        check(st.cfg_mismatch == 0, "gms config sets identical to aapt2");
        check(st.values_bad == 0, "gms values identical to aapt2");
        for (const auto& ex : st.examples) std::printf("    cfg %s\n", ex.c_str());
        for (const auto& ex : st.value_examples) std::printf("    %s\n", ex.c_str());
    }

    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
