// Image decoding + adaptive-icon compositing.
//
// This is the fix for "the apk icon does not show": a modern launcher icon is
// usually (a) a WebP file, not a PNG, and (b) a compiled adaptive-icon XML
// pointing at two more resources (foreground/background), so a tool that just
// grabs "res/mipmap-*/ic_launcher.png" finds nothing at all.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "model/info_document.h"

namespace apkinfo {

// Decodes PNG / JPEG / BMP / GIF (stb_image) and WebP (libwebp) into RGBA8.
std::optional<Bitmap> decode_image(std::span<const uint8_t> bytes, std::string* error);

// Detects the image format from its magic bytes: "png", "jpeg", "webp", "gif",
// "bmp", "xml" (compiled binary xml), "" when unknown.
std::string sniff_image_format(std::span<const uint8_t> bytes);

// Simple high quality box/bilinear resize to exactly w x h.
Bitmap resize_bitmap(const Bitmap& source, int width, int height);

// Composites an Android adaptive icon: `background` fills the canvas,
// `foreground` is drawn on top (both scaled to the same square canvas), and the
// result is downscaled so the longest side is at most `max_side`.
Bitmap compose_adaptive_icon(const Bitmap& background, const Bitmap& foreground, int max_side = 320);

// Downscales so the longest side is at most `max_side` (keeps small icons as-is).
Bitmap limit_size(const Bitmap& source, int max_side);

}  // namespace apkinfo
