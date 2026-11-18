#include "android/apk_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "android/arsc.h"
#include "android/versions.h"
#include "android/axml.h"
#include "container/file_map.h"
#include "container/zip_file.h"
#include "image/decode.h"
#include "image/vector.h"
#include "model/tree_builder.h"
#include "util/hash.h"
#include "util/text.h"
#include "i18n.h"

namespace apkinfo {
namespace {

using axml::Element;
using axml::ValueType;
using arsc::ConfigPreference;

// android:* attribute resource ids used directly (the rest are looked up by
// local name).
constexpr uint32_t kAttrGlEsVersion = 0x01010281;

std::string sdk_text(int64_t api, const std::string& codename = {}) {
    if (api <= 0 && codename.empty()) return {};
    if (api <= 0) return codename;
    // The table lives in android-versions.txt next to the exe (android/versions.h).
    const std::string version = versions::android_version_name(api);
    std::string text = std::to_string(api);
    if (!version.empty()) text += " (Android " + version + ")";
    if (!codename.empty()) text += " · " + codename;
    return text;
}

std::string argb_to_hex(uint32_t argb) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "#%08X", argb);
    return buffer;
}

struct TempFile {
    std::wstring path;
    ~TempFile() {
        if (!path.empty()) DeleteFileW(path.c_str());
    }
};

std::optional<std::wstring> make_temp_file() {
    wchar_t directory[MAX_PATH + 1]{};
    if (GetTempPathW(MAX_PATH, directory) == 0) return std::nullopt;
    wchar_t name[MAX_PATH + 1]{};
    if (GetTempFileNameW(directory, L"api", 0, name) == 0) return std::nullopt;
    return std::wstring(name);
}

// ---------------------------------------------------------------------------
// Manifest attribute access with resource-table resolution
// ---------------------------------------------------------------------------

std::string join_set(const std::set<std::string>& values, std::string_view separator) {
    return join(std::vector<std::string>(values.begin(), values.end()), separator);
}

// Human name for a ResTable_config density value.
std::string density_label(uint32_t density) {
    switch (density) {
        case 120:
            return "ldpi";
        case 160:
            return "mdpi";
        case 213:
            return "tvdpi";
        case 240:
            return "hdpi";
        case 320:
            return "xhdpi";
        case 480:
            return "xxhdpi";
        case 640:
            return "xxxhdpi";
        case 0xFFFE:
            return "anydpi";
        case 0xFFFF:
            return "nodpi";
        default:
            return std::to_string(density) + "dpi";
    }
}

std::optional<uint32_t> attr_reference(const Element& element, std::string_view name) {
    const axml::Attribute* attribute = element.attribute(name);
    if (!attribute) return std::nullopt;
    if (attribute->value.type == ValueType::Reference ||
        attribute->value.type == ValueType::DynamicReference) {
        if (attribute->value.data != 0) return attribute->value.data;
    }
    return std::nullopt;
}

std::string attr_string(const Element& element, std::string_view name, const arsc::Table* table,
                       const ConfigPreference& preference) {
    const axml::Attribute* attribute = element.attribute(name);
    if (!attribute) return {};
    if ((attribute->value.type == ValueType::Reference ||
         attribute->value.type == ValueType::DynamicReference) &&
        table) {
        if (auto resolved = table->resolve_string(attribute->value.data, preference)) return *resolved;
    }
    if (!attribute->value.text.empty()) return attribute->value.text;
    return attribute->value.raw;
}

int64_t attr_int(const Element& element, std::string_view name, const arsc::Table* table,
                 const ConfigPreference& preference, int64_t fallback) {
    const axml::Attribute* attribute = element.attribute(name);
    if (!attribute) return fallback;
    if ((attribute->value.type == ValueType::Reference ||
         attribute->value.type == ValueType::DynamicReference) &&
        table) {
        if (auto resolved = table->resolve_int(attribute->value.data, preference)) return *resolved;
        return fallback;
    }
    switch (attribute->value.type) {
        case ValueType::IntDec:
        case ValueType::IntHex:
        case ValueType::IntBoolean:
        case ValueType::ColorArgb8:
        case ValueType::ColorRgb8:
        case ValueType::ColorArgb4:
        case ValueType::ColorRgb4:
            return static_cast<int32_t>(attribute->value.data);
        case ValueType::String: {
            const std::string& text = attribute->value.text;
            if (text == "true") return 1;
            if (text == "false") return 0;
            return std::strtoll(text.c_str(), nullptr, 0);
        }
        default:
            break;
    }
    return fallback;
}

// Tri-state boolean attribute (absent -> fallback).
// Reads a float-ish attribute (Float, IntDec, Dimension or a numeric string).
// Vector drawables carry viewport sizes as floats, path alphas as float.
float attr_float(const Element& element, std::string_view name, float fallback = 0.0f) {
    const axml::Attribute* attribute = element.attribute(name);
    if (!attribute) return fallback;
    switch (attribute->value.type) {
        case ValueType::Float: {
            float value = 0.0f;
            std::memcpy(&value, &attribute->value.data, sizeof value);
            return value;
        }
        case ValueType::IntDec:
        case ValueType::IntHex:
            return static_cast<float>(static_cast<int32_t>(attribute->value.data));
        case ValueType::Dimension:
            // raw dimension: 24 bits of value, 8 bits of unit; 1/16px for most units
            return static_cast<float>(attribute->value.data >> 8) /
                   (attribute->value.data & 0xFF ? 1.0f : 1.0f) / 16.0f;
        case ValueType::String: {
            const std::string& raw = attribute->value.text.empty() ? attribute->value.raw : attribute->value.text;
            return raw.empty() ? fallback : static_cast<float>(std::atof(raw.c_str()));
        }
        default:
            return fallback;
    }
}

std::optional<bool> attr_flag(const Element& element, std::string_view name) {
    const axml::Attribute* attribute = element.attribute(name);
    if (!attribute) return std::nullopt;
    switch (attribute->value.type) {
        case ValueType::IntBoolean:
        case ValueType::IntDec:
        case ValueType::IntHex:
            return attribute->value.data != 0;
        case ValueType::String:
            return attribute->value.text == "true";
        default:
            return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Icon extraction
// ---------------------------------------------------------------------------

Bitmap drawable_to_bitmap(const ZipFile& zip, const arsc::Table* table, uint32_t resource_id,
                          const ConfigPreference& preference, std::string* note, int depth,
                          int max_side);

uint32_t color_from_element(const Element& element, std::string_view attribute_name,
                            const arsc::Table* table, const ConfigPreference& preference,
                            bool* known) {
    *known = false;
    const axml::Attribute* attribute = element.attribute(attribute_name);
    if (!attribute) return 0;
    switch (attribute->value.type) {
        case ValueType::ColorArgb8:
        case ValueType::ColorRgb8:
        case ValueType::ColorArgb4:
        case ValueType::ColorRgb4:
        case ValueType::IntDec:
        case ValueType::IntHex:
            *known = true;
            return attribute->value.data;
        case ValueType::Reference:
        case ValueType::DynamicReference:
            if (table) {
                if (auto resolved = table->resolve(attribute->value.data, preference)) {
                    if (resolved->type == arsc::ValueType::ColorArgb8 ||
                        resolved->type == arsc::ValueType::ColorRgb8 ||
                        resolved->type == arsc::ValueType::IntDec ||
                        resolved->type == arsc::ValueType::IntHex) {
                        *known = true;
                        return resolved->data;
                    }
                }
            }
            return 0;
        default:
            return 0;
    }
}

// "<scale> android:scaleWidth" is a percentage string ("30%"); accept a plain
// factor as well, which is what some tools write.
float percent_attribute(const Element& element, std::string_view name, float fallback) {
    const axml::Attribute* attribute = element.attribute(name);
    if (!attribute) return fallback;
    if (attribute->value.type == ValueType::Float) {
        float value = 0.0f;
        std::memcpy(&value, &attribute->value.data, sizeof value);
        return value;
    }
    std::string raw = attribute->value.text.empty() ? attribute->value.raw : attribute->value.text;
    if (raw.empty()) return fallback;
    const bool is_percent = raw.find('%') != std::string::npos;
    const float value = static_cast<float>(std::atof(raw.c_str()));
    if (!is_percent && value > 1.5f) return value / 100.0f;  // "30" written as a number
    return is_percent ? value / 100.0f : value;
}

// Bilinear blit of `source` into `canvas` at (x, y) with the given size.
void blit_scaled(Bitmap* canvas, const Bitmap& source, int x, int y, int width, int height) {
    if (!canvas->valid() || !source.valid() || width <= 0 || height <= 0) return;
    for (int dy = 0; dy < height; ++dy) {
        const int ty = y + dy;
        if (ty < 0 || ty >= canvas->height) continue;
        const float sy = (static_cast<float>(dy) + 0.5f) / static_cast<float>(height) * source.height - 0.5f;
        const int sy0 = std::clamp(static_cast<int>(std::floor(sy)), 0, source.height - 1);
        const int sy1 = std::clamp(sy0 + 1, 0, source.height - 1);
        const float fy = std::clamp(sy - static_cast<float>(sy0), 0.0f, 1.0f);
        for (int dx = 0; dx < width; ++dx) {
            const int tx = x + dx;
            if (tx < 0 || tx >= canvas->width) continue;
            const float sx = (static_cast<float>(dx) + 0.5f) / static_cast<float>(width) * source.width - 0.5f;
            const int sx0 = std::clamp(static_cast<int>(std::floor(sx)), 0, source.width - 1);
            const int sx1 = std::clamp(sx0 + 1, 0, source.width - 1);
            const float fx = std::clamp(sx - static_cast<float>(sx0), 0.0f, 1.0f);

            const auto sample = [&](int px, int py, int channel) {
                return static_cast<float>(
                    source.rgba[(static_cast<size_t>(py) * static_cast<size_t>(source.width) +
                                 static_cast<size_t>(px)) * 4 + static_cast<size_t>(channel)]);
            };
            uint8_t out[4];
            for (int channel = 0; channel < 4; ++channel) {
                const float top = sample(sx0, sy0, channel) * (1.0f - fx) + sample(sx1, sy0, channel) * fx;
                const float bottom = sample(sx0, sy1, channel) * (1.0f - fx) + sample(sx1, sy1, channel) * fx;
                out[channel] = static_cast<uint8_t>(std::lround(std::clamp(top * (1.0f - fy) + bottom * fy, 0.0f, 255.0f)));
            }
            uint8_t* pixel = &canvas->rgba[(static_cast<size_t>(ty) * static_cast<size_t>(canvas->width) +
                                           static_cast<size_t>(tx)) * 4];
            const float src_a = out[3] / 255.0f;
            const float dst_a = pixel[3] / 255.0f;
            const float total_a = src_a + dst_a * (1.0f - src_a);
            if (total_a <= 0.0f) continue;
            for (int channel = 0; channel < 3; ++channel) {
                const float mixed = (out[channel] / 255.0f * src_a + pixel[channel] / 255.0f * dst_a * (1.0f - src_a)) /
                                    total_a;
                pixel[channel] = static_cast<uint8_t>(std::lround(std::clamp(mixed, 0.0f, 1.0f) * 255.0f));
            }
            pixel[3] = static_cast<uint8_t>(std::lround(std::clamp(total_a, 0.0f, 1.0f) * 255.0f));
        }
    }
}

// Resolves a compiled <vector> drawable into a bitmap: viewport scaling, <group>
// transforms and <path> fill/stroke. Without this, adaptive icons whose layers are
// vectors (Magisk, KernelSU, ...) decoded to nothing.
Bitmap vector_drawable_to_bitmap(const Element& root, const arsc::Table* table,
                                 const ConfigPreference& preference, int max_side, std::string* note) {
    const float viewport_width = attr_float(root, "viewportWidth");
    const float viewport_height = attr_float(root, "viewportHeight");
    if (viewport_width <= 0.0f || viewport_height <= 0.0f) {
        if (note) *note = "vector (no viewport)";
        return {};
    }

    // Keep the aspect ratio of the declared dp size; fall back to the viewport.
    float aspect = viewport_width / viewport_height;
    const float width_dp = attr_float(root, "width");
    const float height_dp = attr_float(root, "height");
    if (width_dp > 0.0f && height_dp > 0.0f) aspect = width_dp / height_dp;

    const int long_side = std::clamp(max_side, 16, 1024);
    int canvas_width = long_side;
    int canvas_height = long_side;
    if (aspect >= 1.0f) {
        canvas_height = std::clamp(static_cast<int>(std::lround(long_side / aspect)), 8, 1024);
    } else {
        canvas_width = std::clamp(static_cast<int>(std::lround(long_side * aspect)), 8, 1024);
    }

    Bitmap canvas = Bitmap::solid(canvas_width, canvas_height, 0, 0, 0, 0);
    const vector::Transform base =
        vector::Transform::scale(static_cast<float>(canvas_width) / viewport_width,
                                 static_cast<float>(canvas_height) / viewport_height);

    bool used_gradient_fallback = false;
    bool clipped = false;

    // Recursive walk over <group> / <path> children.
    std::function<void(const Element&, const vector::Transform&, int)> walk =
        [&](const Element& parent, const vector::Transform& transform, int depth) {
            if (depth > 12) return;
            for (const Element& child : parent.children) {
                if (child.name == "group") {
                    const float translate_x = attr_float(child, "translateX");
                    const float translate_y = attr_float(child, "translateY");
                    const float scale_x = attr_float(child, "scaleX", 1.0f);
                    const float scale_y = attr_float(child, "scaleY", 1.0f);
                    const float rotation = attr_float(child, "rotation");
                    const float pivot_x = attr_float(child, "pivotX");
                    const float pivot_y = attr_float(child, "pivotY");

                    // Android applies: translate(translate + pivot) * rotate * scale * translate(-pivot)
                    vector::Transform local =
                        vector::Transform::translate(translate_x + pivot_x, translate_y + pivot_y)
                            .concat(vector::Transform::rotate(rotation, 0.0f, 0.0f))
                            .concat(vector::Transform::scale(scale_x, scale_y))
                            .concat(vector::Transform::translate(-pivot_x, -pivot_y));
                    walk(child, transform.concat(local), depth + 1);
                    continue;
                }
                if (child.name == "clip-path") {
                    clipped = true;
                    continue;
                }
                if (child.name != "path") continue;

                const std::string data = attr_string(child, "pathData", table, preference);
                if (data.empty()) continue;
                const std::vector<vector::Subpath> subpaths = vector::parse_path(data, 0.12f);
                if (subpaths.empty()) continue;

                const float fill_alpha = attr_float(child, "fillAlpha", 1.0f);
                const float stroke_alpha = attr_float(child, "strokeAlpha", 1.0f);
                const float stroke_width = attr_float(child, "strokeWidth");

                bool has_fill = false;
                uint32_t fill_color = color_from_element(child, "fillColor", table, preference, &has_fill);
                if (!has_fill) {
                    // <aapt:attr name="android:fillColor"><gradient .../></aapt:attr>:
                    // use the start colour as a flat approximation.
                    for (const Element& nested : child.children) {
                        if (nested.name != "gradient") continue;
                        bool known = false;
                        const uint32_t start = color_from_element(nested, "startColor", table, preference, &known);
                        if (known) {
                            fill_color = start;
                            has_fill = true;
                            used_gradient_fallback = true;
                        }
                        break;
                    }
                }
                if (has_fill) {
                    const uint32_t argb = (fill_color & 0x00FFFFFFu) |
                                          (static_cast<uint32_t>(((fill_color >> 24) & 0xFF) * fill_alpha) << 24);
                    const bool even_odd = attr_string(child, "fillType", table, preference) == "evenOdd";
                    vector::fill(&canvas, subpaths, transform, argb, even_odd);
                }
                if (stroke_width > 0.0f) {
                    bool known = false;
                    const uint32_t stroke_color = color_from_element(child, "strokeColor", table, preference, &known);
                    if (known) {
                        const uint32_t argb = (stroke_color & 0x00FFFFFFu) |
                                              (static_cast<uint32_t>(((stroke_color >> 24) & 0xFF) * stroke_alpha) << 24);
                        vector::stroke(&canvas, subpaths, transform, argb, stroke_width);
                    }
                }
            }
        };
    walk(root, base, 0);

    if (note) {
        std::string tag = "vector";
        if (used_gradient_fallback) tag += ", gradient flattened to its start colour";
        if (clipped) tag += ", clip-path ignored";
        *note = tag;
    }
    return canvas;
}

// Resolves a compiled XML drawable (adaptive icon, bitmap wrapper, layer-list,
// inset, colour) into a bitmap.
Bitmap xml_drawable_to_bitmap(const ZipFile& zip, const axml::Document& document,
                              const arsc::Table* table, const ConfigPreference& preference,
                              std::string* note, int depth, int max_side) {
    const Element* root = document.root();
    if (!root) return {};
    const std::string& root_name = root->name;

    if (root_name == "adaptive-icon" || root_name == "adaptive-icon-v2") {
        Bitmap background;
        Bitmap foreground;
        bool monochrome = false;
        for (const Element& child : root->children) {
            const bool is_background = child.name == "background";
            const bool is_foreground = child.name == "foreground";
            if (child.name == "monochrome") monochrome = true;
            if (!is_background && !is_foreground) continue;

            Bitmap layer;
            // Layer can be a colour reference, a drawable reference, or a
            // nested <bitmap>/<color>/<inset> element.
            if (auto reference = attr_reference(child, "drawable")) {
                layer = drawable_to_bitmap(zip, table, *reference, preference, note, depth + 1, max_side);
            }
            if (!layer.valid()) {
                for (const Element& nested : child.children) {
                    if (nested.name == "bitmap" || nested.name == "inset") {
                        if (auto reference = attr_reference(nested, "src")) {
                            layer = drawable_to_bitmap(zip, table, *reference, preference, note, depth + 1, max_side);
                        } else if (auto inset = attr_reference(nested, "drawable")) {
                            layer = drawable_to_bitmap(zip, table, *inset, preference, note, depth + 1, max_side);
                        }
                    } else if (nested.name == "color") {
                        bool known = false;
                        const uint32_t argb = color_from_element(nested, "color", table, preference, &known);
                        if (known) {
                            layer = Bitmap::solid(1, 1, static_cast<uint8_t>(argb >> 16),
                                                  static_cast<uint8_t>(argb >> 8),
                                                  static_cast<uint8_t>(argb), static_cast<uint8_t>(argb >> 24));
                        }
                    }
                    if (layer.valid()) break;
                }
            }
            if (!layer.valid()) {
                bool known = false;
                const uint32_t argb = color_from_element(child, "color", table, preference, &known);
                if (known) {
                    layer = Bitmap::solid(1, 1, static_cast<uint8_t>(argb >> 16), static_cast<uint8_t>(argb >> 8),
                                          static_cast<uint8_t>(argb), static_cast<uint8_t>(argb >> 24));
                }
            }
            if (is_background) background = std::move(layer);
            if (is_foreground) foreground = std::move(layer);
        }
        if (note) {
            *note = monochrome ? "adaptive icon (monochrome)" : "adaptive icon";
        }
        if (!background.valid() && !foreground.valid()) {
            if (note) note->append(" (");
                note->append("adaptive icon (layers could not be decoded)");
                note->append(")");
            return {};
        }
        return compose_adaptive_icon(background, foreground, max_side);
    }

    if (root_name == "vector") {
        return vector_drawable_to_bitmap(*root, table, preference, max_side, note);
    }

    if (root_name == "bitmap" || root_name == "inset" || root_name == "nine-patch") {
        if (auto reference = attr_reference(*root, "src")) {
            return drawable_to_bitmap(zip, table, *reference, preference, note, depth + 1, max_side);
        }
        if (auto reference = attr_reference(*root, "drawable")) {
            return drawable_to_bitmap(zip, table, *reference, preference, note, depth + 1, max_side);
        }
        return {};
    }

    if (root_name == "scale") {
        // <scale drawable="@..." scaleWidth="30%" scaleHeight="30%" scaleGravity="center">
        // Draw the inner drawable into a canvas of its own size, so the surrounding
        // composition still has the geometry the icon author intended.
        Bitmap inner;
        if (auto reference = attr_reference(*root, "drawable")) {
            inner = drawable_to_bitmap(zip, table, *reference, preference, note, depth + 1, max_side);
        }
        if (!inner.valid()) {
            for (const Element& nested : root->children) {
                if (auto reference = attr_reference(nested, "drawable")) {
                    inner = drawable_to_bitmap(zip, table, *reference, preference, note, depth + 1, max_side);
                    if (inner.valid()) break;
                }
            }
        }
        if (!inner.valid()) return {};

        const float scale_x = percent_attribute(*root, "scaleWidth", 1.0f);
        const float scale_y = percent_attribute(*root, "scaleHeight", 1.0f);
        const int target_w = std::max(1, static_cast<int>(std::lround(inner.width * scale_x)));
        const int target_h = std::max(1, static_cast<int>(std::lround(inner.height * scale_y)));

        // android:scaleGravity: low nibble = x (1 centre, 3 left, 5 right),
        // high nibble = y (1 centre, 3 top, 5 bottom). 0x11 (centre) is the default.
        const int gravity = static_cast<int>(attr_int(*root, "scaleGravity", table, preference, 0x11));
        const int gx = gravity & 0x0F;
        const int gy = (gravity >> 4) & 0x0F;
        int offset_x = (inner.width - target_w) / 2;
        int offset_y = (inner.height - target_h) / 2;
        if (gx == 3) offset_x = 0;
        if (gx == 5) offset_x = inner.width - target_w;
        if (gy == 3) offset_y = 0;
        if (gy == 5) offset_y = inner.height - target_h;

        Bitmap canvas = Bitmap::solid(inner.width, inner.height, 0, 0, 0, 0);
        blit_scaled(&canvas, inner, offset_x, offset_y, target_w, target_h);
        return canvas;
    }

    if (root_name == "layer-list") {
        // The top-most layer that decodes wins (that is what is visible).
        Bitmap result;
        for (const Element& item : root->children) {
            if (item.name != "item") continue;
            const uint32_t reference = attr_reference(item, "drawable").value_or(0);
            if (!reference) continue;
            Bitmap layer = drawable_to_bitmap(zip, table, reference, preference, note, depth + 1, max_side);
            if (layer.valid()) result = std::move(layer);
        }
        return result;
    }

    if (root_name == "color") {
        bool known = false;
        const uint32_t argb = color_from_element(*root, "color", table, preference, &known);
        if (known) {
            return Bitmap::solid(1, 1, static_cast<uint8_t>(argb >> 16), static_cast<uint8_t>(argb >> 8),
                                 static_cast<uint8_t>(argb), static_cast<uint8_t>(argb >> 24));
        }
        return {};
    }

    if (note) *note = "drawable <" + root_name + "> cannot be rasterised";
    return {};
}

Bitmap drawable_to_bitmap(const ZipFile& zip, const arsc::Table* table, uint32_t resource_id,
                          const ConfigPreference& preference, std::string* note, int depth,
                          int max_side) {
    if (depth > 8 || resource_id == 0) return {};
    if (table) {
        auto resolved = table->resolve(resource_id, preference);
        if (resolved) {
            switch (resolved->type) {
                case arsc::ValueType::String: {
                    if (!resolved->string) return {};
                    std::vector<uint8_t> bytes;
                    std::string error;
                    if (!zip.read(*resolved->string, bytes, &error)) {
                        return {};
                    }
                    if (sniff_image_format(bytes) == "xml") {
                        auto document = axml::parse(bytes);
                        if (!document.ok) return {};
                        return xml_drawable_to_bitmap(zip, document.doc, table, preference, note, depth + 1,
                                                      max_side);
                    }
                    return decode_image(bytes, &error).value_or(Bitmap{});
                }
                case arsc::ValueType::ColorArgb8:
                case arsc::ValueType::ColorRgb8:
                case arsc::ValueType::ColorArgb4:
                case arsc::ValueType::ColorRgb4:
                case arsc::ValueType::IntDec:
                case arsc::ValueType::IntHex: {
                    const uint32_t argb = resolved->data;
                    return Bitmap::solid(1, 1, static_cast<uint8_t>(argb >> 16), static_cast<uint8_t>(argb >> 8),
                                         static_cast<uint8_t>(argb & 0xFF), static_cast<uint8_t>(argb >> 24));
                }
                default:
                    break;
            }
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Split bundle helpers
// ---------------------------------------------------------------------------

// Describes a split member in two parts: `key` is an English dictionary key the
// presenter translates, `extra` is a technical qualifier that is never
// translated (config.xxxhdpi, arm64_v8a, zh, ...).
struct SplitKind {
    std::string key;
    std::string extra;
};

SplitKind describe_split(std::string_view entry_name) {
    std::string name = basename(entry_name);
    if (ends_with(name, ".apk")) name.resize(name.size() - 4);
    if (name == "base" || name == "base-master" || name == "base.apk") return {"base", {}};
    if (starts_with(name, "split_")) name = name.substr(6);

    std::string qualifier;
    if (starts_with(name, "config.")) qualifier = name.substr(7);

    if (!qualifier.empty()) {
        static const std::set<std::string> abis = {"armeabi", "armeabi_v7a", "arm64_v8a", "x86", "x86_64", "mips", "mips64"};
        static const std::set<std::string> densities = {"ldpi", "mdpi", "hdpi", "xhdpi", "xxhdpi", "xxxhdpi",
                                                       "tvdpi", "nodpi", "anydpi"};
        std::string normalized = qualifier;
        std::replace(normalized.begin(), normalized.end(), '-', '_');
        if (abis.count(normalized)) return {"ABI", normalized};
        if (densities.count(normalized)) return {"density", normalized};
        return {"locale", qualifier};
    }
    if (ends_with(name, "_installtime")) return {"feature (install time)", {}};
    if (ends_with(name, "_ondemand")) return {"feature (on demand)", {}};
    if (ends_with(name, "_fastfollow")) return {"feature (fast follow)", {}};
    return {"feature", {}};
}

// ---------------------------------------------------------------------------
// APK archive holder (handles the "apk inside a bundle" case)
// ---------------------------------------------------------------------------

class ApkArchive {
public:
    bool open(std::span<const uint8_t> file, const Detection& detection, std::string* error) {
        outer_map_ = file;
        if (detection.kind != ContainerKind::AndroidSplitBundle) {
            outer_ = ZipFile::open(file, error);
            if (!outer_) return false;
            zip_ = &*outer_;
            return true;
        }
        auto outer = ZipFile::open(file, error);
        if (!outer) return false;
        outer_ = std::move(outer);

        const std::string base_entry = detection.base_apk_entry;
        if (base_entry.empty()) {
            if (error) *error = "the bundle does not contain a base apk";
            return false;
        }
        const ZipFile::Entry* entry = outer_->find(base_entry);
        if (!entry) {
            if (error) *error = "bundle entry disappears: " + base_entry;
            return false;
        }
        auto temp_path = make_temp_file();
        if (!temp_path) {
            if (error) *error = "cannot create a temporary file for the base apk";
            return false;
        }
        temp_.path = *temp_path;
        if (!outer_->extract_to_file(*entry, temp_.path, error)) return false;
        auto map = FileMap::open(temp_.path, error);
        if (!map) return false;
        base_map_ = std::move(*map);
        inner_ = ZipFile::open(base_map_.bytes(), error);
        if (!inner_) return false;
        zip_ = &*inner_;
        return true;
    }

    const ZipFile& zip() const { return *zip_; }
    bool is_bundle() const { return inner_.has_value(); }
    const ZipFile& outer_zip() const { return *outer_; }
    // The bytes the apk zip was parsed from: the file itself for a single apk,
    // the extracted base apk for a bundle.
    std::span<const uint8_t> apk_bytes() const { return inner_ ? base_map_.bytes() : outer_map_; }

private:
    TempFile temp_;
    FileMap base_map_;
    std::span<const uint8_t> outer_map_;
    std::optional<ZipFile> outer_;
    std::optional<ZipFile> inner_;
    const ZipFile* zip_ = nullptr;
};

// Looks for the APK Signing Block in front of the central directory and reports
// which signature schemes it carries (v2: 0x7109871a, v3: 0xf05368c0,
// v3.1: 0x1b93ad61, source stamp: 0x2b09189e).
std::vector<std::string> apk_signing_schemes(std::span<const uint8_t> file, uint64_t cd_offset) {
    std::vector<std::string> schemes;
    constexpr size_t kMagicSize = 16;
    if (cd_offset < 32 || cd_offset > file.size()) return schemes;
    const size_t magic_offset = static_cast<size_t>(cd_offset) - kMagicSize;
    if (std::memcmp(file.data() + magic_offset, "APK Sig Block 42", kMagicSize) != 0) return schemes;

    // size-of-block (uint64) sits 24 bytes before the central directory.
    const size_t size_offset = static_cast<size_t>(cd_offset) - 24;
    if (size_offset > file.size()) return schemes;
    // Block layout:  [uint64 size][pairs ...][uint64 size][16 byte magic]
    // where the stored value is the block length minus 8, so the block starts
    // `stored_size + 8` bytes before the central directory and the pairs begin
    // 8 bytes into it. (Verified against a real v2-signed apk.)
    uint64_t stored_size = 0;
    std::memcpy(&stored_size, file.data() + size_offset, sizeof stored_size);
    if (stored_size < 24 || stored_size + 8 > static_cast<uint64_t>(cd_offset)) return schemes;
    const size_t block_start = static_cast<size_t>(cd_offset - stored_size - 8);
    size_t cursor = block_start + 8;
    const size_t block_end = size_offset;
    if (block_start + 8 > block_end) return schemes;
    while (cursor + 12 <= block_end) {
        uint64_t pair_size = 0;
        std::memcpy(&pair_size, file.data() + cursor, sizeof pair_size);
        if (pair_size < 4 || pair_size > block_end - cursor) break;
        uint32_t id = 0;
        std::memcpy(&id, file.data() + cursor + 8, sizeof id);
        switch (id) {
            case 0x7109871a:
                schemes.push_back("v2");
                break;
            case 0xf05368c0:
                schemes.push_back("v3");
                break;
            case 0x1b93ad61:
                schemes.push_back("v3.1");
                break;
            case 0x2b09189e:
                schemes.push_back("source stamp");
                break;
            default:
                break;
        }
        cursor += static_cast<size_t>(pair_size) + 8;
    }
    return schemes;
}

}  // namespace

InfoDocument read_android_package(std::span<const uint8_t> file, const Detection& detection,
                                  const std::string& display_path, const AndroidOptions& options) {
    InfoDocument document;
    document.kind = detection.kind;
    document.file_path = display_path;
    document.file_name = basename(display_path);
    document.file_size = file.size();
    document.badge = detection.kind == ContainerKind::AndroidSplitBundle ? "Split bundle" : "Android APK";

    // NOTE: several sections below keep a reference (package_section,
    // app_section, ...) and are filled after other sections have been appended.
    // Reserving up front keeps those references valid: the vector never
    // reallocates while size() <= capacity().
    document.sections.reserve(32);

    std::string error;
    ApkArchive archive;
    if (!archive.open(file, detection, &error)) {
        document.error = error.empty() ? "cannot open" : error;
        return document;
    }
    const ZipFile& zip = archive.zip();

    // --- manifest -----------------------------------------------------------
    std::vector<uint8_t> manifest_bytes;
    if (!zip.read("AndroidManifest.xml", manifest_bytes, &error)) {
        document.error = std::string("AndroidManifest.xml: ") + error;
        return document;
    }
    axml::Result manifest = axml::parse(manifest_bytes);
    if (!manifest.ok || !manifest.doc.root()) {
        document.error = std::string("AndroidManifest.xml: ") + "(" +
                         (manifest.error.empty() ? std::string("no root element") : manifest.error) + ")";
        return document;
    }
    const Element& root = *manifest.doc.root();
    if (root.name != "manifest") {
        document.warnings.push_back("manifest root element is <" + root.name + ">, not <manifest>");
    }

    // --- resource table -----------------------------------------------------
    std::optional<arsc::Table> table;
    std::vector<uint8_t> resources_bytes;
    if (zip.find("resources.arsc")) {
        if (zip.read("resources.arsc", resources_bytes, &error)) {
            table = arsc::Table::parse(resources_bytes);
            if (!table) document.warnings.push_back(std::string("resources.arsc could not be parsed") + " (" + error + ")");
        } else {
            document.warnings.push_back("resources.arsc could not be read: " + error);
        }
    } else {
        document.warnings.push_back("the package has no resources.arsc (resource references cannot be resolved)");
    }
    const arsc::Table* resources = table ? &*table : nullptr;

    ConfigPreference text_preference;
    text_preference.language = options.language;
    text_preference.region = options.region;
    text_preference.sdk_version = 34;
    text_preference.prefer_default = options.language.empty();

    ConfigPreference icon_preference = text_preference;
    icon_preference.density = options.icon_density;
    icon_preference.prefer_default = false;

    // --- identity -----------------------------------------------------------
    const std::string package_name = attr_string(root, "package", resources, text_preference);
    const Element* application = root.child("application");
    const Element* uses_sdk = root.child("uses-sdk");

    std::string label;
    std::string label_source = "manifest";
    if (application) {
        if (auto reference = attr_reference(*application, "label"); reference && resources) {
            if (auto resolved = resources->resolve_string(*reference, text_preference)) {
                label = *resolved;
                label_source = "resources.arsc (android:label)";
            }
        }
        if (label.empty()) {
            label = attr_string(*application, "label", resources, text_preference);
            if (!label.empty()) label_source = "manifest literal";
        }
    }
    if (label.empty()) {
        label = package_name;
        label_source = "package name (no label resource)";
    }

    document.title = label;
    const std::string version_name = attr_string(root, "versionName", resources, text_preference);
    const int64_t version_code = attr_int(root, "versionCode", resources, text_preference, 0);
    std::string subtitle = package_name;
    if (!version_name.empty()) subtitle += " · " + version_name;
    if (version_code != 0) subtitle += " (" + std::to_string(version_code) + ")";
    document.subtitle = subtitle;

    // --- sections -----------------------------------------------------------
    InfoSection& package_section = document.sections.emplace_back();
    package_section.title = "Package";
    package_section.add("Application name", label, "Resolved from " + label_source);
    package_section.add("Package name", package_name, "Application id", true);
    package_section.add("Version name", version_name.empty() ? "(none)" : version_name);
    package_section.add("Version code", version_code != 0 ? std::to_string(version_code) : "(none)");
    package_section.add("Package type",
                        detection.kind == ContainerKind::AndroidSplitBundle ? "Split APK bundle" : "Single APK",
                        detection.reason);
    package_section.add("File name", document.file_name);
    package_section.add("File size", human_size(document.file_size));
    if (archive.is_bundle()) {
        package_section.add("Base APK", detection.base_apk_entry, "Entry inside the bundle", true);
    }

    // SDK
    InfoSection& sdk_section = document.sections.emplace_back();
    sdk_section.title = "SDK";
    const int64_t min_sdk = uses_sdk ? attr_int(*uses_sdk, "minSdkVersion", resources, text_preference, 0) : 0;
    const int64_t target_sdk = uses_sdk ? attr_int(*uses_sdk, "targetSdkVersion", resources, text_preference, 0) : 0;
    const int64_t compile_sdk = attr_int(root, "compileSdkVersion", resources, text_preference, 0);
    const std::string min_codename = uses_sdk ? attr_string(*uses_sdk, "minSdkVersion", resources, text_preference) : "";
    sdk_section.add("Minimum SDK", sdk_text(min_sdk, min_codename.empty() || min_sdk > 0 ? "" : min_codename));
    sdk_section.add("Target SDK", sdk_text(target_sdk));
    if (compile_sdk > 0) sdk_section.add("Compile SDK", sdk_text(compile_sdk));
    const std::string platform_build_version = attr_string(root, "platformBuildVersionName", resources, text_preference);
    if (!platform_build_version.empty()) {
        sdk_section.add("Platform build version", platform_build_version);
    }

    // --- content statistics (from the zip listing) --------------------------
    std::set<std::string> abis;
    std::set<std::string> densities;
    std::set<std::string> locales;
    size_t dex_count = 0;
    uint64_t dex_size = 0;
    uint64_t resource_size = 0;
    uint64_t asset_size = 0;
    uint64_t native_size = 0;
    size_t signatures_v1 = 0;

    // Entries are only *counted* here and pushed into the tree builder: the tree
    // keeps names, sizes and node structure, never the whole listing as rows.
    TreeBuilder tree;
    for (const ZipFile::Entry& entry : zip.entries()) {
        uint8_t node_detail = 0;   // TreeNode::detail: 2 dex, 3 native lib, 4 resource, 5 asset

        if (starts_with(entry.name, "lib/")) {
            const std::vector<std::string> parts = split(entry.name, '/');
            if (parts.size() >= 3) {
                abis.insert(parts[1]);
                node_detail = 3;
            }
            native_size += entry.uncompressed_size;
        } else if (starts_with(entry.name, "res/")) {
            const std::string directory = dirname(entry.name);
            const std::vector<std::string> parts = split(directory, '/');
            for (size_t i = 1; i < parts.size(); ++i) {
                const std::string& part = parts[i];
                const size_t dash = part.find('-');
                const std::string base = dash == std::string::npos ? part : part.substr(0, dash);
                if (dash == std::string::npos) continue;
                for (const std::string& qualifier : split(part.substr(dash + 1), '-')) {
                    if (qualifier == "ldpi" || qualifier == "mdpi" || qualifier == "tvdpi" ||
                        qualifier == "hdpi" || qualifier == "xhdpi" || qualifier == "xxhdpi" ||
                        qualifier == "xxxhdpi" || qualifier == "nodpi" || qualifier == "anydpi") {
                        densities.insert(qualifier);
                    } else if (base == "values" &&
                               qualifier.size() <= 3 && qualifier != "v21" && qualifier[0] >= 'a' &&
                               qualifier[0] <= 'z') {
                        locales.insert(qualifier);
                    }
                }
            }
            node_detail = 4;
            resource_size += entry.uncompressed_size;
        } else if (starts_with(entry.name, "assets/")) {
            node_detail = 5;
            asset_size += entry.uncompressed_size;
        } else if (starts_with(entry.name, "META-INF/")) {
            if (ends_with(entry.name, ".SF") || ends_with(entry.name, ".RSA") ||
                ends_with(entry.name, ".DSA") || ends_with(entry.name, ".EC")) {
                ++signatures_v1;
            }
        } else if (starts_with(entry.name, "classes") && ends_with(entry.name, ".dex")) {
            node_detail = 2;
            ++dex_count;
            dex_size += entry.uncompressed_size;
        }
        if (entry.is_dir) {
            tree.add_dir(entry.name);
        } else {
            tree.add_file(entry.name, entry.uncompressed_size, node_detail);
        }
    }

    // --- device support -----------------------------------------------------
    InfoSection& support_section = document.sections.emplace_back();
    support_section.title = "Device support";
    // Split bundles carry their configuration in the member names
    // (split_config.xxxhdpi.apk, split_config.zh.apk, split_config.arm64_v8a.apk)
    // so those qualifiers are visible even when res/ names are obfuscated.
    std::vector<std::string> split_abis;
    {
        static const std::set<std::string> known_abis = {"armeabi", "armeabi_v7a", "arm64_v8a",
                                                         "x86",     "x86_64",      "mips", "mips64"};
        for (const ZipFile::Entry* entry : archive.is_bundle() ? archive.outer_zip().find_suffix(".apk")
                                                               : std::vector<const ZipFile::Entry*>{}) {
            const SplitKind split_kind = describe_split(entry->name);
            if (split_kind.key == "density") {
                densities.insert(split_kind.extra);
            } else if (split_kind.key == "locale") {
                locales.insert(split_kind.extra);
            } else if (split_kind.key == "ABI") {
                split_abis.push_back(split_kind.extra);
            }
        }
        for (const std::string& abi : split_abis) abis.insert(abi);
    }

    // The resource table knows every density/locale the package actually ships,
    // even when res/ directory names are obfuscated away (which is the normal
    // case for a shrunk release build). This is the authoritative source.
    if (resources) {
        for (const arsc::Config& config : resources->configurations()) {
            if (config.density != 0) densities.insert(density_label(config.density));
            if (!config.language.empty()) {
                locales.insert(config.language +
                               (config.region.empty() ? std::string() : "-r" + config.region));
            }
        }
    }

    if (!abis.empty()) support_section.add("Native platforms (ABIs)", join_set(abis, ", "),
                                           "Taken from the lib/ folders");
    if (!densities.empty()) {
        support_section.add("Screen densities", join_set(densities, ", "),
                            "Density configurations present in resources.arsc");
    }
    if (!locales.empty()) {
        std::vector<std::string> sorted(locales.begin(), locales.end());
        std::sort(sorted.begin(), sorted.end());
        // The count goes into the summary row; the full list is its own collapsible
        // section (see below), so nothing is hidden behind an ellipsis.
        support_section.add("Locales", std::to_string(sorted.size()),
                            "Language configurations present in resources.arsc");
    }
    for (const Element* feature : root.children_named("uses-feature")) {
        const axml::Attribute* gl = feature->attribute_by_res(kAttrGlEsVersion);
        if (!gl) continue;
        const uint32_t raw = gl->value.data;
        const int major = static_cast<int>(raw >> 16);
        const int minor = static_cast<int>(raw & 0xFFFF);
        support_section.add("OpenGL ES", std::to_string(major) + "." + std::to_string(minor),
                            "android:glEsVersion of uses-feature");
        break;
    }
    if (const Element* screens = root.child("supports-screens")) {
        std::vector<std::string> supported;
        for (const char* name : {"smallScreens", "normalScreens", "largeScreens", "xlargeScreens"}) {
            if (attr_flag(*screens, name).value_or(true)) supported.emplace_back(name);
        }
        if (!supported.empty()) support_section.add("Supported screen sizes", join(supported, ", "));
    }

    // --- application --------------------------------------------------------
    InfoSection& app_section = document.sections.emplace_back();
    app_section.title = "Application";
    std::string main_activity;
    size_t activity_count = 0, service_count = 0, receiver_count = 0, provider_count = 0, alias_count = 0;
    if (application) {
        for (const Element& child : application->children) {
            if (child.name == "activity") {
                ++activity_count;
                if (main_activity.empty()) {
                    for (const Element* filter : child.children_named("intent-filter")) {
                        bool has_main = false, has_launcher = false;
                        for (const Element* action : filter->children_named("action")) {
                            if (attr_string(*action, "name", nullptr, text_preference) == "android.intent.action.MAIN") {
                                has_main = true;
                            }
                        }
                        for (const Element* category : filter->children_named("category")) {
                            const std::string name = attr_string(*category, "name", nullptr, text_preference);
                            if (name == "android.intent.category.LAUNCHER" ||
                                name == "android.intent.category.DEFAULT") {
                                if (name == "android.intent.category.LAUNCHER") has_launcher = true;
                            }
                        }
                        if (has_main && has_launcher) {
                            const std::string name = attr_string(child, "name", nullptr, text_preference);
                            main_activity = name.empty() ? "(unnamed)" : name;
                        }
                    }
                }
            } else if (child.name == "activity-alias") {
                ++alias_count;
            } else if (child.name == "service") {
                ++service_count;
            } else if (child.name == "receiver") {
                ++receiver_count;
            } else if (child.name == "provider") {
                ++provider_count;
            }
        }
    }
    if (!main_activity.empty()) app_section.add("Main activity", main_activity, "", true);
    std::vector<std::string> components;
    if (activity_count) components.push_back(std::to_string(activity_count) + " " + "activities");
    if (alias_count) components.push_back(std::to_string(alias_count) + " " + "aliases");
    if (service_count) components.push_back(std::to_string(service_count) + " " + "services");
    if (receiver_count) components.push_back(std::to_string(receiver_count) + " " + "receivers");
    if (provider_count) components.push_back(std::to_string(provider_count) + " " + "providers");
    if (!components.empty()) app_section.add("Components", join(components, ", "));

    if (application) {
        if (auto flag = attr_flag(*application, "debuggable")) {
            app_section.add("Debuggable", *flag ? "yes" : "no");
        }
        if (auto flag = attr_flag(*application, "allowBackup")) {
            app_section.add("Backup allowed", *flag ? "yes" : "no");
        }
        if (auto flag = attr_flag(*application, "extractNativeLibs")) {
            app_section.add("extractNativeLibs", *flag ? "yes" : "no",
                            "When no, native libraries are loaded straight from the apk");
        }
        const std::string theme = attr_string(*application, "theme", resources, text_preference);
        if (!theme.empty()) app_section.add("Theme", theme, "", true);
    }
    const int64_t install_location = attr_int(root, "installLocation", resources, text_preference, 0);
    if (install_location > 0) {
        app_section.add("Install location",
                        install_location == 1 ? "internal only"
                                     : (install_location == 2 ? "prefer external" : "auto"));
    }
    // Split related attributes (only on split apks).
    for (const char* name : {"split", "configForSplit", "requiredSplitTypes", "splitTypes"}) {
        const std::string value = attr_string(root, name, resources, text_preference);
        if (!value.empty()) app_section.add(std::string("manifest ") + name, value, "", true);
    }
    if (auto required = attr_flag(root, "isSplitRequired"); required && *required) {
        app_section.add("Requires split apks", "yes");
    }

    // --- signatures & payload ----------------------------------------------
    InfoSection& payload_section = document.sections.emplace_back();
    payload_section.title = "Contents summary";
    payload_section.add("Entries", with_thousands(zip.entries().size()));
    payload_section.add("Uncompressed size", human_size(zip.total_uncompressed_size()));
    if (dex_count) {
        payload_section.add("DEX files", std::to_string(dex_count) + " · " + human_size(dex_size));
    }
    if (resource_size) payload_section.add("Resources", human_size(resource_size));
    if (asset_size) payload_section.add("Assets", human_size(asset_size));
    if (native_size) payload_section.add("Native libraries", human_size(native_size));

    std::vector<std::string> schemes;
    if (signatures_v1) schemes.push_back("v1 (JAR)");
    for (const std::string& scheme : apk_signing_schemes(archive.apk_bytes(), zip.central_directory_offset())) {
        schemes.push_back(scheme);
    }
    if (!schemes.empty()) {
        payload_section.add("Signature schemes", join(schemes, ", "),
                            "v1 = JAR signing, v2/v3 = APK Signature Scheme");
    } else {
        payload_section.add("Signature schemes", "none detected");
    }

    // --- permissions --------------------------------------------------------
    std::vector<std::string> permissions;
    for (const Element* permission : root.children_named("uses-permission")) {
        const std::string name = attr_string(*permission, "name", nullptr, text_preference);
        if (!name.empty()) permissions.push_back(name);
    }
    for (const Element* permission : root.children_named("uses-permission-sdk-23")) {
        const std::string name = attr_string(*permission, "name", nullptr, text_preference);
        if (!name.empty()) permissions.push_back(name + "  " + "(sdk 23+)");
    }
    if (!permissions.empty()) {
        InfoSection& section = document.sections.emplace_back();
        section.title = "Permissions";
        section.count = permissions.size();
        section.collapsible = true;
        std::sort(permissions.begin(), permissions.end());
        for (const std::string& permission : permissions) {
            section.add("", permission, "", true);
        }
    }

    // --- features -----------------------------------------------------------
    std::vector<std::string> features;
    for (const Element* feature : root.children_named("uses-feature")) {
        std::string name = attr_string(*feature, "name", nullptr, text_preference);
        if (name.empty()) {
            const axml::Attribute* gl = feature->attribute_by_res(kAttrGlEsVersion);
            if (gl) {
                name = "glEsVersion " + std::to_string(gl->value.data >> 16) + "." +
                       std::to_string(gl->value.data & 0xFFFF);
            }
        }
        if (name.empty()) continue;
        if (auto required = attr_flag(*feature, "required"); required && !*required) {
            name += std::string("  ") + "(optional)";
        }
        features.push_back(name);
    }
    if (!features.empty()) {
        InfoSection& section = document.sections.emplace_back();
        section.title = "Features";
        section.count = features.size();
        section.collapsible = true;
        std::sort(features.begin(), features.end());
        for (const std::string& feature : features) section.add("", feature, "", true);
    }

    // --- localized application names ---------------------------------------
    if (application && resources) {
        if (auto label_reference = attr_reference(*application, "label")) {
            const std::vector<arsc::Entry> entries = resources->entries(*label_reference);
            std::vector<std::string> names;
            for (const arsc::Entry& entry : entries) {
                if (entry.config.language.empty() || !entry.string) continue;
                std::string shown = entry.config.to_string();
                names.push_back(shown + ": " + one_line(*entry.string));
            }
            if (names.size() > 1) {
                std::sort(names.begin(), names.end());
                InfoSection& section = document.sections.emplace_back();
                section.title = "Localized names";
        section.count = names.size();
        section.collapsible = true;
                // No truncation here: collapsing is a presentation concern, and the
                // old cap left a bogus "... N more" line as the last entry of the
                // section - it also showed up inside the expanded list.
                for (const std::string& name : names) section.add("", name, "", true, true);
            }
        }
    }

    // --- launcher icon ------------------------------------------------------
    if (options.read_icon) {
        uint32_t icon_id = 0;
        std::string icon_source = "android:icon";
        if (application) {
            if (auto reference = attr_reference(*application, "icon")) {
                icon_id = *reference;
            } else if (auto round_reference = attr_reference(*application, "roundIcon")) {
                icon_id = *round_reference;
                icon_source = "android:roundIcon";
            }
        }
        if (icon_id) {
            std::string note;
            Bitmap icon = drawable_to_bitmap(zip, resources, icon_id, icon_preference, &note, 0,
                                             options.icon_max_side);
            // In a split bundle a drawable can live only in a configuration
            // split (that is exactly how Google Play services ships its
            // launcher icon: the base apk has the *reference*, the density
            // split has the bitmaps). Fall back to the splits.
            bool icon_from_split = false;
            if (!icon.valid() && archive.is_bundle()) {
                for (const ZipFile::Entry* entry : archive.outer_zip().find_suffix(".apk")) {
                    if (entry->name == detection.base_apk_entry) continue;
                    if (entry->uncompressed_size > 128ull * 1024 * 1024) continue;
                    auto temp_path = make_temp_file();
                    if (!temp_path) break;
                    TempFile candidate_temp;
                    candidate_temp.path = *temp_path;
                    std::string split_error;
                    if (!archive.outer_zip().extract_to_file(*entry, candidate_temp.path, &split_error)) continue;
                    auto split_map = FileMap::open(candidate_temp.path, &split_error);
                    if (!split_map) continue;
                    auto split_zip = ZipFile::open(split_map->bytes(), &split_error);
                    if (!split_zip) continue;
                    std::vector<uint8_t> split_arsc;
                    if (!split_zip->read("resources.arsc", split_arsc, &split_error)) continue;
                    auto split_table = arsc::Table::parse(split_arsc);
                    if (!split_table || !split_table->has(icon_id)) continue;
                    std::string split_note;
                    Bitmap split_icon =
                        drawable_to_bitmap(*split_zip, &*split_table, icon_id, icon_preference, &split_note, 0,
                                           options.icon_max_side);
                    if (split_icon.valid()) {
                        icon = std::move(split_icon);
                        note = split_note.empty() ? "from " + basename(entry->name)
                                                  : split_note + ", from " + basename(entry->name);
                        icon_from_split = true;
                        break;
                    }
                }
            }
            if (icon.valid()) {
                document.icon = std::move(icon);
                document.icon_notes.push_back(icon_source);
                if (!note.empty()) document.icon_notes.push_back(note);
            } else {
                document.icon_notes.push_back(icon_source);
                document.icon_notes.push_back("could not be decoded");
                if (!note.empty()) document.icon_notes.push_back(note);
                char id_text[16];
                std::snprintf(id_text, sizeof id_text, "0x%08X", icon_id);
                document.warnings.push_back(std::string("launcher icon resource ") + id_text +
                                            " could not be turned into an image");
            }
            // Report where the icon lives, which is handy when debugging.
            if (resources && !icon_from_split) {
                if (auto path = resources->resolve_string(icon_id, icon_preference)) {
                    app_section.add("Icon resource", *path, "Path inside the apk (resolved through resources.arsc)",
                                    true, true);
                }
            }
        }
    }

    // --- bundle split listing ----------------------------------------------
    // The splits stay structured (key + qualifier); the presenter turns them into
    // a localised section and a tab, so switching language needs no re-read.
    if (archive.is_bundle()) {
        document.contents_kind = ContentsKind::Splits;
        for (const ZipFile::Entry* entry : archive.outer_zip().find_suffix(".apk")) {
            ContentItem item;
            item.name = entry->name;
            item.size = entry->uncompressed_size;
            item.packed_size = entry->compressed_size;
            item.kind = "split";
            const SplitKind split_kind = describe_split(entry->name);
            item.detail_key = split_kind.key;
            item.detail_extra = split_kind.extra;
            document.splits.push_back(std::move(item));
        }
    } else if (options.read_contents) {
        document.contents_kind = ContentsKind::Files;
        document.file_count = zip.entries().size();
        document.tree = tree.finish(document.file_name);
    }

    // SHA-256 is deliberately *not* computed here: hashing a 100 MB+ package
    // takes a second and pulls the whole mapping into the working set. The app
    // fills document.file_hash on a worker thread after the window is up.

    // Appended last: appending a section may reallocate the vector and invalidate
    // any InfoSection& held above, so this cannot sit inside the device-support block.
    if (locales.size() > 1) {
        std::vector<std::string> sorted(locales.begin(), locales.end());
        std::sort(sorted.begin(), sorted.end());
        InfoSection& locale_section = document.sections.emplace_back();
        locale_section.title = "Locales";
        locale_section.count = sorted.size();
        locale_section.collapsible = true;
        for (const std::string& locale : sorted) locale_section.add("", locale, "", true);
    }

    document.ok = true;
    return document;
}

}  // namespace apkinfo