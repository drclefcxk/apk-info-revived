// Minimal PNG writer: the CLI dumps icons with it, the viewer exports them with it.
#pragma once

#include <cstdint>
#include <string>

#include "model/info_document.h"

namespace apkinfo {

// Writes `bitmap` as an 8-bit RGBA PNG. Returns false when the file cannot be
// written.
bool write_png(const Bitmap& bitmap, const std::wstring& path);

}  // namespace apkinfo
