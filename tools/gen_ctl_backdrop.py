#!/usr/bin/env python3
"""生成打印态底部复合控件的玻璃底衬（ARGB8888 lv_image_dsc_t），供 page_user_home.cpp 使用。

图源为 Figma 设计稿 fileKey DNrr8MiRglxIiof1MBk4TY 的两个 Union 布尔形状：
    ctl_bd_run     4796:6172 「底框2」168×80  运行组（暂停/恢复钮 + 终止钮）
    ctl_bd_collect 4231:6034 「底框」 488×80  取件组（吹粉钮 + 滑动取件胶囊）

这两条形状是「圆 blob + 细腰 + 圆/胶囊」并集后又给结果路径加了圆角平滑，交接处是
一段内凹圆滑过渡。LVGL 只能画凸的圆角矩形，拿几块矩形拼出来的腰是尖角、而且各块
自带的描边会把藏在并集内部的圆弧也描出来，所以这里改走出图的路子。

导出的 SVG 里 Union 是 fill=black / fill-opacity=0.5，上面另叠一条 1px 白 10% 的
内描边。两者都直接烤进 ARGB8888——运行时一张图一次混合，既不会在交叠处叠深，也保住
了那圈描边。用 A8 只能存单色蒙版，描边就没了。

输入（需先用 download_assets 按 svg 格式导出到该目录）：
    tools/_pf_tmp/{ctl_bd_run,ctl_bd_collect}.svg
该目录不进版本库。
输出：
    app/ui/lib/assets/src/ctl_backdrop_img.c
    app/ui/lib/assets/inc/ctl_backdrop_img.h

用法：
    python3 tools/gen_ctl_backdrop.py
"""
import io
import os
import re

import cairosvg
from PIL import Image

# 稿的坐标系（两条形状同高、左端 blob 同径）
SRC_H = 80
RUN_SRC_W = 168
COLLECT_SRC_W = 488
# 取件组胶囊的直段：稿上胶囊是 x∈[88,488]、r=40 的圆角矩形，故 x∈[128,448] 每一列
# 都完全相同。拉伸只发生在这一段里，圆头和细腰保持原样。
BAND_X0, BAND_X1 = 128, 448

SS = 4          # 超采样倍数，先放大渲染再 LANCZOS 降采样，边缘更干净

# profile -> (btn, pill)，必须与 ui_theme.cpp 的 user_print_ctl_btn /
# user_print_pill_w 保持一致。改了那两个常量就要重跑本脚本，否则底衬和按钮会错位。
# 三档都是稿（1280×800，btn 64 / pill 400）按屏宽等比折算，故 1280 档得到的
# 168×80 与 488×80 正是稿的原尺寸，底衬图零重采样。
PROFILES = [
    ("1920", 96, 600),
    ("1280", 64, 400),
    ("1024", 51, 320),
]


def blob_of(btn: int) -> int:
    """底衬上那颗圆的直径：稿里圆钮 64 坐在直径 80 的 blob 上。

    +2 是四舍五入——直接整除的话 1024 档（btn=51）会截成 63、比稿少 1px。
    必须与 page_user_home.cpp 的 ctl_blob() 一致。
    """
    return (btn * 5 + 2) // 4


def gap_of(btn: int) -> int:
    """两块之间的缝：稿 btn=64 时为 8。必须与 page_user_home.cpp 的 ctl_gap() 一致。"""
    return btn // 8


def load_svg(path: str) -> bytes:
    """读 SVG 并去掉 Figma 附带的画板底色与背景模糊层。

    导出里第一条 <rect> 是画板底色（不透明灰），留着会把整张图糊死；<foreignObject>
    是 backdrop-filter 背景模糊，屏上做不了也不需要。
    """
    s = open(path).read()
    s = re.sub(r'<rect width="\d+" height="\d+" fill="#[0-9A-Fa-f]{6}"/>', "", s, count=1)
    s = re.sub(r"<foreignObject.*?</foreignObject>", "", s, flags=re.S)
    return s.encode()


def render(svg: bytes, w: int, h: int) -> Image.Image:
    png = cairosvg.svg2png(bytestring=svg, output_width=w, output_height=h)
    return Image.open(io.BytesIO(png)).convert("RGBA")


def render_run(svg: bytes, w: int, h: int) -> Image.Image:
    """运行组：两端都是圆，没有可拉伸的直段，只能整体缩放。

    目标宽 blob*2+gap 与稿的 168:80 相差不到 1%（如 1024 档 119 : 119.7），非等比
    渲染带来的水平压缩看不出来，换来的是底衬与按钮坐标严格对齐。
    """
    return render(svg, w * SS, h * SS).resize((w, h), Image.LANCZOS)


def render_collect(svg: bytes, w: int, h: int) -> Image.Image:
    """取件组：先按稿的比例等比渲染，再在胶囊直段里插列拉到目标宽。

    胶囊长度不随 blob 等比变化（各档 pill/blob 在 5.56~5.61 之间浮动，而稿是 5.0），
    整张缩放会把左端的圆压扁，所以只拉直段。直段里每列相同，复制单列是精确的。
    """
    ss_h = h * SS
    src_w = round(COLLECT_SRC_W * ss_h / SRC_H)
    dst_w = w * SS
    delta = dst_w - src_w            # 正=直段要加长，负=要缩短（各档都只差几个像素）
    im = render(svg, src_w, ss_h)

    cut = round((BAND_X0 + BAND_X1) / 2 * ss_h / SRC_H)     # 直段正中下刀
    half_band = round((BAND_X1 - BAND_X0) / 2 * ss_h / SRC_H)
    if abs(delta) >= half_band:
        raise SystemExit(
            "collect %dx%d: 需在直段增删 %d 列，超出直段半宽 %d。"
            "请检查 PROFILES 里的 pill 值。" % (w, h, delta, half_band))

    out = Image.new("RGBA", (dst_w, ss_h))
    out.paste(im.crop((0, 0, cut, ss_h)), (0, 0))
    if delta > 0:
        col = im.crop((cut, 0, cut + 1, ss_h))
        out.paste(col.resize((delta, ss_h), Image.NEAREST), (cut, 0))
    # delta<0 时直接跳过 -delta 列；直段里每列相同，增删都是精确的
    out.paste(im.crop((cut - min(delta, 0), 0, src_w, ss_h)), (cut + max(delta, 0), 0))
    return out.resize((w, h), Image.LANCZOS)


def emit(name: str, im: Image.Image) -> str:
    w, h = im.size
    px = im.load()
    lines = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = px[x, y]
            row.extend((str(b), str(g), str(r), str(a)))   # ARGB8888 小端：B,G,R,A
        lines.append("    " + ",".join(row) + ",")
    body = "\n".join(lines)
    return (
        f"static const uint8_t {name}_map[] = {{\n{body}\n}};\n\n"
        f"const lv_image_dsc_t {name} = {{\n"
        f"    .header = {{\n"
        f"        .magic = LV_IMAGE_HEADER_MAGIC,\n"
        f"        .cf = LV_COLOR_FORMAT_ARGB8888,\n"
        f"        .flags = 0,\n"
        f"        .w = {w},\n"
        f"        .h = {h},\n"
        f"        .stride = {w * 4},\n"
        f"        .reserved_2 = 0,\n"
        f"    }},\n"
        f"    .data_size = {w * h * 4},\n"
        f"    .data = {name}_map,\n"
        f"}};\n\n"
    )


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tmp_dir = os.path.join(root, "tools", "_pf_tmp")
    out_c = os.path.join(root, "app", "ui", "lib", "assets", "src", "ctl_backdrop_img.c")
    out_h = os.path.join(root, "app", "ui", "lib", "assets", "inc", "ctl_backdrop_img.h")

    svg_run = load_svg(os.path.join(tmp_dir, "ctl_bd_run.svg"))
    svg_collect = load_svg(os.path.join(tmp_dir, "ctl_bd_collect.svg"))

    banner = "/* AUTO-GENERATED by tools/gen_ctl_backdrop.py — do not edit by hand. */\n"
    c = banner + '#include "lvgl_wrap.hpp"\n\n'
    names = []
    for tag, btn, pill in PROFILES:
        blob = blob_of(btn)
        gap = gap_of(btn)
        for kind, w, fn, svg in (
            ("run",     blob * 2 + gap,   render_run,     svg_run),
            ("collect", blob + gap + pill, render_collect, svg_collect),
        ):
            name = f"img_ctl_bd_{kind}_{tag}"
            c += emit(name, fn(svg, w, blob))
            names.append(name)
            print(f"  {name}: {w}x{blob}")

    h = banner + "#ifndef CTL_BACKDROP_IMG_H\n#define CTL_BACKDROP_IMG_H\n\n"
    h += '#include "lvgl_wrap.hpp"\n\n#ifdef __cplusplus\nextern "C" {\n#endif\n\n'
    for n in names:
        h += f"extern const lv_image_dsc_t {n};\n"
    h += "\n#ifdef __cplusplus\n}\n#endif\n\n#endif\n"

    with open(out_c, "w") as f:
        f.write(c)
    with open(out_h, "w") as f:
        f.write(h)
    print(f"wrote {out_c}")
    print(f"wrote {out_h}")


if __name__ == "__main__":
    main()
