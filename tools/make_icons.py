#!/usr/bin/env python3
"""Generates the ApkInfo icon family (original artwork, no third-party assets).

Single source of truth: every icon is a list of primitives in a 1024x1024
coordinate space. The same list produces

  * an editable SVG (assets/icons/*.svg), and
  * raster PNG + multi-size ICO files with Pillow (assets/icons/*).

Design language
  application  rounded-square badge, blue gradient, white magnifier - "inspect".
  file icons   a document with a folded corner, filled with the type colour and
               a white glyph, so Explorer rows stay legible at 16 px.
  .apk         one blue document with a "package" mark. Deliberately *shared* by
               Android and Alpine packages: the shell chooses the icon by file
               extension, so it cannot know which world a file belongs to.
  .apks/.apkm/.xapk
               purple document with a stacked edge (a second sheet peeking out)
               and a lid split into two halves - "several parts".
  in-app       the window/taskbar icon *does* reflect the detected format
  window icon  (green package for Android, teal mountain for Alpine, purple
               bundle for split bundles), because the reader knows the truth.
  All shapes are generic geometry (magnifier, box, mountain). No Android robot,
  Alpine logo, Play Store mark or other trademarked artwork is used.

Usage:
    python tools/make_icons.py             # write all assets
    python tools/make_icons.py --preview   # + ASCII preview at 32 and 16 px
"""
from __future__ import annotations

import os
import sys

from PIL import Image, ImageDraw

CANVAS = 1024
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(os.path.dirname(HERE), "assets", "icons")

BLUE_TOP = "#3B7BE8"
BLUE_BOTTOM = "#1D4FB8"
APK_BLUE = "#2B6CE0"
BUNDLE_PURPLE = "#7C4DFF"
ANDROID_GREEN = "#2E9E5B"
ALPINE_TEAL = "#12A0A6"
WHITE = "#FFFFFF"

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def lighten(hex_color: str, amount: float) -> str:
    raw = hex_color.lstrip("#")
    rgb = [int(raw[i:i + 2], 16) for i in (0, 2, 4)]
    rgb = [min(255, int(c + (255 - c) * amount)) for c in rgb]
    return "#%02X%02X%02X" % tuple(rgb)


# ----------------------------------------------------------------- primitives
def rrect(box, radius, fill, width=0, stroke=None):
    return {"type": "rrect", "box": box, "radius": radius, "fill": fill,
            "width": width, "stroke": stroke}


def poly(points, fill):
    return {"type": "poly", "points": points, "fill": fill}


def circle(center, radius, fill=None, width=0, stroke=None):
    return {"type": "circle", "center": center, "radius": radius, "fill": fill,
            "width": width, "stroke": stroke}


def line(p0, p1, width, color):
    return {"type": "line", "p0": p0, "p1": p1, "width": width, "color": color}


def vgradient(box, radius, top, bottom):
    return {"type": "vgradient", "box": box, "radius": radius, "top": top, "bottom": bottom}


def group(children):
    return {"type": "group", "children": children}


# ----------------------------------------------------------------- icons
def app_icon():
    """Rounded blue badge with a white magnifier."""
    return [
        vgradient((64, 64, 960, 960), 208, BLUE_TOP, BLUE_BOTTOM),
        circle((452, 448), 226, width=100, stroke=WHITE),      # lens
        line((624, 620), (828, 824), 118, WHITE),              # handle
    ]


def document(accent, glyph, stacked=False):
    """Coloured sheet + folded corner + glyph.

    `stacked` adds a second sheet peeking out at the top left, which is how the
    bundle icon differs from the single-package icon at a glance (silhouette, not
    interior detail, is what survives at 16 px).
    """
    right, bottom = 828, 960
    cut = 210
    shapes = []
    if stacked:
        offset = 74
        back = [(196 - offset, 64 - offset), (right - cut - offset, 64 - offset),
                (right - offset, 64 + cut - offset), (right - offset, bottom - offset),
                (196 - offset, bottom - offset)]
        shapes.append(poly(back, lighten(accent, 0.52)))
    shapes.append(poly([(196, 64), (right - cut, 64), (right, 64 + cut), (right, bottom), (196, bottom)], accent))
    shapes.append(poly([(right - cut, 64), (right, 64 + cut), (right - cut, 64 + cut)], lighten(accent, 0.62)))
    shapes.append(glyph(accent))
    return shapes


def glyph_package(accent):
    """Single package: lid + body, both solid (outlines turn to mud at 16 px)."""
    return group([
        rrect((330, 418, 694, 536), 28, WHITE),
        rrect((368, 600, 656, 822), 30, WHITE),
    ])


def glyph_split(accent):
    """Split bundle: lid in two halves + a body - "several parts of one package"."""
    return group([
        rrect((330, 418, 494, 536), 26, WHITE),
        rrect((530, 418, 694, 536), 26, WHITE),
        rrect((368, 600, 656, 822), 30, WHITE),
    ])


def glyph_mountain(accent):
    """Alpine: a single bold peak (generic geometry, no detail to lose at 16 px)."""
    return group([
        poly([(288, 800), (512, 392), (736, 800)], WHITE),
    ])


def icon_table():
    return [
        ("app", app_icon()),
        ("apk", document(APK_BLUE, glyph_package)),
        ("apks", document(BUNDLE_PURPLE, glyph_split, stacked=True)),
        ("android", document(ANDROID_GREEN, glyph_package)),
        ("alpine", document(ALPINE_TEAL, glyph_mountain)),
    ]


# ----------------------------------------------------------------- raster
def flatten(shapes):
    for shape in shapes:
        if shape["type"] == "group":
            yield from flatten(shape["children"])
        else:
            yield shape


def _rgba(color):
    if color is None:
        return None
    raw = color.lstrip("#")
    if len(raw) == 8:
        return tuple(int(raw[i:i + 2], 16) for i in (0, 2, 4, 6))
    return tuple(int(raw[i:i + 2], 16) for i in (0, 2, 4)) + (255,)


def draw_gradient(img, shape):
    box, radius = shape["box"], shape["radius"]
    top = tuple(int(shape["top"].lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))
    bottom = tuple(int(shape["bottom"].lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))
    width, height = box[2] - box[0], box[3] - box[1]
    gradient = Image.new("RGBA", (1, height))
    for y in range(height):
        t = y / max(1, height - 1)
        gradient.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    gradient = gradient.resize((width, height))
    mask = Image.new("L", (width, height), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, width - 1, height - 1), radius, fill=255)
    img.paste(gradient, (box[0], box[1]), mask)


def render(shapes, size=CANVAS):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    for shape in flatten(shapes):
        kind = shape["type"]
        if kind == "vgradient":
            draw_gradient(img, shape)
        elif kind == "rrect":
            draw.rounded_rectangle(shape["box"], shape["radius"],
                                   fill=_rgba(shape["fill"]),
                                   outline=_rgba(shape["stroke"]) if shape["stroke"] else None,
                                   width=shape["width"])
        elif kind == "poly":
            draw.polygon(shape["points"], fill=_rgba(shape["fill"]))
        elif kind == "circle":
            cx, cy = shape["center"]
            r = shape["radius"]
            draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=_rgba(shape["fill"]),
                         outline=_rgba(shape["stroke"]) if shape["stroke"] else None, width=shape["width"])
        elif kind == "line":
            (x0, y0), (x1, y1) = shape["p0"], shape["p1"]
            w = shape["width"]
            draw.line((x0, y0, x1, y1), fill=_rgba(shape["color"]), width=w)
            for (cx, cy) in (shape["p0"], shape["p1"]):     # round caps
                draw.ellipse((cx - w / 2, cy - w / 2, cx + w / 2, cy + w / 2), fill=_rgba(shape["color"]))
    return img


# ----------------------------------------------------------------- svg
def to_svg(shapes, size=CANVAS):
    parts = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" '
        f'viewBox="0 0 {size} {size}" fill="none">',
        "  <!-- generated by tools/make_icons.py; edit the script, not this file -->",
    ]
    gradients = []
    body = []
    gid = 0
    for shape in flatten(shapes):
        kind = shape["type"]
        if kind == "vgradient":
            gid += 1
            name = f"g{gid}"
            x0, y0, x1, y1 = shape["box"]
            gradients.append(
                f'    <linearGradient id="{name}" x1="0" y1="0" x2="0" y2="1">'
                f'<stop offset="0" stop-color="{shape["top"]}"/>'
                f'<stop offset="1" stop-color="{shape["bottom"]}"/></linearGradient>')
            body.append(f'  <rect x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0}" '
                        f'rx="{shape["radius"]}" fill="url(#{name})"/>')
        elif kind == "rrect":
            x0, y0, x1, y1 = shape["box"]
            stroke = f' stroke="{shape["stroke"]}" stroke-width="{shape["width"]}"' if shape["stroke"] else ""
            body.append(f'  <rect x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0}" '
                        f'rx="{shape["radius"]}" fill="{shape["fill"] or "none"}"{stroke}/>')
        elif kind == "poly":
            points = " ".join(f"{x},{y}" for x, y in shape["points"])
            body.append(f'  <polygon points="{points}" fill="{shape["fill"]}"/>')
        elif kind == "circle":
            cx, cy = shape["center"]
            stroke = f' stroke="{shape["stroke"]}" stroke-width="{shape["width"]}"' if shape["stroke"] else ""
            body.append(f'  <circle cx="{cx}" cy="{cy}" r="{shape["radius"]}" '
                        f'fill="{shape["fill"] or "none"}"{stroke}/>')
        elif kind == "line":
            (x0, y0), (x1, y1) = shape["p0"], shape["p1"]
            body.append(f'  <line x1="{x0}" y1="{y0}" x2="{x1}" y2="{y1}" stroke="{shape["color"]}" '
                        f'stroke-width="{shape["width"]}" stroke-linecap="round"/>')
    if gradients:
        parts.append("  <defs>")
        parts.extend(gradients)
        parts.append("  </defs>")
    parts.extend(body)
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


# ----------------------------------------------------------------- preview
BLOCKS = " .:-=+*#%@"


def ascii_preview(img: Image.Image, width: int) -> str:
    small = img.resize((width, width), Image.LANCZOS)
    rows = []
    for y in range(width):
        row = ""
        for x in range(width):
            r, g, b, a = small.getpixel((x, y))
            if a < 32:
                row += " "
            else:
                lum = (0.299 * r + 0.587 * g + 0.114 * b) / 255 * (a / 255)
                row += BLOCKS[min(len(BLOCKS) - 1, int(lum * len(BLOCKS)))]
        rows.append(row)
    return "\n".join(rows)


# ----------------------------------------------------------------- main
def main() -> int:
    preview = "--preview" in sys.argv
    os.makedirs(OUT_DIR, exist_ok=True)
    written = []
    for name, shapes in icon_table():
        master = render(shapes)
        svg_path = os.path.join(OUT_DIR, f"{name}.svg")
        with open(svg_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(to_svg(shapes))
        png_path = os.path.join(OUT_DIR, f"{name}.png")
        master.save(png_path, format="PNG")
        ico_path = os.path.join(OUT_DIR, f"{name}.ico")
        master.save(ico_path, format="ICO", sizes=[(s, s) for s in SIZES])
        written.extend([svg_path, png_path, ico_path])
        if preview:
            for size in (32, 16):
                print(f"=== {name} @ {size}")
                print(ascii_preview(master, size))
    for path in written:
        print(f"{os.path.getsize(path):8d}  {os.path.relpath(path, os.path.dirname(OUT_DIR))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
