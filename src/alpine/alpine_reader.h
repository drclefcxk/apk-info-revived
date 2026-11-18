// Turns a parsed Alpine package into the shared InfoDocument model.
//
// This is deliberately independent of the Android reader: the two formats have
// nothing in common, only the *screen* is shared.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "detect.h"
#include "model/info_document.h"

namespace apkinfo {

struct AlpineOptions {
    bool read_contents = true;
    size_t max_contents = 20000;
};

InfoDocument read_alpine_package(std::span<const uint8_t> file, const Detection& detection,
                                 const std::string& display_path, const AlpineOptions& options);

}  // namespace apkinfo
