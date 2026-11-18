// Format-neutral "what to show" model.
//
// Android packages and Alpine packages have nothing in common on the wire, and
// their parsing lives in two independent modules (android/ and alpine/). What
// they *do* share is the shape of the result screen, so both produce this
// document and the UI renders it.
//
// Localisation contract
// ---------------------
// The readers always produce **English**, and only the keys listed in
// src/i18n.cpp are ever translated: row labels, section titles, the badge and
// enumerated values ("yes", "no", "(none)", "Single APK", ...). The presenter
// (src/presenter.cpp) translates at render time, which is why switching language
// never re-reads the package - it just re-renders from the cached document.
// Free-form diagnostics (warnings, error text) stay in English on purpose.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace apkinfo {

enum class ContainerKind {
    Unknown = 0,
    AndroidApk,          // single .apk (a zip with AndroidManifest.xml)
    AndroidSplitBundle,  // .apks / .apkm / .xapk / .apk containing base.apk + splits
    AlpinePackage,       // Alpine Package Keeper package (v2 or v3)
};

// English text for the container type; the presenter translates it.
const char* container_kind_name(ContainerKind kind);

// A decoded image, straight (non-premultiplied) RGBA8, row major, 4 bytes/px.
struct Bitmap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool valid() const { return width > 0 && height > 0 && rgba.size() == static_cast<size_t>(width) * height * 4; }
    void clear() {
        width = height = 0;
        rgba.clear();
    }
    static Bitmap solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
};

struct InfoRow {
    std::string label;   // English key ("" for list items)
    std::string value;
    std::string hint;    // technical detail, not translated
    bool monospace = false;
    bool wrap = false;
};

struct InfoSection {
    std::string title;      // English key
    size_t count = 0;       // > 0 -> the presenter renders "title (count)"
    std::vector<InfoRow> rows;

    // Long, low-value lists (permissions, localized names, ...) are shown
    // truncated: the presenter emits the first few rows plus a "show all"
    // affordance that opens the scrollable overlay.
    bool collapsible = false;

    void add(std::string label, std::string value, std::string hint = {}, bool monospace = false,
             bool wrap = false) {
        rows.push_back(InfoRow{std::move(label), std::move(value), std::move(hint), monospace, wrap});
    }
};

// One entry of a flat list (split bundles only - file contents are a tree).
struct ContentItem {
    std::string name;
    uint64_t size = 0;
    uint64_t packed_size = 0;
    std::string kind;        // "split"
    std::string detail_key;  // English key: "base" / "feature" / "feature (on demand)" / "density" / "locale"
    std::string detail_extra;// qualifier such as "config.xxxhdpi" - never translated
};

// A node of the lazily expandable file tree. Children of a node are stored
// contiguously, so expanding a node never touches the rest of the tree.
struct TreeNode {
    std::string name;         // display name ("" for the root)
    std::string path;         // path inside the package (tooltip)
    bool is_dir = false;
    uint64_t size = 0;        // own size for files, accumulated size for directories
    uint32_t children = 0;    // number of direct children
    uint32_t first_child = 0; // index of the first direct child
    // Number of nodes in this subtree, including this node. The children of a
    // node are contiguous, but a sibling does NOT immediately follow its
    // predecessor (that child's own subtree does), so skipping a collapsed node
    // needs this number.
    uint32_t subtree = 1;
    uint8_t depth = 0;
    uint8_t detail = 0;       // 0 = none, 1 = symlink, 2 = dex, 3 = native lib, 4 = resource, 5 = asset
};

enum class ContentsKind { Files, Splits };

struct InfoDocument {
    bool ok = false;
    std::string error;      // English; shown in the error banner
    ContainerKind kind = ContainerKind::Unknown;

    std::string file_path;
    std::string file_name;
    uint64_t file_size = 0;

    std::string title;     // app label / package name (never translated)
    std::string subtitle;  // "com.example.app · 1.2.3 (1234)"
    std::string badge;     // English key, translated by the presenter

    std::optional<Bitmap> icon;
    // Short English strings describing where the icon came from, joined with
    // " · " by the presenter (each part is translated when it is a known key).
    std::vector<std::string> icon_notes;

    std::vector<InfoSection> sections;

    ContentsKind contents_kind = ContentsKind::Files;
    std::vector<ContentItem> splits;    // contents_kind == Splits
    std::vector<TreeNode> tree;          // contents_kind == Files (node 0 is the root)
    size_t file_count = 0;               // number of file nodes (for the tab title)

    std::vector<std::string> warnings;   // English diagnostics

    // Filled in later, off the UI thread (hashing a 100 MB package must not block
    // the window); "" until known.
    std::string file_hash;
};

}  // namespace apkinfo
