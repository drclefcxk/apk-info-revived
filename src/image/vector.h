// Minimal SVG-path rasteriser for Android <vector> drawables.
//
// Modern launcher icons are usually an <adaptive-icon> whose background and
// foreground are <vector> XML (with <group> transforms and <path> data). Without a
// path rasteriser those icons decoded to nothing, which is why some packages showed
// no preview at all.
//
// The implementation is deliberately small: it flattens every SVG path command
// (including elliptical arcs and the smooth-curve variants) into line segments and
// fills/strokes them with 4x4 supersampling. That is enough for launcher artwork and
// keeps the parser free of any XML or resource-table knowledge.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "model/info_document.h"

namespace apkinfo {
namespace vector {

struct Point {
    float x = 0;
    float y = 0;
};

// One flattened subpath: a polyline of device-independent coordinates.
struct Subpath {
    std::vector<Point> points;
    bool closed = false;
};

// 2D affine transform: (x, y) -> (a*x + c*y + e, b*x + d*y + f).
struct Transform {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    static Transform identity() { return {}; }
    static Transform translate(float tx, float ty);
    static Transform scale(float sx, float sy);
    static Transform rotate(float degrees, float pivot_x, float pivot_y);

    // Returns `this` applied after `inner` (i.e. inner first, then this).
    Transform concat(const Transform& inner) const;
    Point apply(Point point) const;
};

// Parses SVG path data (M/L/H/V/C/S/Q/T/A/Z, absolute and relative) into subpaths.
// Curves and arcs are flattened; `tolerance` is the maximum deviation in user units.
std::vector<Subpath> parse_path(std::string_view data, float tolerance = 0.15f);

// Fills the subpaths into `canvas` (premultiplied RGBA8, straight alpha stored) with
// 4x4 supersampling. `argb` is 0xAARRGGBB; `even_odd` selects the fill rule.
void fill(Bitmap* canvas, const std::vector<Subpath>& subpaths, const Transform& transform, uint32_t argb,
          bool even_odd);

// Strokes the subpaths with `width` user units (round joins and caps, approximated).
void stroke(Bitmap* canvas, const std::vector<Subpath>& subpaths, const Transform& transform, uint32_t argb,
            float width);

}  // namespace vector
}  // namespace apkinfo
