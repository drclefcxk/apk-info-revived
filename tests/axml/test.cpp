// Test harness for ApkInfo/src/android/axml.cpp  (lives outside src on purpose)
#include "axml.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace apkinfo::axml;

static std::vector<uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    return data;
}

static std::string read_text(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string trim_line(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\r')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// Normalize an aapt2-style dump into "depth|content" so that indentation
// conventions and cosmetic (line=..) differences do not matter.
// aapt2 indents elements by 4*depth-2 and attributes by 4*depth;
// we indent elements by 2*depth and attributes by 2*depth+2.
static std::vector<std::string> normalize(const std::string& text, bool aapt2_style) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        std::string content = trim_line(line);
        if (content.empty()) {
            if (nl >= text.size()) break;
            continue;
        }
        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;
        int depth = 0;
        bool is_attr = content.rfind("A: ", 0) == 0;
        if (aapt2_style) {
            depth = is_attr ? static_cast<int>(indent / 4)
                            : static_cast<int>((indent + 2) / 4);
        } else {
            depth = is_attr ? static_cast<int>(indent / 2) - 1
                            : static_cast<int>(indent / 2);
        }
        if (depth < 0) depth = 0;
        // Namespace lines carry no meaningful depth in either dump (aapt2 nests
        // them inconsistently; Document::Namespace has no line/depth), so only
        // their content is compared.
        if (content.rfind("N: ", 0) == 0) {
            depth = 0;
            size_t p = content.find(" (line=");
            if (p != std::string::npos) content.erase(p);
        }
        out.push_back(std::to_string(depth) + "|" + content);
        if (nl >= text.size()) break;
    }
    return out;
}

static int compare(const char* axml_path, const char* gt_path) {
    auto data = read_file(axml_path);
    Result r = parse(std::span<const uint8_t>(data.data(), data.size()));
    if (!r.ok) {
        std::printf("FAIL parse %s: %s\n", axml_path, r.error.c_str());
        return 1;
    }
    std::string mine = to_text(r.doc);
    std::string gt = read_text(gt_path);
    auto a = normalize(mine, false);
    auto b = normalize(gt, true);
    size_t mismatches = 0;
    size_t n = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const std::string x = i < a.size() ? a[i] : std::string("<missing>");
        const std::string y = i < b.size() ? b[i] : std::string("<missing>");
        if (x != y) {
            if (mismatches < 15) {
                std::printf("  line %zu\n    mine: %s\n    gt  : %s\n", i + 1, x.c_str(), y.c_str());
            }
            ++mismatches;
        }
    }
    std::printf("%s: mine=%zu gt=%zu lines, mismatches=%zu\n", axml_path, a.size(), b.size(),
                mismatches);
    return mismatches == 0 ? 0 : 1;
}

static bool expect(bool cond, const char* what) {
    if (!cond) std::printf("  ASSERT FAILED: %s\n", what);
    return cond;
}

static int selftest_icon(const char* path) {
    auto data = read_file(path);
    Result r = parse(std::span<const uint8_t>(data.data(), data.size()));
    bool ok = true;
    ok &= expect(r.ok, "icon parses");
    if (!r.ok) {
        std::printf("  error: %s\n", r.error.c_str());
        return 1;
    }
    const Element* root = r.doc.root();
    ok &= expect(root && root->name == "adaptive-icon", "root is adaptive-icon");
    ok &= expect(r.doc.namespaces.size() == 1, "one namespace");
    ok &= expect(r.doc.namespaces[0].prefix == "android", "prefix android");
    ok &= expect(r.doc.namespaces[0].uri == "http://schemas.android.com/apk/res/android",
                 "android uri");
    const Element* bg = root ? root->child("background") : nullptr;
    ok &= expect(bg != nullptr, "background child");
    const Element* bmp = bg ? bg->child("bitmap") : nullptr;
    ok &= expect(bmp != nullptr, "background/bitmap");
    const Attribute* src = bmp ? bmp->attribute("src") : nullptr;
    ok &= expect(src != nullptr, "bitmap/@src");
    if (src) {
        ok &= expect(src->res_id == 0x01010119u, "src res id 0x01010119");
        ok &= expect(src->value.text == "@0x7f0f0001", "src value @0x7f0f0001");
        ok &= expect(src->value.type == ValueType::Reference, "src is a reference");
        ok &= expect(src->prefix == "android", "src prefix resolved to android");
        ok &= expect(src->ns == "http://schemas.android.com/apk/res/android", "src ns uri");
    }
    const Element* mono = root ? root->find("monochrome") : nullptr;
    ok &= expect(mono != nullptr, "monochrome (depth-first find)");
    const Attribute* dr = mono ? mono->attribute_by_res(0x01010199u) : nullptr;
    ok &= expect(dr != nullptr, "monochrome drawable by res id 0x01010199");
    if (dr) ok &= expect(dr->value.text == "@0x7f0802d0", "monochrome drawable value");
    const Element* fg = root ? root->child("foreground") : nullptr;
    ok &= expect(fg != nullptr && fg->child("bitmap") != nullptr, "foreground/bitmap");
    ok &= expect(root->children_named("background").size() == 1, "children_named works");
    return ok ? 0 : 1;
}

static int selftest_manifest(const char* path) {
    auto data = read_file(path);
    Result r = parse(std::span<const uint8_t>(data.data(), data.size()));
    bool ok = true;
    ok &= expect(r.ok, "manifest parses");
    if (!r.ok) {
        std::printf("  error: %s\n", r.error.c_str());
        return 1;
    }
    const Element* root = r.doc.root();
    ok &= expect(root && root->name == "manifest", "root is manifest");
    if (!root) return 1;
    ok &= expect(root->attr_text("package") == "app.lawnchair", "package attr");
    ok &= expect(root->attr_int("versionCode") == 1500020300LL, "versionCode 1500020300");
    ok &= expect(root->attr_text("versionName") == "15.Beta 3", "versionName");
    const Attribute* vc = root->attribute_by_res(0x0101021bu);
    ok &= expect(vc != nullptr, "versionCode by res id");
    if (vc) {
        ok &= expect(vc->name == "versionCode", "res-id lookup name");
        ok &= expect(vc->value.type == ValueType::IntDec, "versionCode is IntDec");
        ok &= expect(vc->prefix == "android", "versionCode prefix android");
    }
    ok &= expect(root->attribute("versionCode", "http://schemas.android.com/apk/res/android") !=
                     nullptr,
                 "namespace-exact lookup");
    ok &= expect(root->attribute("versionCode", "urn:nope") == nullptr,
                 "namespace-exact lookup rejects wrong ns");
    ok &= expect(root->attr_int("nope", 42) == 42, "attr_int fallback");
    const Element* sdk = root->child("uses-sdk");
    ok &= expect(sdk && sdk->attr_int("minSdkVersion") == 26, "minSdkVersion 26");
    ok &= expect(sdk && sdk->attr_int("targetSdkVersion") == 35, "targetSdkVersion 35");
    ok &= expect(root->children_named("uses-permission").size() > 20, "many uses-permission");
    const Element* app = root->child("application");
    ok &= expect(app != nullptr, "application element");
    ok &= expect(app && app->attribute_by_res(0x01010002u) != nullptr, "application @icon");
    ok &= expect(app && app->child("activity") != nullptr, "activity child");
    ok &= expect(r.doc.namespaces.size() >= 1, "namespace present");
    ok &= expect(looks_like_axml(std::span<const uint8_t>(data.data(), data.size())),
                 "looks_like_axml");
    return ok ? 0 : 1;
}

// Truncations at every length plus random single/multi byte corruption.
static int fuzz(const char* path) {
    auto data = read_file(path);
    size_t ok_count = 0, err_count = 0;
    bool ok = true;
    for (size_t n = 0; n <= data.size(); ++n) {
        Result r = parse(std::span<const uint8_t>(data.data(), n));
        if (r.ok) ++ok_count; else ++err_count;
    }
    std::printf("  truncation sweep %zu lengths: ok=%zu err=%zu\n", data.size() + 1, ok_count,
                err_count);
    // random corruption
    uint32_t state = 0x12345678u;
    auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    std::vector<uint8_t> buf;
    for (int iter = 0; iter < 20000; ++iter) {
        buf = data;
        unsigned flips = 1 + (next() % 12);
        for (unsigned i = 0; i < flips; ++i) {
            size_t at = next() % buf.size();
            buf[at] = static_cast<uint8_t>(next());
        }
        Result r = parse(std::span<const uint8_t>(buf.data(), buf.size()));
        (void)r;
    }
    std::printf("  20000 corrupted buffers parsed without crashing\n");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test dump <file> | check icon <file> | check manifest <file> |"
                    " fuzz <file> | cmp <axml> <gt.txt>\n");
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "dump" && argc >= 3) {
        auto data = read_file(argv[2]);
        Result r = parse(std::span<const uint8_t>(data.data(), data.size()));
        if (!r.ok) {
            std::printf("error: %s\n", r.error.c_str());
            return 1;
        }
        std::fputs(to_text(r.doc).c_str(), stdout);
        return 0;
    }
    if (mode == "check" && argc >= 4) {
        const std::string what = argv[2];
        int rc = (what == "icon")      ? selftest_icon(argv[3])
                 : (what == "manifest") ? selftest_manifest(argv[3])
                                        : 2;
        std::printf("%s: %s\n", what.c_str(), rc == 0 ? "PASS" : "FAIL");
        return rc;
    }
    if (mode == "fuzz" && argc >= 3) return fuzz(argv[2]);
    if (mode == "cmp" && argc >= 4) return compare(argv[2], argv[3]);
    std::printf("bad arguments\n");
    return 2;
}
