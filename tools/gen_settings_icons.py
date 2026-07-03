#!/usr/bin/env python3
"""生成设置页子导航图标（A8 lv_image_dsc_t），格式与 nav_icons.c 一致。

图标路径取自设计稿（每张 300x72 行）的左侧图标区（x∈[36,60], y∈[24,46]）。
统一放进 viewBox="24 12 48 48"（以图标中心 (48,36) 为中心的 48x48 区域），
栅格化为 64x64 透明底白色描边，取 alpha 通道作为 A8 数据（运行时由
lv_obj_set_style_image_recolor 着色：未选灰 / 选中蓝）。

用法：
    python3 tools/gen_settings_icons.py
输出：
    app/ui/lib/assets/src/settings_icons.c
    app/ui/lib/assets/inc/settings_icons.h
"""
import io
import os

import cairosvg
from PIL import Image

SIZE = 64
VIEWBOX = "24 12 48 48"

# name -> 图标内部元素（仅左侧图标，不含标签/箭头/背景）
ICONS = {
    "network": '''
<path d="M58.6668 33.3333H37.3335L42.6668 28" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M37.3332 38.6667H58.6665L53.3332 44" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
''',
    "display": '''
<path d="M45.3332 41.333H50.6665" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
<rect x="36" y="26.667" width="24" height="18.6667" rx="2.66667" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
''',
    "print": '''
<rect x="45.3335" y="28" width="5.33333" height="8" rx="2.66667" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M48 27.3337L48 24.667" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M53.5887 33.667C55.0455 33.667 56.3642 34.5296 56.9481 35.8643L59.6903 42.1309C60.5571 44.1127 59.1054 46.3337 56.9422 46.334H39.0575C36.8943 46.3337 35.4426 44.1127 36.3094 42.1309L39.0516 35.8643C39.6355 34.5296 40.9542 33.667 42.411 33.667H42.6776C42.7217 34.3807 42.9053 35.0565 43.203 35.667H42.411C41.7488 35.667 41.1491 36.0594 40.8836 36.666L38.1415 42.9326C37.8527 43.5931 38.3366 44.3337 39.0575 44.334H56.9422C57.6631 44.3337 58.147 43.5931 57.8583 42.9326L55.1161 36.666C54.8507 36.0594 54.2509 35.667 53.5887 35.667H52.7967C53.0943 35.0565 53.277 34.3807 53.3211 33.667H53.5887Z" fill="white"/>
''',
    "device": '''
<rect x="42.6665" y="25.333" width="10.6667" height="10.6667" rx="5.33333" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M57.3332 46.6667V45.3333C57.3332 42.3878 54.9454 40 51.9998 40H43.9998C41.0543 40 38.6665 42.3878 38.6665 45.3333V46.6667" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
''',
    "privacy": '''
<rect x="39.3335" y="32.667" width="17.3333" height="13.3333" rx="2.66667" stroke="white" stroke-width="2" stroke-linejoin="round"/>
<path d="M42.6665 32.667V29.3337C42.6665 27.4927 44.1589 26.0003 45.9998 26.0003H49.9998C51.8408 26.0003 53.3332 27.4927 53.3332 29.3337V30.0003" stroke="white" stroke-width="2" stroke-linejoin="bevel"/>
''',
    "about": '''
<path d="M48 35.333L48 41.333" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M48 30.6663L48 31.333" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
<rect x="37.3335" y="25.333" width="21.3333" height="21.3333" rx="10.6667" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
''',
    "debug": '''
<path d="M49.3332 30.6663L46.6665 41.333" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M43.3333 30.6663L38 35.9997L43.3333 41.333" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
<path d="M52.6667 30.6663L58 35.9997L52.6667 41.333" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
''',
}

# 输出顺序（决定 .c/.h 中的排列）
ORDER = ["network", "display", "print", "device", "privacy", "about", "debug"]


def rasterize_alpha(inner: str):
    svg = (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE}" height="{SIZE}" '
        f'viewBox="{VIEWBOX}" fill="none">{inner}</svg>'
    )
    png = cairosvg.svg2png(bytestring=svg.encode("utf-8"),
                           output_width=SIZE, output_height=SIZE)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    return list(img.getchannel("A").getdata())


def emit_map(name: str, alpha) -> str:
    lines = []
    for row in range(SIZE):
        chunk = alpha[row * SIZE:(row + 1) * SIZE]
        lines.append("    " + ",".join(str(v) for v in chunk) + ",")
    body = "\n".join(lines)
    return (f"static const uint8_t settings_icon_{name}_map[] = {{\n"
            f"{body}\n}};\n\n")


def emit_dsc(name: str) -> str:
    return (f"const lv_image_dsc_t settings_icon_{name} = {{\n"
            f"    .header = {{\n"
            f"        .magic = LV_IMAGE_HEADER_MAGIC,\n"
            f"        .cf = LV_COLOR_FORMAT_A8,\n"
            f"        .flags = 0,\n"
            f"        .w = {SIZE},\n"
            f"        .h = {SIZE},\n"
            f"        .stride = {SIZE},\n"
            f"        .reserved_2 = 0,\n"
            f"    }},\n"
            f"    .data_size = {SIZE * SIZE},\n"
            f"    .data = settings_icon_{name}_map,\n"
            f"}};\n\n")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = os.path.join(root, "app", "ui", "lib", "assets", "src", "settings_icons.c")
    inc = os.path.join(root, "app", "ui", "lib", "assets", "inc", "settings_icons.h")

    c = ['/* AUTO-GENERATED by tools/gen_settings_icons.py — do not edit by hand. */',
         '#include "lvgl_wrap.hpp"', '', '']
    c = "\n".join(c)
    for name in ORDER:
        alpha = rasterize_alpha(ICONS[name])
        c += emit_map(name, alpha)
        c += emit_dsc(name)

    h = ['/* AUTO-GENERATED by tools/gen_settings_icons.py — do not edit by hand. */',
         '#ifndef SETTINGS_ICONS_H', '#define SETTINGS_ICONS_H', '',
         '#include "lvgl_wrap.hpp"', '', '#ifdef __cplusplus',
         'extern "C" {', '#endif', '']
    for name in ORDER:
        h.append(f"extern const lv_image_dsc_t settings_icon_{name};")
    h += ['', '#ifdef __cplusplus', '}', '#endif', '', '#endif', '']
    h = "\n".join(h)

    with open(src, "w") as f:
        f.write(c)
    with open(inc, "w") as f:
        f.write(h)
    print(f"wrote {src}")
    print(f"wrote {inc}")


if __name__ == "__main__":
    main()
