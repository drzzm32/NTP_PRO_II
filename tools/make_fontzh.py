#!/usr/bin/env python3
"""
按需生成中文字库子集（fontzh_data.c）。

为什么要子集：源字体 fusion-pixel-8px 有 27689 个字形，按 8 字节点阵算约 221KB，
CH559 的 code 空间只有 60KB —— 放不下整份。所以只取固件真正用到的字。

字表怎么来（**按需导入**）：
  默认就是**扫固件源码里字符串字面量中的中文** —— 只有 "..." 里的才算，注释里的
  中文不算（不然注释里的字全被算进子集，白白占空间）。所以加字的方式是：
  在 .c 里把中文写进字符串，然后重新跑一次本脚本。

源字体是**纯内嵌点阵**（只有 EBDT/EBLC，glyf 是空的），所以：
  * 只能按它的设计尺寸渲染（这里 8ppem），放大渲染会得到空白；
  * Pillow 在 8px 下渲染出来是纯 1 位，没有灰边 —— 阈值取 127 就够。
设计尺寸直接从 EBLC 的 bitmapSizeTable 里读 ppemX/ppemY，不写死。

用法：
    python tools/make_fontzh.py                    # 扫源码 → 生成 fontzh_data.c
    python tools/make_fontzh.py --list             # 只报告会取哪些字、占多少字节
    python tools/make_fontzh.py --chars "电压测试"  # 手动指定字表（调试用）
    python tools/make_fontzh.py --ttf other.ttf    # 换字体（比如 12px/16px 变体）
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:                                     # pragma: no cover
    sys.exit("需要 Pillow：pip install Pillow")

ROOT = Path(__file__).resolve().parent.parent           # NTP_PRO_II/
DEFAULT_TTF = ROOT / "tools" / "fusion-pixel-8px-monospaced-zh_hans.ms.bitmap.ttf"
OUT_C = ROOT / "fontzh_data.c"

# 扫字表时跳过的目录（生成物自己、构建产物）
SKIP_DIRS = {"out", "build", "dist", "dist_onedir", ".git", "__pycache__"}


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def sfnt_tables(ttf: Path) -> dict:
    d = ttf.read_bytes()
    n = struct.unpack(">H", d[4:6])[0]
    t = {}
    for i in range(n):
        tag, _c, o, ln = struct.unpack(">4sIII", d[12 + i * 16:28 + i * 16])
        t[tag.decode("latin1")] = (o, ln)
    return t


def strike_size(ttf: Path) -> tuple:
    """从 EBLC 的 bitmapSizeTable 读设计像素尺寸与位深。

    EBLC 头: version(4) numSizes(4)；每个 bitmapSizeTable 48 字节，
    其中偏移 44 = ppemX、45 = ppemY、46 = bitDepth、47 = flags。
    """
    d = ttf.read_bytes()
    tabs = sfnt_tables(ttf)
    if "EBLC" not in tabs:
        sys.exit(f"{ttf.name} 里没有 EBLC —— 这不是内嵌点阵字体，本脚本不适用")
    o, _ = tabs["EBLC"]
    num_sizes = struct.unpack(">I", d[o + 4:o + 8])[0]
    if num_sizes < 1:
        sys.exit("EBLC 里没有 bitmapSizeTable")
    px, py, depth = d[o + 8 + 44], d[o + 8 + 45], d[o + 8 + 46]
    if num_sizes > 1:
        print(f"   注意：字体有 {num_sizes} 档点阵，本脚本只用第一档 ({px}x{py}, {depth}bpp)")
    return px, py, depth


def strip_comments_keep_strings(src: str) -> str:
    """剥掉注释，但**保留字符串字面量** —— 字表就是从字面量里取的。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\":
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == q:
                    i += 1
                    break
                i += 1
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def collect_cjk_from_sources() -> set:
    """扫 .c/.h 的字符串字面量，收集其中的非 ASCII 字符（也就是中文 + 中文标点）。"""
    chars = set()
    files = []
    for p in sorted(ROOT.rglob("*")):
        if p.suffix not in (".c", ".h") or not p.is_file():
            continue
        if any(part in SKIP_DIRS for part in p.parts):
            continue
        files.append(p)

    for p in files:
        code = strip_comments_keep_strings(read(p))
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', code):
            for ch in lit:
                if ord(ch) > 0x7F:
                    chars.add(ch)
    print(f"   扫了 {len(files)} 个源文件")
    return chars


def render_glyph(font, ch: str, w: int, h: int) -> tuple:
    """把字渲染进 w x h 的格子。返回 (点阵字节, 是否有墨, 是否被裁掉)。

    字节布局与 oledBinary() 一致：byte[f] 的 bit[row%8]，f = w*(row/8) + col。
    Pillow 的 draw.text((0,0)) 把 ascender 放在 y=0，对本字体正好落在 8 行格子里。
    """
    probe_h = h + 6                                     # 多留几行，用来发现"被裁掉"
    im = Image.new("L", (w, probe_h), 0)
    ImageDraw.Draw(im).text((0, 0), ch, font=font, fill=255)
    px = im.load()

    segs = h // 8
    out = bytearray(w * segs)
    ink = False
    for seg in range(segs):
        for col in range(w):
            b = 0
            for r in range(8):
                if px[col, seg * 8 + r] > 127:
                    b |= 1 << r
                    ink = True
            out[seg * w + col] = b
    clipped = any(px[col, row] > 127 for row in range(h, probe_h) for col in range(w))
    for col in range(w):
        for seg in range(segs):
            _ = col
    return bytes(out), ink, clipped


def art(bits: bytes, w: int, h: int) -> list:
    segs = h // 8
    rows = []
    for row in range(h):
        s = ""
        for col in range(w):
            s += "#" if (bits[(row // 8) * w + col] >> (row % 8)) & 1 else "·"
        rows.append(s)
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description="生成中文字库子集 fontzh_data.c")
    ap.add_argument("--ttf", default=str(DEFAULT_TTF))
    ap.add_argument("--chars", default=None, help="手动指定字表（不给就扫源码）")
    ap.add_argument("--list", action="store_true", help="只报告，不写文件")
    ap.add_argument("--show", type=int, default=0, help="打印前 N 个字的点阵预览")
    args = ap.parse_args()

    ttf = Path(args.ttf)
    if not ttf.exists():
        sys.exit(f"找不到字体 {ttf}")

    w, h, depth = strike_size(ttf)
    print(f"源字体 {ttf.name}: 设计点阵 {w}x{h}, {depth}bpp")

    # ⚠️ 硬拦截：oledBinary() 按 byte[f] 的 bit[row%8]、f = W*(row/8)+col 打包，
    #    要求 **宽是 4 的倍数、高是 8 的倍数**（每 8 行一段、每行 W/4 个字）。
    #    不满足时本脚本会算出 per = w*h/8 字节，而 render_glyph 只能写出 w*(h//8) 字节
    #    —— 静默产出长度不符的错数据。宁可在这里直接报错。
    #    12px 那档字体是 12x12，两个条件都不满足：要支持它得加一个绘制原语
    #    （12x12 按 6 字节/行打包、走 oledTransfer 直发），不是调参数能解决的。
    if w % 4 or h % 8:
        sys.exit(
            f"❌ {w}x{h} 的格子画不了：oledBinary() 要求宽是 4 的倍数、高是 8 的倍数。\n"
            f"   要支持这一档字体得先给固件加一个绘制原语（见本文件注释）。")

    if args.chars:
        chars = {c for c in args.chars if ord(c) > 0x7F}
        print(f"   字表来自 --chars")
    else:
        chars = collect_cjk_from_sources()

    if not chars:
        print("\n源码里没有中文字符串字面量 —— 子集会是空的。")
        print("先在 .c 的字符串里写中文（注释里的不算），再跑一次。")
        return 0

    ordered = sorted(chars, key=ord)
    per = (w * h) // 8
    print(f"\n字表: {len(ordered)} 个字，点阵 {len(ordered) * per} 字节"
          f"（整份字体 {27689 * per:,} 字节，占了 {100.0 * len(ordered) / 27689:.2f}%）")
    print("   " + "".join(ordered))

    font = ImageFont.truetype(str(ttf), h)
    recs, blank, clipped, samples = [], [], [], []
    for ch in ordered:
        bits, ink, clip = render_glyph(font, ch, w, h)
        if not ink:
            blank.append(ch)
            continue
        if clip:
            clipped.append(ch)
        recs.append((ord(ch), bits))
        if len(samples) < 3:
            samples.append((ch, bits))

    if blank:
        print(f"\n⚠️ 这些字渲染出来是空白（字体里没有？）：{''.join(blank)}")
    if clipped:
        print(f"\n⚠️ 这些字超出 {w}x{h} 格子、被裁掉了：{''.join(clipped)}")
    if args.show:
        for ch, bits in samples[:args.show]:
            print(f"\n   '{ch}'  U+{ord(ch):04X}")
            for r in art(bits, w, h):
                print("      " + r)

    if args.list:
        print("\n（--list：没写文件）")
        return 0

    guard = f"FONTZH_{w}x{h}_{len(recs)}"
    lines = [
        "/* ============================================================================",
        " * 中文字库子集 —— **本文件由 tools/make_fontzh.py 生成，别手改！**",
        " *",
        f" * 源字体 : {ttf.name}（内嵌点阵 {w}x{h}, {depth}bpp）",
        " * 字表来源: NTP_PRO_II 源码里字符串字面量中的中文（注释里的不算）",
        f" * 字数   : {len(recs)}，点阵 {len(recs) * per} 字节",
        " *",
        " * 加字的办法：在 .c 里把中文写进字符串，再跑一次 `python tools/make_fontzh.py`。",
        " * 字节布局与 oledBinary() 一致：byte[f] 的 bit[row%8]，f = W*(row/8) + col。",
        " * ========================================================================== */",
        "",
        '#include "fontzh.h"',
        "",
        f"/* 这行只是给 grep 用的哨兵，改动字表时会跟着变 */",
        f"/* {guard} */",
        "",
        f"const uint16_t fontzh_count = {len(recs)}u;",
        "",
        "/* 码位，升序 —— fontzhGet() 线性查它 */",
        "const uint16_t fontzh_codes[] = {",
    ]
    for i in range(0, len(recs), 10):
        chunk = recs[i:i + 10]
        lines.append("    " + " ".join(f"0x{c:04X}," for c, _ in chunk))
    lines += ["};", "", f"/* 每个字 {per} 字节，按 fontzh_codes 的顺序排 */", "const uint8_t fontzh_bits[] = {"]
    for i, (c, bits) in enumerate(recs):
        ch = chr(c)
        lines.append(f"    /* {i:3d}  {ch}  U+{c:04X} */")
        for j in range(0, per, w):
            lines.append("    " + " ".join(f"0x{b:02X}," for b in bits[j:j + w]))
    lines += ["};", ""]

    OUT_C.write_text("\n".join(lines), encoding="utf-8", newline="\r\n")
    print(f"\n写出 {OUT_C.relative_to(ROOT)}  ({OUT_C.stat().st_size:,} 字节)")
    print(f"   共 {len(recs)} 字 × {per} 字节 = {len(recs) * per} 字节点阵")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
