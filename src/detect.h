// Content based file-type detection.
//
// Requirement #2 of the rewrite: decide *what a file actually is* before
// reading it, because an Alpine package and an Android package share the .apk
// extension but have nothing else in common (Alpine: gzip/deflate container of
// tar streams or an "ADB" block container; Android: a zip with
// AndroidManifest.xml). The decision is made from the bytes, never from the
// file extension.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "model/info_document.h"

namespace apkinfo {

struct Detection {
    ContainerKind kind = ContainerKind::Unknown;
    // Short machine-ish format name: "v2", "v3", "zip", "apk", "split bundle", ...
    std::string format;
    // Human readable reason, e.g. "zip archive containing AndroidManifest.xml".
    std::string reason;
    // For bundles: which entry is the base apk ("" when not applicable).
    std::string base_apk_entry;
};

// Detects the container type of a whole file (a memory mapped view is fine).
Detection detect(std::span<const uint8_t> file);

}  // namespace apkinfo
