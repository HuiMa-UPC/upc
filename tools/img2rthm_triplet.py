#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate 3 .rthm thumbnails from one image (1280 main profile preset).

Preset sizes:
  - thumb_small: 72x72
  - thumb_large: 320x180
  - thumb_print: 100x100

Usage:
  python tools/img2rthm_triplet.py -i /path/model.png -o /path/thumbs
  python tools/img2rthm_triplet.py -i /path/model.jpg -o /userdata/Gcode/thumbs --name demo_a

Output files:
  <out_dir>/<name>_small.rthm
  <out_dir>/<name>_large.rthm
  <out_dir>/<name>_print.rthm

Notes:
  - Requires Pillow: pip install pillow
  - File format follows common/fs_manifest/raw_thumb_format.hpp
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path
from typing import Dict, Tuple

try:
    from PIL import Image
except Exception as exc:  # pragma: no cover
    raise SystemExit(
        "Pillow is required. Install with: pip install pillow\n"
        f"Import error: {exc}"
    )


RTHM_MAGIC = b"RTHM"
RTHM_VERSION = 1
RTHM_COLOR_RGB565 = 1

THUMB_SPECS: Dict[str, Tuple[int, int]] = {
    "small": (72, 72),
    "large": (320, 180),
    "print": (100, 100),
}


def rgb888_to_rgb565_le(r: int, g: int, b: int) -> bytes:
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)
    return struct.pack("<H", value)


def _is_cyan_support(r: int, g: int, b: int, a: int) -> bool:
    if a <= 16:
        return False
    mx = max(r, g, b)
    mn = min(r, g, b)
    if mx < 40 or (mx - mn) * 255 // mx < 40:
        return False
    if r >= g or r >= b or g < 50:
        return False
    if abs(g - b) > 40 or b > g + 30:
        return False
    return True


def erase_support_tint(img: Image.Image) -> Image.Image:
    """Paint old Snowpath teal/cyan supports with the canvas background."""
    rgba = img.convert("RGBA")
    w, h = rgba.size
    if w == 0 or h == 0:
        return img
    pix = rgba.load()
    corners = [pix[0, 0], pix[w - 1, 0], pix[0, h - 1], pix[w - 1, h - 1]]
    bg = tuple(sum(c[i] for c in corners) // 4 for i in range(4))
    if any(abs(c[i] - bg[i]) > 24 for c in corners for i in range(4)):
        return img
    changed = False
    for y in range(h):
        for x in range(w):
            r, g, b, a = pix[x, y]
            if _is_cyan_support(r, g, b, a):
                pix[x, y] = bg
                changed = True
    return rgba if changed else img


def trim_empty_canvas(img: Image.Image) -> Image.Image:
    """Crop uniform corner-color / transparent padding (Snowpath 3MF thumbs)."""
    rgba = img.convert("RGBA")
    w, h = rgba.size
    if w == 0 or h == 0:
        return img
    pix = rgba.load()
    corners = [pix[0, 0], pix[w - 1, 0], pix[0, h - 1], pix[w - 1, h - 1]]
    bg = tuple(sum(c[i] for c in corners) // 4 for i in range(4))
    if any(
        abs(c[i] - bg[i]) > 24
        for c in corners
        for i in range(4)
    ):
        return img

    color_tol = 18
    alpha_tol = 16
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            r, g, b, a = pix[x, y]
            if bg[3] <= alpha_tol:
                is_bg = a <= alpha_tol
            elif a <= alpha_tol:
                is_bg = True
            else:
                is_bg = (
                    abs(r - bg[0]) <= color_tol
                    and abs(g - bg[1]) <= color_tol
                    and abs(b - bg[2]) <= color_tol
                )
            if is_bg:
                continue
            minx = min(minx, x)
            miny = min(miny, y)
            maxx = max(maxx, x)
            maxy = max(maxy, y)
    if maxx < minx or maxy < miny:
        return img
    raw_w = maxx - minx + 1
    raw_h = maxy - miny + 1
    if raw_w < 8 or raw_h < 8:
        return img
    pad = max(4, max(raw_w, raw_h) // 10)
    box = (
        max(0, minx - pad),
        max(0, miny - pad),
        min(w, maxx + 1 + pad),
        min(h, maxy + 1 + pad),
    )
    return img.crop(box)


def resize_contain(img: Image.Image, width: int, height: int) -> Image.Image:
    src_w, src_h = img.size
    if src_w <= 0 or src_h <= 0:
        raise ValueError("invalid source image size")

    src_ratio = src_w / src_h
    dst_ratio = width / height

    if src_ratio > dst_ratio:
        new_w = width
        new_h = max(1, int(round(width / src_ratio)))
    else:
        new_h = height
        new_w = max(1, int(round(height * src_ratio)))

    # Pillow < 9.1 没有 Image.Resampling；两种写法的 LANCZOS 值相同。
    resampling = getattr(Image, "Resampling", Image)
    resized = img.resize((new_w, new_h), resampling.LANCZOS)
    corner = img.convert("RGBA").getpixel((0, 0))[:3]
    canvas = Image.new("RGB", (width, height), corner)
    ox = max(0, (width - new_w) // 2)
    oy = max(0, (height - new_h) // 2)
    canvas.paste(resized.convert("RGB"), (ox, oy))
    return canvas


def encode_rgb565_data(img: Image.Image) -> bytes:
    rgb = img.convert("RGB")
    pixels = rgb.load()
    width, height = rgb.size
    out = bytearray(width * height * 2)
    idx = 0
    for y in range(height):
        for x in range(width):
            r, g, b = pixels[x, y]
            out[idx : idx + 2] = rgb888_to_rgb565_le(r, g, b)
            idx += 2
    return bytes(out)


def write_rthm(output_path: Path, width: int, height: int, data: bytes) -> None:
    stride_bytes = width * 2
    header = struct.pack(
        "<4sHHIIIII",
        RTHM_MAGIC,
        RTHM_VERSION,
        0,  # reserved
        width,
        height,
        stride_bytes,
        RTHM_COLOR_RGB565,
        len(data),
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("wb") as fp:
        fp.write(header)
        fp.write(data)


def sanitize_name(name: str) -> str:
    safe = "".join(ch if ch.isalnum() or ch in "-_." else "_" for ch in name)
    return safe or "thumb"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate thumb_small/thumb_large/thumb_print .rthm from one image"
    )
    parser.add_argument("-i", "--input", required=True, help="input image path (png/jpg)")
    parser.add_argument(
        "-o", "--out-dir", required=True, help="output directory for generated .rthm files"
    )
    parser.add_argument(
        "--name",
        default=None,
        help="basename for output files (default: input filename stem)",
    )
    return parser


def main() -> None:
    args = build_parser().parse_args()
    in_path = Path(args.input).expanduser().resolve()
    out_dir = Path(args.out_dir).expanduser().resolve()

    if not in_path.exists():
        raise SystemExit(f"Input file not found: {in_path}")

    base_name = sanitize_name(args.name if args.name else in_path.stem)

    outputs = {}
    with Image.open(in_path) as img:
        filled = trim_empty_canvas(erase_support_tint(img))
        for key, (w, h) in THUMB_SPECS.items():
            prepared = resize_contain(filled, w, h)
            data = encode_rgb565_data(prepared)
            out_path = out_dir / f"{base_name}_{key}.rthm"
            write_rthm(out_path, w, h, data)
            outputs[key] = out_path
            print(f"OK {key}: {out_path} ({w}x{h}, {len(data)} bytes)")

    small = outputs["small"]
    large = outputs["large"]
    prt = outputs["print"]
    print("\nconfig.json snippet:")
    print(f'"thumb_small":"{small}"')
    print(f'"thumb_large":"{large}"')
    print(f'"thumb_print":"{prt}"')


if __name__ == "__main__":
    main()
