#include "image/vector.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace apkinfo {
namespace vector {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kSuper = 4;  // supersampling factor per axis

float to_radians(float degrees) { return degrees * kPi / 180.0f; }

// --- number scanning -------------------------------------------------------

struct Scanner {
    std::string_view text;
    size_t position = 0;

    void skip_separators() {
        while (position < text.size()) {
            const char c = text[position];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
                ++position;
            } else {
                break;
            }
        }
    }

    bool peek_command() {
        skip_separators();
        if (position >= text.size()) return false;
        const char c = text[position];
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    char next_command() {
        skip_separators();
        return position < text.size() ? text[position++] : '\0';
    }

    // Reads one number; returns false when the text does not start with one.
    bool number(float* out) {
        skip_separators();
        if (position >= text.size()) return false;
        const char* begin = text.data() + position;
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end == begin) return false;
        position += static_cast<size_t>(end - begin);
        *out = static_cast<float>(value);
        return true;
    }

    // Arc flags are single digits, possibly without separators ("a1 1 0 1 1 2 2").
    bool flag(bool* out) {
        skip_separators();
        if (position >= text.size()) return false;
        const char c = text[position];
        if (c != '0' && c != '1') return false;
        *out = (c == '1');
        ++position;
        return true;
    }
};

// --- curve flattening ------------------------------------------------------

void add_point(Subpath& path, Point point) {
    if (!path.points.empty()) {
        const Point& last = path.points.back();
        if (std::fabs(last.x - point.x) < 1e-6f && std::fabs(last.y - point.y) < 1e-6f) return;
    }
    path.points.push_back(point);
}

void flatten_quadratic(Subpath& path, Point p0, Point p1, Point p2, int steps) {
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float mt = 1.0f - t;
        add_point(path, Point{mt * mt * p0.x + 2 * mt * t * p1.x + t * t * p2.x,
                              mt * mt * p0.y + 2 * mt * t * p1.y + t * t * p2.y});
    }
}

void flatten_cubic(Subpath& path, Point p0, Point p1, Point p2, Point p3, int steps) {
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float mt = 1.0f - t;
        const float a = mt * mt * mt;
        const float b = 3 * mt * mt * t;
        const float c = 3 * mt * t * t;
        const float d = t * t * t;
        add_point(path, Point{a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                              a * p0.y + b * p1.y + c * p2.y + d * p3.y});
    }
}

// Steps for a curve, from its control polygon length (enough for a smooth result).
int curve_steps(Point p0, Point p1, Point p2, Point p3, float tolerance) {
    const float length = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) +
                         std::hypot(p3.x - p2.x, p3.y - p2.y);
    const int steps = static_cast<int>(length / std::max(0.05f, tolerance));
    return std::clamp(steps, 2, 96);
}

// SVG elliptical arc (endpoint parameterisation) flattened into line segments.
void flatten_arc(Subpath& path, Point p0, float rx, float ry, float rotation_degrees, bool large_arc,
                 bool sweep, Point p1, float tolerance) {
    if (rx == 0.0f || ry == 0.0f) {
        add_point(path, p1);
        return;
    }
    rx = std::fabs(rx);
    ry = std::fabs(ry);

    const float cos_phi = std::cos(to_radians(rotation_degrees));
    const float sin_phi = std::sin(to_radians(rotation_degrees));

    const float dx2 = (p0.x - p1.x) / 2.0f;
    const float dy2 = (p0.y - p1.y) / 2.0f;
    const float x1 = cos_phi * dx2 + sin_phi * dy2;
    const float y1 = -sin_phi * dx2 + cos_phi * dy2;

    float rx_sq = rx * rx;
    float ry_sq = ry * ry;
    const float x1_sq = x1 * x1;
    const float y1_sq = y1 * y1;
    const float radii = x1_sq / rx_sq + y1_sq / ry_sq;
    if (radii > 1.0f) {
        const float scale = std::sqrt(radii);
        rx *= scale;
        ry *= scale;
        rx_sq = rx * rx;
        ry_sq = ry * ry;
    }

    float factor = (rx_sq * ry_sq - rx_sq * y1_sq - ry_sq * x1_sq) / (rx_sq * y1_sq + ry_sq * x1_sq);
    factor = factor < 0.0f ? 0.0f : std::sqrt(factor);
    if (large_arc == sweep) factor = -factor;
    const float cx1 = factor * rx * y1 / ry;
    const float cy1 = -factor * ry * x1 / rx;
    const float cx = cos_phi * cx1 - sin_phi * cy1 + (p0.x + p1.x) / 2.0f;
    const float cy = sin_phi * cx1 + cos_phi * cy1 + (p0.y + p1.y) / 2.0f;

    const auto angle = [](float ux, float uy, float vx, float vy) {
        const float dot = ux * vx + uy * vy;
        const float length = std::hypot(ux, uy) * std::hypot(vx, vy);
        float value = (length == 0.0f) ? 0.0f : std::clamp(dot / length, -1.0f, 1.0f);
        value = std::acos(value);
        return (ux * vy - uy * vx < 0.0f) ? -value : value;
    };

    const float theta1 = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    float delta = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && delta > 0.0f) delta -= 2 * kPi;
    if (sweep && delta < 0.0f) delta += 2 * kPi;

    const float radius = std::max(rx, ry);
    const int steps = std::clamp(static_cast<int>(std::fabs(delta) * radius / std::max(0.05f, tolerance)), 4, 128);
    for (int i = 1; i <= steps; ++i) {
        const float theta = theta1 + delta * static_cast<float>(i) / static_cast<float>(steps);
        const float px = cos_phi * rx * std::cos(theta) - sin_phi * ry * std::sin(theta) + cx;
        const float py = sin_phi * rx * std::cos(theta) + cos_phi * ry * std::sin(theta) + cy;
        add_point(path, Point{px, py});
    }
}

// --- coverage blending -----------------------------------------------------

void blend_pixel(Bitmap* canvas, int x, int y, uint32_t argb, float coverage) {
    if (coverage <= 0.0f) return;
    coverage = std::min(1.0f, coverage);
    uint8_t* pixel = &canvas->rgba[(static_cast<size_t>(y) * static_cast<size_t>(canvas->width) +
                                   static_cast<size_t>(x)) * 4];
    const float src_a = ((argb >> 24) & 0xFF) / 255.0f * coverage;
    if (src_a <= 0.0f) return;
    const float src_r = ((argb >> 16) & 0xFF) / 255.0f;
    const float src_g = ((argb >> 8) & 0xFF) / 255.0f;
    const float src_b = (argb & 0xFF) / 255.0f;

    const float dst_a = pixel[3] / 255.0f;
    const float out_a = src_a + dst_a * (1.0f - src_a);
    if (out_a <= 0.0f) return;
    const auto mix = [&](int index, float src) {
        const float dst = pixel[index] / 255.0f;
        const float out = (src * src_a + dst * dst_a * (1.0f - src_a)) / out_a;
        pixel[index] = static_cast<uint8_t>(std::lround(std::clamp(out, 0.0f, 1.0f) * 255.0f));
    };
    mix(0, src_r);
    mix(1, src_g);
    mix(2, src_b);
    pixel[3] = static_cast<uint8_t>(std::lround(std::clamp(out_a, 0.0f, 1.0f) * 255.0f));
}

// Rasterises device-space polygons into a coverage buffer, then blends them.
void rasterise_polygons(Bitmap* canvas, const std::vector<std::vector<Point>>& polygons, uint32_t argb,
                        bool even_odd) {
    if (canvas->width <= 0 || canvas->height <= 0) return;
    const int width = canvas->width * kSuper;
    const int height = canvas->height * kSuper;
    std::vector<uint8_t> coverage(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

    std::vector<float> crossings;
    for (int row = 0; row < height; ++row) {
        const float sample_y = (static_cast<float>(row) + 0.5f) / kSuper;
        crossings.clear();
        for (const std::vector<Point>& polygon : polygons) {
            if (polygon.size() < 3) continue;
            for (size_t i = 0; i < polygon.size(); ++i) {
                const Point& a = polygon[i];
                const Point& b = polygon[(i + 1) % polygon.size()];
                if (a.y == b.y) continue;
                if ((sample_y >= a.y && sample_y < b.y) || (sample_y >= b.y && sample_y < a.y)) {
                    const float t = (sample_y - a.y) / (b.y - a.y);
                    crossings.push_back(a.x + t * (b.x - a.x));
                }
            }
        }
        if (crossings.empty()) continue;
        std::sort(crossings.begin(), crossings.end());
        if (even_odd) {
            for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
                const int from = static_cast<int>(std::ceil(crossings[i] * kSuper - 0.5f));
                const int to = static_cast<int>(std::ceil(crossings[i + 1] * kSuper - 0.5f));
                for (int x = std::max(0, from); x < std::min(width, to); ++x) {
                    coverage[static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(x)] = 1;
                }
            }
        } else {
            // Non-zero winding. The direction of every edge has to be carried
            // explicitly: encoding it in the sign of x loses it when x is 0 (which
            // is exactly where a canvas-anchored square starts).
            struct Crossing {
                float x;
                int direction;
            };
            std::vector<Crossing> ordered;
            ordered.reserve(crossings.size());
            for (const std::vector<Point>& polygon : polygons) {
                if (polygon.size() < 3) continue;
                for (size_t i = 0; i < polygon.size(); ++i) {
                    const Point& a = polygon[i];
                    const Point& b = polygon[(i + 1) % polygon.size()];
                    if (a.y == b.y) continue;
                    if ((sample_y >= a.y && sample_y < b.y) || (sample_y >= b.y && sample_y < a.y)) {
                        const float t = (sample_y - a.y) / (b.y - a.y);
                        ordered.push_back(Crossing{a.x + t * (b.x - a.x), a.y < b.y ? 1 : -1});
                    }
                }
            }
            std::sort(ordered.begin(), ordered.end(),
                      [](const Crossing& lhs, const Crossing& rhs) { return lhs.x < rhs.x; });

            int winding = 0;
            for (size_t i = 0; i < ordered.size(); ++i) {
                winding += ordered[i].direction;
                if (winding == 0 || i + 1 >= ordered.size()) continue;
                const float from_x = ordered[i].x;
                const float to_x = ordered[i + 1].x;
                const int from = static_cast<int>(std::ceil(from_x * kSuper - 0.5f));
                const int to = static_cast<int>(std::ceil(to_x * kSuper - 0.5f));
                for (int x = std::max(0, from); x < std::min(width, to); ++x) {
                    coverage[static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(x)] = 1;
                }
            }
        }
    }

    // downsample 4x4 -> per-pixel coverage, then blend
    const float inverse = 1.0f / (kSuper * kSuper);
    for (int y = 0; y < canvas->height; ++y) {
        for (int x = 0; x < canvas->width; ++x) {
            int hits = 0;
            for (int sy = 0; sy < kSuper; ++sy) {
                const size_t base = static_cast<size_t>(y * kSuper + sy) * static_cast<size_t>(width);
                for (int sx = 0; sx < kSuper; ++sx) hits += coverage[base + static_cast<size_t>(x * kSuper + sx)];
            }
            if (hits == 0) continue;
            blend_pixel(canvas, x, y, argb, static_cast<float>(hits) * inverse);
        }
    }
}

std::vector<Point> transform_polygon(const std::vector<Point>& points, const Transform& transform) {
    std::vector<Point> out;
    out.reserve(points.size());
    for (const Point& point : points) out.push_back(transform.apply(point));
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------

Transform Transform::translate(float tx, float ty) {
    Transform result;
    result.e = tx;
    result.f = ty;
    return result;
}

Transform Transform::scale(float sx, float sy) {
    Transform result;
    result.a = sx;
    result.d = sy;
    return result;
}

Transform Transform::rotate(float degrees, float pivot_x, float pivot_y) {
    const float radians = to_radians(degrees);
    const float cs = std::cos(radians);
    const float sn = std::sin(radians);
    Transform result;
    result.a = cs;
    result.b = sn;
    result.c = -sn;
    result.d = cs;
    result.e = pivot_x - cs * pivot_x + sn * pivot_y;
    result.f = pivot_y - sn * pivot_x - cs * pivot_y;
    return result;
}

Transform Transform::concat(const Transform& inner) const {
    Transform result;
    result.a = a * inner.a + c * inner.b;
    result.b = b * inner.a + d * inner.b;
    result.c = a * inner.c + c * inner.d;
    result.d = b * inner.c + d * inner.d;
    result.e = a * inner.e + c * inner.f + e;
    result.f = b * inner.e + d * inner.f + f;
    return result;
}

Point Transform::apply(Point point) const {
    return Point{a * point.x + c * point.y + e, b * point.x + d * point.y + f};
}

// ---------------------------------------------------------------------------
// Path parsing
// ---------------------------------------------------------------------------

std::vector<Subpath> parse_path(std::string_view data, float tolerance) {
    std::vector<Subpath> subpaths;
    Scanner scanner{data, 0};

    Point current{0, 0};
    Point start{0, 0};
    // last control point of the previous curve, for the smooth variants
    Point last_cubic_control{0, 0};
    Point last_quad_control{0, 0};
    char previous_command = '\0';
    Subpath path;

    const auto flush = [&]() {
        if (path.points.size() >= 2) subpaths.push_back(path);
        path = Subpath{};
    };

    char command = '\0';
    while (true) {
        scanner.skip_separators();
        if (scanner.position >= data.size()) break;
        if (scanner.peek_command()) {
            command = scanner.next_command();
        } else if (command == '\0') {
            break;  // numbers without a command
        } else if (command == 'M') {
            command = 'L';  // implicit lineto after moveto
        } else if (command == 'm') {
            command = 'l';
        }

        const bool relative = command >= 'a' && command <= 'z';
        const char op = relative ? static_cast<char>(command - 'a' + 'A') : command;
        float x = 0, y = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;

        switch (op) {
            case 'M': {
                if (!scanner.number(&x) || !scanner.number(&y)) return subpaths;
                Point target = relative ? Point{current.x + x, current.y + y} : Point{x, y};
                flush();
                current = target;
                start = target;
                add_point(path, current);
                break;
            }
            case 'L': {
                if (!scanner.number(&x) || !scanner.number(&y)) return subpaths;
                current = relative ? Point{current.x + x, current.y + y} : Point{x, y};
                add_point(path, current);
                break;
            }
            case 'H': {
                if (!scanner.number(&x)) return subpaths;
                current.x = relative ? current.x + x : x;
                add_point(path, current);
                break;
            }
            case 'V': {
                if (!scanner.number(&y)) return subpaths;
                current.y = relative ? current.y + y : y;
                add_point(path, current);
                break;
            }
            case 'C': {
                if (!scanner.number(&x) || !scanner.number(&y) || !scanner.number(&x1) ||
                    !scanner.number(&y1) || !scanner.number(&x2) || !scanner.number(&y2)) {
                    return subpaths;
                }
                const Point c1 = relative ? Point{current.x + x, current.y + y} : Point{x, y};
                const Point c2 = relative ? Point{current.x + x1, current.y + y1} : Point{x1, y1};
                const Point end = relative ? Point{current.x + x2, current.y + y2} : Point{x2, y2};
                flatten_cubic(path, current, c1, c2, end, curve_steps(current, c1, c2, end, tolerance));
                last_cubic_control = c2;
                current = end;
                break;
            }
            case 'S': {
                if (!scanner.number(&x) || !scanner.number(&y) || !scanner.number(&x1) || !scanner.number(&y1)) {
                    return subpaths;
                }
                const Point c1 = (previous_command == 'C' || previous_command == 'S')
                                     ? Point{2 * current.x - last_cubic_control.x, 2 * current.y - last_cubic_control.y}
                                     : current;
                const Point c2 = relative ? Point{current.x + x, current.y + y} : Point{x, y};
                const Point end = relative ? Point{current.x + x1, current.y + y1} : Point{x1, y1};
                flatten_cubic(path, current, c1, c2, end, curve_steps(current, c1, c2, end, tolerance));
                last_cubic_control = c2;
                current = end;
                break;
            }
            case 'Q': {
                if (!scanner.number(&x) || !scanner.number(&y) || !scanner.number(&x1) || !scanner.number(&y1)) {
                    return subpaths;
                }
                const Point control = relative ? Point{current.x + x, current.y + y} : Point{x, y};
                const Point end = relative ? Point{current.x + x1, current.y + y1} : Point{x1, y1};
                flatten_quadratic(path, current, control, end,
                                  curve_steps(current, control, control, end, tolerance));
                last_quad_control = control;
                current = end;
                break;
            }
            case 'T': {
                if (!scanner.number(&x) || !scanner.number(&y)) return subpaths;
                const Point control = (previous_command == 'Q' || previous_command == 'T')
                                          ? Point{2 * current.x - last_quad_control.x,
                                                  2 * current.y - last_quad_control.y}
                                          : current;
                const Point end = relative ? Point{current.x + x, current.y + y} : Point{x, y};
                flatten_quadratic(path, current, control, end,
                                  curve_steps(current, control, control, end, tolerance));
                last_quad_control = control;
                current = end;
                break;
            }
            case 'A': {
                float rx = 0, ry = 0, rotation = 0;
                bool large_arc = false, sweep = false;
                if (!scanner.number(&rx) || !scanner.number(&ry) || !scanner.number(&rotation) ||
                    !scanner.flag(&large_arc) || !scanner.flag(&sweep) || !scanner.number(&x) ||
                    !scanner.number(&y)) {
                    return subpaths;
                }
                const Point end = relative ? Point{current.x + x, current.y + y} : Point{x, y};
                flatten_arc(path, current, rx, ry, rotation, large_arc, sweep, end, tolerance);
                current = end;
                break;
            }
            case 'Z': {
                if (!path.points.empty()) {
                    path.closed = true;
                    add_point(path, path.points.front());
                }
                flush();
                current = start;
                break;
            }
            default:
                // Unknown command: stop instead of looping forever.
                return subpaths;
        }
        previous_command = op;
    }
    flush();
    return subpaths;
}

// ---------------------------------------------------------------------------
// Fill / stroke
// ---------------------------------------------------------------------------

void fill(Bitmap* canvas, const std::vector<Subpath>& subpaths, const Transform& transform, uint32_t argb,
          bool even_odd) {
    if (!canvas || !canvas->valid() || subpaths.empty()) return;
    std::vector<std::vector<Point>> polygons;
    polygons.reserve(subpaths.size());
    for (const Subpath& subpath : subpaths) {
        if (subpath.points.size() < 3) continue;
        polygons.push_back(transform_polygon(subpath.points, transform));
    }
    rasterise_polygons(canvas, polygons, argb, even_odd);
}

void stroke(Bitmap* canvas, const std::vector<Subpath>& subpaths, const Transform& transform, uint32_t argb,
            float width) {
    if (!canvas || !canvas->valid() || subpaths.empty() || width <= 0.0f) return;

    // Approximate the stroke with one quad per segment plus a disc at every joint.
    const float scale = std::sqrt(std::fabs(transform.a * transform.d - transform.b * transform.c));
    const float half = std::max(0.5f, width * scale / 2.0f);

    std::vector<std::vector<Point>> polygons;
    const auto add_disc = [&](Point centre) {
        const int steps = std::clamp(static_cast<int>(half * 2.0f) + 8, 10, 48);
        std::vector<Point> disc;
        disc.reserve(static_cast<size_t>(steps));
        for (int i = 0; i < steps; ++i) {
            const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(steps);
            disc.push_back(Point{centre.x + half * std::cos(angle), centre.y + half * std::sin(angle)});
        }
        polygons.push_back(std::move(disc));
    };

    for (const Subpath& subpath : subpaths) {
        if (subpath.points.size() < 2) continue;
        std::vector<Point> points = transform_polygon(subpath.points, transform);
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            const Point& a = points[i];
            const Point& b = points[i + 1];
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float length = std::hypot(dx, dy);
            if (length < 1e-4f) continue;
            const float nx = -dy / length * half;
            const float ny = dx / length * half;
            polygons.push_back({Point{a.x + nx, a.y + ny}, Point{b.x + nx, b.y + ny}, Point{b.x - nx, b.y - ny},
                                Point{a.x - nx, a.y - ny}});
            add_disc(b);
        }
        add_disc(points.front());
    }
    rasterise_polygons(canvas, polygons, argb, false);
}

}  // namespace vector
}  // namespace apkinfo
