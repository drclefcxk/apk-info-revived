// Reads an Android package (single .apk or a split bundle) into InfoDocument.
//
// Everything is parsed from the package itself: the binary AndroidManifest.xml
// and resources.arsc are read with the two independent parsers in this folder,
// the launcher icon is resolved through the resource table (which is the only
// way to find it in an obfuscated/shrunk APK) and composed when it turns out to
// be an adaptive icon.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "detect.h"
#include "model/info_document.h"

namespace apkinfo {

struct AndroidOptions {
    bool read_icon = true;
    bool read_contents = true;
    int icon_max_side = 320;
    // Density asked of the resource table when picking an icon bitmap.
    uint32_t icon_density = 640;
    // Cap for the file/split listing.
    size_t max_contents = 20000;
    // Locale used to resolve strings; "" means "the default resource".
    std::string language;
    std::string region;
};

// `file` is the whole file (memory mapped view is fine). `display_path` is only
// used for display. Never throws.
InfoDocument read_android_package(std::span<const uint8_t> file, const Detection& detection,
                                  const std::string& display_path, const AndroidOptions& options);

}  // namespace apkinfo
