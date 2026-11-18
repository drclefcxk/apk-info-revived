// Tests for the SVG-path rasteriser used by <vector> drawables (src/image/vector.cpp).
//
// Launcher icons are vector art; a broken arc or a wrong fill rule shows up as a
// squashed or hollow logo, so the checks measure the rendered pixels, not just that
// something was drawn.
#include <cmath>
#include <cstdio>
#include <string>

#include "image/vector.h"
#include "model/info_document.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}

// Bounding box of pixels with any coverage.
struct Box {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    int covered = 0;
};

Box measure(const apkinfo::Bitmap& bitmap) {
    Box box;
    for (int y = 0; y < bitmap.height; ++y) {
        for (int x = 0; x < bitmap.width; ++x) {
            const uint8_t alpha = bitmap.rgba[(static_cast<size_t>(y) * static_cast<size_t>(bitmap.width) +
                                               static_cast<size_t>(x)) * 4 + 3];
            if (alpha < 8) continue;
            box.x0 = std::min(box.x0, x);
            box.y0 = std::min(box.y0, y);
            box.x1 = std::max(box.x1, x);
            box.y1 = std::max(box.y1, y);
            ++box.covered;
        }
    }
    return box;
}

void test_square_fill() {
    auto subpaths = apkinfo::vector::parse_path("M 0 0 H 10 V 10 H 0 Z");
    check(subpaths.size() == 1, "one subpath from a rectangle");
    check(subpaths[0].points.size() >= 5, "rectangle keeps its corners");
    check(subpaths[0].closed, "Z closes the subpath");

    apkinfo::Bitmap canvas = apkinfo::Bitmap::solid(100, 100, 0, 0, 0, 0);
    const apkinfo::vector::Transform scale = apkinfo::vector::Transform::scale(10, 10);
    apkinfo::vector::fill(&canvas, subpaths, scale, 0xFFFF0000u, false);
    const Box box = measure(canvas);
    check(box.x0 == 0 && box.y0 == 0 && box.x1 == 99 && box.y1 == 99, "10x10 square scaled to 100x100 fills the canvas");
    check(box.covered == 100 * 100, "every pixel of the square is covered");
    const uint8_t* pixel = &canvas.rgba[(50 * 100 + 50) * 4];
    check(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255, "fill colour is red");
}

void test_circle_arcs() {
    // The exact path tieba's launcher icon uses: a circle drawn with two arcs.
    auto subpaths = apkinfo::vector::parse_path("M24,24m-24,0a24,24 0,1 1,48 0a24,24 0,1 1,-48 0");
    check(!subpaths.empty(), "arc path parses");
    if (subpaths.empty()) return;
    check(subpaths[0].points.size() > 24, "arcs are flattened into many segments");

    apkinfo::Bitmap canvas = apkinfo::Bitmap::solid(96, 96, 0, 0, 0, 0);
    // 48 user units -> 96 px
    apkinfo::vector::fill(&canvas, subpaths, apkinfo::vector::Transform::scale(2, 2), 0xFF0000FFu, false);
    const Box box = measure(canvas);
    const int width = box.x1 - box.x0 + 1;
    const int height = box.y1 - box.y0 + 1;
    check(std::abs(width - height) <= 2, "the circle stays round (" + std::to_string(width) + "x" +
                                              std::to_string(height) + ")");
    check(width >= 92 && height >= 92, "the circle spans the canvas (" + std::to_string(width) + "x" +
                                           std::to_string(height) + ")");
    const double area = 3.14159265358979 * 48 * 48;
    const double ratio = box.covered / (96.0 * 96.0);
    check(std::fabs(ratio - area / (96.0 * 96.0)) < 0.03,
          "circle coverage matches pi r^2 (" + std::to_string(ratio) + ")");
    // the corners must stay empty
    check(canvas.rgba[3] == 0, "top-left corner is outside the circle");
    check(canvas.rgba[(95 * 96 + 95) * 4 + 3] == 0, "bottom-right corner is outside the circle");
}

void test_even_odd() {
    // Two nested squares: non-zero fills both, even-odd leaves a hole.
    auto subpaths = apkinfo::vector::parse_path("M0 0 H100 V100 H0 Z M25 25 H75 V75 H25 Z");
    check(subpaths.size() == 2, "two subpaths");

    apkinfo::Bitmap nonzero = apkinfo::Bitmap::solid(100, 100, 0, 0, 0, 0);
    apkinfo::vector::fill(&nonzero, subpaths, apkinfo::vector::Transform::identity(), 0xFF00FF00u, false);
    apkinfo::Bitmap evenodd = apkinfo::Bitmap::solid(100, 100, 0, 0, 0, 0);
    apkinfo::vector::fill(&evenodd, subpaths, apkinfo::vector::Transform::identity(), 0xFF00FF00u, true);

    // Same winding direction: non-zero fills the middle, even-odd does not.
    const uint8_t nonzero_centre = nonzero.rgba[(50 * 100 + 50) * 4 + 3];
    const uint8_t evenodd_centre = evenodd.rgba[(50 * 100 + 50) * 4 + 3];
    check(nonzero_centre > 200, "non-zero fill covers the inner square");
    check(evenodd_centre < 32, "even-odd fill leaves the inner square empty");
}

void test_curves_and_relative() {
    // Relative moves and a cubic curve: the shape must stay inside the canvas and
    // be filled (a broken parser returns nothing).
    auto subpaths = apkinfo::vector::parse_path("m10,10 c20,-20 40,20 60,0 l0,20 z");
    check(!subpaths.empty(), "relative commands parse");
    apkinfo::Bitmap canvas = apkinfo::Bitmap::solid(100, 100, 0, 0, 0, 0);
    if (!subpaths.empty()) {
        apkinfo::vector::fill(&canvas, subpaths, apkinfo::vector::Transform::identity(), 0xFF000000u, false);
    }
    const Box box = measure(canvas);
    check(box.covered > 100, "the curved shape covers a sensible area (" + std::to_string(box.covered) + ")");
    check(box.x0 >= 5 && box.x1 <= 95, "the curve stays inside the canvas");

    // a stroke produces pixels too
    apkinfo::Bitmap stroked = apkinfo::Bitmap::solid(100, 100, 0, 0, 0, 0);
    apkinfo::vector::stroke(&stroked, subpaths, apkinfo::vector::Transform::identity(), 0xFF000000u, 3.0f);
    check(measure(stroked).covered > 50, "stroke renders");
}

void test_transform() {
    using apkinfo::vector::Transform;
    const Transform rotate = Transform::rotate(90, 50, 50);
    const auto point = rotate.apply({50, 0});
    // 90 degrees clockwise in screen coordinates: (50,0) -> (100,50)
    check(std::fabs(point.x - 100.0f) < 0.5f && std::fabs(point.y - 50.0f) < 0.5f,
          "rotate() turns clockwise around the pivot");
    const Transform shifted = Transform::translate(10, 20).concat(Transform::scale(2, 2));
    const auto moved = shifted.apply({1, 1});
    check(std::fabs(moved.x - 12.0f) < 0.01f && std::fabs(moved.y - 22.0f) < 0.01f,
          "concat() applies the inner transform first");
}

}  // namespace

int main() {
    test_square_fill();
    test_circle_arcs();
    test_even_odd();
    test_curves_and_relative();
    test_transform();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
