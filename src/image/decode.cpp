#include "image/decode.h"

#include <webp/decode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "stb/stb_image.h"

namespace apkinfo {
namespace {

void blend_over(uint8_t* dst, const uint8_t* src, float alpha_scale = 1.0f) {
    const float sa = (src[3] / 255.0f) * alpha_scale;
    if (sa <= 0.0f) return;
    const float da = dst[3] / 255.0f;
    const float out_a = sa + da * (1.0f - sa);
    if (out_a <= 0.0f) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        return;
    }
    for (int c = 0; c < 3; ++c) {
        const float v = (src[c] * sa + dst[c] * da * (1.0f - sa)) / out_a;
        dst[c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
    }
    dst[3] = static_cast<uint8_t>(std::clamp(out_a * 255.0f, 0.0f, 255.0f) + 0.5f);
}

}  // namespace

std::string sniff_image_format(std::span<const uint8_t> bytes) {
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') {
        return "png";
    }
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) return "jpeg";
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
        std::memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
        return "webp";
    }
    if (bytes.size() >= 6 && (std::memcmp(bytes.data(), "GIF87a", 6) == 0 ||
                              std::memcmp(bytes.data(), "GIF89a", 6) == 0)) {
        return "gif";
    }
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return "bmp";
    // Android compiled XML (adaptive icon, vector drawable ...)
    if (bytes.size() >= 2 && bytes[0] == 0x03 && bytes[1] == 0x00) return "xml";
    if (bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K') return "zip";
    return {};
}

std::optional<Bitmap> decode_image(std::span<const uint8_t> bytes, std::string* error) {
    if (bytes.empty()) {
        if (error) *error = "empty image data";
        return std::nullopt;
    }
    const std::string format = sniff_image_format(bytes);
    if (format == "webp") {
        int width = 0, height = 0;
        if (!WebPGetInfo(bytes.data(), bytes.size(), &width, &height) || width <= 0 || height <= 0) {
            if (error) *error = "invalid WebP header";
            return std::nullopt;
        }
        if (static_cast<int64_t>(width) * height > 64ll * 1024 * 1024) {
            if (error) *error = "WebP image is unreasonably large";
            return std::nullopt;
        }
        Bitmap bitmap;
        bitmap.width = width;
        bitmap.height = height;
        bitmap.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        if (!WebPDecodeRGBAInto(bytes.data(), bytes.size(), bitmap.rgba.data(),
                                static_cast<size_t>(width) * height * 4, width * 4)) {
            if (error) *error = "WebP decode failed";
            return std::nullopt;
        }
        return bitmap;
    }

    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(std::min<size_t>(bytes.size(), 0x7FFFFFFF)),
                                                  &width, &height, &channels, 4);
    if (!pixels) {
        if (error) {
            const char* reason = stbi_failure_reason();
            *error = std::string("image decode failed") + (reason ? std::string(": ") + reason : "");
        }
        return std::nullopt;
    }
    Bitmap bitmap;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.rgba.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(pixels);
    return bitmap;
}

Bitmap resize_bitmap(const Bitmap& source, int width, int height) {
    if (!source.valid() || width <= 0 || height <= 0) return {};
    Bitmap out;
    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    // Box filter for downscaling (good enough and stable), bilinear for up.
    const float scale_x = static_cast<float>(source.width) / static_cast<float>(width);
    const float scale_y = static_cast<float>(source.height) / static_cast<float>(height);
    for (int y = 0; y < height; ++y) {
        const float sy0 = y * scale_y;
        const float sy1 = std::min<float>(sy0 + scale_y, static_cast<float>(source.height));
        for (int x = 0; x < width; ++x) {
            const float sx0 = x * scale_x;
            const float sx1 = std::min<float>(sx0 + scale_x, static_cast<float>(source.width));
            float accum[4] = {0, 0, 0, 0};
            float weight = 0;
            for (int sy = static_cast<int>(sy0); sy < std::max(static_cast<int>(sy0) + 1, static_cast<int>(std::ceil(sy1))); ++sy) {
                if (sy >= source.height) break;
                const float wy = std::min(sy1, static_cast<float>(sy + 1)) - std::max(sy0, static_cast<float>(sy));
                if (wy <= 0) continue;
                for (int sx = static_cast<int>(sx0); sx < std::max(static_cast<int>(sx0) + 1, static_cast<int>(std::ceil(sx1))); ++sx) {
                    if (sx >= source.width) break;
                    const float wx = std::min(sx1, static_cast<float>(sx + 1)) - std::max(sx0, static_cast<float>(sx));
                    if (wx <= 0) continue;
                    const float w = wx * wy;
                    const size_t idx = (static_cast<size_t>(sy) * source.width + sx) * 4;
                    const float alpha = source.rgba[idx + 3] / 255.0f;
                    // Premultiply while averaging so transparent pixels do not
                    // bleed their (usually black) colour into the result.
                    accum[0] += source.rgba[idx + 0] * alpha * w;
                    accum[1] += source.rgba[idx + 1] * alpha * w;
                    accum[2] += source.rgba[idx + 2] * alpha * w;
                    accum[3] += alpha * w;
                    weight += w;
                }
            }
            const size_t out_idx = (static_cast<size_t>(y) * width + x) * 4;
            if (weight <= 0) continue;
            const float alpha = accum[3] / weight;
            out.rgba[out_idx + 3] = static_cast<uint8_t>(std::clamp(alpha * 255.0f, 0.0f, 255.0f) + 0.5f);
            if (alpha > 0.0001f) {
                for (int c = 0; c < 3; ++c) {
                    out.rgba[out_idx + c] = static_cast<uint8_t>(
                        std::clamp(accum[c] / (alpha * weight), 0.0f, 255.0f) + 0.5f);
                }
            }
        }
    }
    return out;
}

Bitmap compose_adaptive_icon(const Bitmap& background, const Bitmap& foreground, int max_side) {
    if (!background.valid() && !foreground.valid()) return {};
    if (!foreground.valid()) return limit_size(background, max_side);
    if (!background.valid()) return limit_size(foreground, max_side);

    // Adaptive icons are 108dp canvases where both layers span the whole
    // canvas. Use the larger of the two as the canvas size.
    const int canvas = std::max({background.width, background.height, foreground.width, foreground.height});
    if (canvas <= 0) return {};

    Bitmap result;
    result.width = canvas;
    result.height = canvas;
    result.rgba.assign(static_cast<size_t>(canvas) * canvas * 4, 0);

    const Bitmap bg = (background.width == canvas && background.height == canvas)
                          ? background
                          : resize_bitmap(background, canvas, canvas);
    std::copy(bg.rgba.begin(), bg.rgba.end(), result.rgba.begin());

    const Bitmap fg = (foreground.width == canvas && foreground.height == canvas)
                          ? foreground
                          : resize_bitmap(foreground, canvas, canvas);
    for (size_t i = 0; i + 3 < result.rgba.size(); i += 4) {
        blend_over(&result.rgba[i], &fg.rgba[i]);
    }
    Bitmap limited = limit_size(result, max_side);
    return limited;
}

Bitmap limit_size(const Bitmap& source, int max_side) {
    if (!source.valid() || max_side <= 0) return {};
    const int longest = std::max(source.width, source.height);
    if (longest <= max_side) return source;
    const float ratio = static_cast<float>(max_side) / static_cast<float>(longest);
    const int width = std::max(1, static_cast<int>(source.width * ratio + 0.5f));
    const int height = std::max(1, static_cast<int>(source.height * ratio + 0.5f));
    return resize_bitmap(source, width, height);
}

}  // namespace apkinfo