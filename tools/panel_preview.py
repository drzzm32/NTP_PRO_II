#!/usr/bin/env python3
"""
面板显示预览 —— 把 main.c 里 uiShowLocal() / uiShowUsb() 要画的两屏，用 font.c 里
**真实的** font_8x8 / font_8x16 字模 + main.c 里**真实的**方向箭头字模渲染成 PNG，
不用上板就能核对布局。

为什么值得单独放一个工具：
  * 扁平化界面里"文本有没有越界"、"条形图和数值对不对得上位"这类问题，肉眼看代码
    很难发现，出张图一眼就看出来；
  * 一次 8x16 重画是几毫秒，改布局前先看看值不值得；
  * 字库只有 ASCII，想加中文标签得先看到"放不下"。

防漂移：**布局常量、调色板、格式串、箭头字模都从 main.c 里解析**，不在本脚本里
再抄一份。只有"绘制顺序"是复刻的（这部分没法解析，改 main.c 的绘制流程时要同步）。

用法：
    python tools/panel_preview.py                 # -> tools/out/panel_preview.png
    python tools/panel_preview.py --out x.png

只依赖 Pillow。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:                                     # pragma: no cover
    sys.exit("需要 Pillow：pip install Pillow")

ROOT = Path(__file__).resolve().parent.parent           # NTP_PRO_II/
FONT_C = ROOT / "font.c"
FONTZH_C = ROOT / "fontzh_data.c"
FONTZH_H_C = ROOT / "fontzh_data.h"                  # 生成的尺寸头                       # 生成的中文子集
MAIN_C = ROOT / "main.c"

# 中文字形尺寸 / 混排时 6x8 ASCII 的下移量：由 load_fontzh() 从生成的头文件填进来
ZH_W, ZH_H, ZH_DY = 12, 12, 3

SCREEN_W, SCREEN_H = 256, 64

LAYOUT_MACROS = ("UI_BAND_Y", "UI_BAND_H", "UI_ROW1_Y", "UI_ROW2_Y",
                 "UI_INFO1_Y", "UI_BAR_X", "UI_BAR_W", "UI_BAR_H",
                 "UI_BAR_DY", "UI_VAL_X", "UI_ARROW_X", "UI_BAND_TITLE_X",
                 "UI_BAND_STOP_X", "UI_BAND_MODE_X", "UI_BAND_PWM_X",
                 "UI_VSET_FULL_MV", "UI_AXIS_FULL")

PALETTE_MACROS = ("UI_BAND_BG", "UI_TXT", "UI_LABEL", "UI_TRACK", "UI_FILL",
                  "UI_BAND_TXT", "UI_BAND_DIM", "UI_DIR_HOT", "UI_DIR_DIM")

REQUIRED_LITERALS = ("NTP PRO II", "PWM", "电  压", "占空比", "X  轴", "Y  轴",
                     "电流", "频率", "偏置", "急停", "本机", "USB", "按键", "电  压")

# main.c 里 ui_line 是 char[4][40]，格式化后的 **UTF-8 字节数**不能超这个
LINE_BUF = 48          # main.c 里现在是 char ui_line[4][48]


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def strip_comments(s: str) -> str:
    """两种注释都要剥！font.c 的 font_8x16 用的是 /* 0x21 [!] */ 块注释，
    只剥 // 的话注释里的 0x21 会被当成数据收进来（1536 会变 1632）。"""
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\r\n]*", "", s)


def grab_bytes(src: str, name: str) -> bytes:
    m = re.search(rf"uint8_t\s+{name}\s*\[\s*(\d*)\s*\]\s*=\s*\{{(.*?)\n\}};", src, re.S)
    if not m:
        sys.exit(f"找不到数组 {name}[]")
    return bytes(int(x, 16) for x in
                 re.findall(r"0x([0-9A-Fa-f]{2})", strip_comments(m.group(2))))


def load_fontzh() -> tuple:
    """读 tools/make_fontzh.py 生成的中文子集。

    尺寸**从 fontzh_data.h 读**，不写死 —— 现在这档是 12x12（每字 24 字节：
    12 列 × 2 段），换成 8x8 那档也能直接用。
    返回 ({码位: 点阵}, W, H, 混排时 6x8 ASCII 要下移的像素数)。
    """
    if not FONTZH_C.exists() or not FONTZH_H_C.exists():
        print("   （没有 fontzh_data.c/.h，中文会按缺字画成空白）")
        return {}, 12, 12, 3
    hs = read(FONTZH_H_C)

    def hdef(name: str) -> int:
        m = re.search(rf"#define\s+{name}\s+(\d+)", hs)
        if not m:
            sys.exit(f"fontzh_data.h 里找不到 {name}")
        return int(m.group(1))

    w, h, per, dy = hdef("FONTZH_W"), hdef("FONTZH_H"), hdef("FONTZH_BYTES"), hdef("FONTZH_DY")
    s = strip_comments(read(FONTZH_C))
    m = re.search(r"fontzh_codes\[\]\s*=\s*\{(.*?)\};", s, re.S)
    codes = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{4})", m.group(1))] if m else []
    bits = grab_bytes(s, "fontzh_bits")
    zh = {c: bits[i * per:(i + 1) * per] for i, c in enumerate(codes)
          if len(bits[i * per:(i + 1) * per]) == per}
    return zh, w, h, dy


def macro(mc: str, name: str) -> int:
    m = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+|\d+)", mc)
    if not m:
        sys.exit(f"main.c 里找不到 #define {name}")
    return int(m.group(1), 0)


def draw_text(px, font: bytes, x0: int, y0: int, s: str, color: int,
              w: int = 8, h: int = 16) -> None:
    """按 oledBinary() 的规则画：每个字符格先铺背景色、再把点亮的像素涂前景色。
    color 低 4 位 = 前景，高 4 位 = 背景。"""
    fg, bg = color & 0x0F, (color >> 4) & 0x0F
    segs = h // 8
    for i, ch in enumerate(s):
        o = (ord(ch) - 0x20) * segs * w
        if o < 0 or o + segs * w > len(font):
            continue
        for col in range(w):
            for seg in range(segs):
                bits = font[o + seg * w + col]
                for r in range(8):
                    x, y = x0 + i * w + col, y0 + seg * 8 + r
                    if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H:
                        px[x, y] = ((fg if (bits >> r) & 1 else bg)) * 17


def draw_glyph(px, glyph: bytes, x0: int, y0: int, w: int, h: int, color: int) -> None:
    fg, bg = color & 0x0F, (color >> 4) & 0x0F
    for col in range(w):
        for row in range(h):
            f = w * (row // 8) + col
            if f >= len(glyph):
                continue
            x, y = x0 + col, y0 + row
            if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H:
                px[x, y] = ((fg if (glyph[f] >> (row % 8)) & 1 else bg)) * 17


def draw_utf8(px, font: bytes, zh: dict, x0: int, y0: int, s: str, color: int,
              w: int = 8, h: int = 16) -> int:
    """画一行：中文走子集字库（在 h 里垂直居中），其余交给传入的 ASCII 点阵字库。
    返回结束时的 x。"""
    x = x0
    for ch in s:
        if ch == "\n":
            x = 0
            y0 += h + 2
            continue
        if ord(ch) < 0x80:
            draw_text(px, font, x, y0, ch, color, w, h)
        else:
            g = zh.get(ord(ch))
            if g:
                draw_glyph(px, g, x, y0 + (h - ZH_H) // 2, ZH_W, ZH_H, color)
        x += ZH_W if ord(ch) >= 0x80 else w
    return x


def pack_ascii_buf(font6: bytes, s: str, run: int, buf_w: int,
                   fg: int, bg: int) -> bytearray:
    """**忠实模拟固件的 oledString6N()**：把 run 个字按 6 像素步进合成进一个
    "每行 buf_w 像素" 的 nibble 缓冲（buf_w 对应固件里的 bl_w）。

    偶数像素写高 nibble（高半字节是左边那个像素）、奇数写低 nibble —— 同 oledBinary。
    """
    buf = bytearray((buf_w // 2) * 8)
    fill = (bg << 4) | bg
    for i in range(len(buf)):
        buf[i] = fill
    for i, ch in enumerate(s[:run]):
        o = (ord(ch) - 0x20) * 6
        if not (0 <= o and o + 6 <= len(font6)):
            continue
        for col in range(6):
            m = font6[o + col]
            for r in range(8):
                ox = i * 6 + col
                q = r * (buf_w // 2) + (ox >> 1)
                if q >= len(buf):
                    continue
                v = fg if (m >> r) & 1 else bg
                if ox & 1:
                    buf[q] = (buf[q] & 0xF0) | v
                else:
                    buf[q] = (buf[q] & 0x0F) | (v << 4)
    return buf


def decode_transfer(buf: bytearray, w: int, rows: int) -> list:
    """**忠实模拟固件的 oledTransfer(x, y, w, rows, ptr, 0)**：位图是"顺序发
    w*rows/2 字节"，也就是按 **w/2 字节一行** 读。缓冲区行距不等于 w 时，
    读出来的行就会整体错位 —— 真机上的花屏就是这么来的。
    """
    out = []
    for r in range(rows):
        row = []
        for c in range(w):
            idx = r * (w // 2) + (c >> 1)
            b = buf[idx] if 0 <= idx < len(buf) else 0
            row.append((b >> 4) if (c & 1) == 0 else (b & 0x0F))
        out.append(row)
    return out


def stride_selftest(font6: bytes) -> bool:
    """确认这个模型**真的能抓到行距 bug**：对"1 个字"（w=8）这种短段，
    用 buf_w=48 合成、按 w=8 解码，结果必须和 buf_w=w 时不同。
    如果两者相同，说明模型没在模拟真实打包，这道自检就是假的。
    """
    bad = 0
    for run, w in ((1, 8), (3, 20), (7, 44)):
        s = ("X", "USB", " 2047mV")[((1, 3, 7).index(run))]
        ok = decode_transfer(pack_ascii_buf(font6, s, run, w, 15, 0), w, 8)
        bug = decode_transfer(pack_ascii_buf(font6, s, run, 48, 15, 0), w, 8)
        if ok == bug:
            print(f"   ✗ 自检失败：{s!r} (w={w}) 用错行距却解出一样的结果，模型不可信")
            bad += 1
    if not bad:
        print("   行距自检 ✅ 模型能复现/预防'缓冲行距 != 传输宽度'这类花屏")
    return bad == 0


def draw_mix6(px, font6: bytes, zh: dict, x0: int, y0: int, s: str, color: int) -> int:
    """镜像固件的 oledStringZh()：ASCII 走 6x8、中文走子集字库（都在 16px 行里）。

    * 中文在行内垂直居中：(16 - ZH_H) // 2。
    * ASCII 再下移 ZH_DY 做基线对齐。
    * ASCII 段**走 pack_ascii_buf + decode_transfer 的真实打包/解码路径**
      （不是直接画像素）—— 这样"缓冲行距 != 传输宽度"这类内存布局错误
      在预览里也会现形，而不是渲染得好好的、真机却花屏。
    * 推进量用补齐到 4 的倍数后的宽度，和固件一致（一次最多 8 个字）。
    """
    fg, bg = color & 0x0F, (color >> 4) & 0x0F
    top = (16 - ZH_H) // 2
    x = x0
    while s:
        if ord(s[0]) >= 0x80:
            g = zh.get(ord(s[0]))
            if g:
                draw_glyph(px, g, x, y0 + top, ZH_W, ZH_H, color)
            x += ZH_W
            s = s[1:]
            continue
        run = 0
        while run < 8 and run < len(s) and ord(s[run]) < 0x80:
            run += 1
        w = ((6 * run + 3) // 4) * 4
        # 固件里缓冲行距就是 w（oledString6N 里 bl_w = w）；这里也保持一致。
        buf = pack_ascii_buf(font6, s, run, w, fg, bg)
        for r, row in enumerate(decode_transfer(buf, w, 8)):
            for c, v in enumerate(row):
                xx, yy = x + c, y0 + top + ZH_DY + r
                if 0 <= xx < SCREEN_W and 0 <= yy < SCREEN_H:
                    px[xx, yy] = v * 17
        x += w
        s = s[run:]
    return x


def rect(px, x: int, y: int, w: int, h: int, color: int) -> None:
    """等价于 oledRect()：整块填同一个灰度（color & 0x0F）。"""
    v = (color & 0x0F) * 17
    for yy in range(y, y + h):
        if not (0 <= yy < SCREEN_H):
            continue
        for xx in range(x, x + w):
            if 0 <= xx < SCREEN_W:
                px[xx, yy] = v


def main() -> int:
    ap = argparse.ArgumentParser(description="渲染 main.c 的两块 OLED 屏")
    ap.add_argument("--out", default=str(ROOT / "tools" / "out" / "panel_preview.png"))
    args = ap.parse_args()

    font_src, main_src = read(FONT_C), read(MAIN_C)
    mc = strip_comments(main_src)

    f8 = grab_bytes(font_src, "font_8x8")
    f6 = grab_bytes(font_src, "font_6x8")
    stride_selftest(f6)
    f16 = grab_bytes(font_src, "font_8x16")
    g_up = grab_bytes(mc, "ui_dir_up")
    g_mid = grab_bytes(mc, "ui_dir_mid")
    g_dn = grab_bytes(mc, "ui_dir_dn")
    global ZH_W, ZH_H, ZH_DY
    zh, ZH_W, ZH_H, ZH_DY = load_fontzh()
    print(f"字模: font_8x8 {len(f8)}B/{len(f8)//8} 字, "
          f"font_8x16 {len(f16)}B/{len(f16)//16} 字, 箭头 {len(g_up)}B x3")
    print(f"中文子集: {len(zh)} 个字（来自 {FONTZH_C.name}）"
          f"{''.join(chr(c) for c in sorted(zh))}")

    L = {n: macro(mc, n) for n in LAYOUT_MACROS}
    C = {n: macro(mc, n) for n in PALETTE_MACROS}
    print(f"布局: 状态带 y={L['UI_BAND_Y']}+{L['UI_BAND_H']}; 主行 y={L['UI_ROW1_Y']}/{L['UI_ROW2_Y']}; "
          f"信息行 y={L['UI_INFO1_Y']}")
    print(f"      条 x={L['UI_BAR_X']} w={L['UI_BAR_W']} h={L['UI_BAR_H']} dy={L['UI_BAR_DY']}; "
          f"数值 x={L['UI_VAL_X']}; 箭头列 x={L['UI_ARROW_X']}")

    fmts: dict[int, list[str]] = {}
    for n, f in re.findall(r'sprintf\(ui_line\[(\d)\],\s*"((?:[^"\\]|\\.)*)"', mc):
        fmts.setdefault(int(n), []).append(f)
    if not fmts:
        sys.exit("没能从 main.c 抓到任何 sprintf(ui_line[...]) 格式串")
    for lit in REQUIRED_LITERALS:
        if lit not in mc:
            sys.exit(f"main.c 里找不到字面量 {lit!r} —— 绘制流程改过了，请同步本脚本")
    print(f"格式串: {sum(len(v) for v in fmts.values())} 条, ui_line 下标 {sorted(fmts)}")

    def blank() -> Image.Image:
        return Image.new("L", (SCREEN_W, SCREEN_H), 0)

    def band(px, usb: bool, pwm_on: bool, estop: bool) -> None:
        rect(px, 0, L["UI_BAND_Y"], SCREEN_W, L["UI_BAND_H"], C["UI_BAND_BG"])
        draw_text(px, f16, L["UI_BAND_TITLE_X"], L["UI_BAND_Y"], "NTP PRO II", C["UI_BAND_TXT"])
        # 状态带整条重画，所以急停那两个字不用手动擦（和 main.c 的 uiBand 一致）
        if estop:
            draw_utf8(px, f16, zh, L["UI_BAND_STOP_X"], L["UI_BAND_Y"], "急停", C["UI_BAND_TXT"])
        draw_utf8(px, f16, zh, L["UI_BAND_MODE_X"], L["UI_BAND_Y"],
                  "USB" if usb else "本机", C["UI_BAND_TXT"])
        draw_text(px, f16, L["UI_BAND_PWM_X"], L["UI_BAND_Y"], "PWM",
                  C["UI_BAND_TXT"] if pwm_on else C["UI_BAND_DIM"])

    def bar(px, row_y: int, px_w: int) -> None:
        rect(px, L["UI_BAR_X"], row_y + L["UI_BAR_DY"],
             L["UI_BAR_W"], L["UI_BAR_H"], C["UI_TRACK"])
        if px_w:
            rect(px, L["UI_BAR_X"], row_y + L["UI_BAR_DY"],
                 min(px_w, L["UI_BAR_W"]), L["UI_BAR_H"], C["UI_FILL"])

    def bar_mid(px, row_y: int, v: int) -> None:
        half = L["UI_BAR_W"] // 2
        off = int(v * half / L["UI_AXIS_FULL"])
        rect(px, L["UI_BAR_X"], row_y + L["UI_BAR_DY"],
             L["UI_BAR_W"], L["UI_BAR_H"], C["UI_TRACK"])
        if off > 0:
            rect(px, L["UI_BAR_X"] + half, row_y + L["UI_BAR_DY"],
                 off, L["UI_BAR_H"], C["UI_FILL"])
        elif off < 0:
            rect(px, L["UI_BAR_X"] + half + off, row_y + L["UI_BAR_DY"],
                 -off, L["UI_BAR_H"], C["UI_FILL"])

    def dirs(px, d: int, estop: bool) -> None:
        for y, g, hit in ((L["UI_ROW1_Y"], g_up, d == 1),
                          (L["UI_ROW2_Y"], g_mid, d == 0),
                          (L["UI_INFO1_Y"], g_dn, d == -1)):
            draw_glyph(px, g, L["UI_ARROW_X"], y, 16, 16,
                       C["UI_DIR_HOT"] if (hit and not estop) else C["UI_DIR_DIM"])

    def local(vset, dhz, duty, voff, isen, d, estop, pwm_on):
        im = blank()
        px = im.load()
        band(px, False, bool(pwm_on), bool(estop))
        draw_mix6(px, f6, zh, 0, L["UI_ROW1_Y"], "电  压", C["UI_LABEL"])
        draw_mix6(px, f6, zh, 0, L["UI_ROW2_Y"], "占空比", C["UI_LABEL"])
        bar(px, L["UI_ROW1_Y"], int(vset * L["UI_BAR_W"] / L["UI_VSET_FULL_MV"]))
        bar(px, L["UI_ROW2_Y"], int(duty * L["UI_BAR_W"] / 100))
        dirs(px, d, bool(estop))
        vals = [fmts[0][0] % (vset // 1000, (vset % 1000) // 10),
                fmts[1][0] % duty,
                fmts[2][0] % (isen, dhz // 10, dhz % 10, voff)]
        draw_text(px, f16, L["UI_VAL_X"], L["UI_ROW1_Y"], vals[0], C["UI_TXT"])
        draw_text(px, f16, L["UI_VAL_X"], L["UI_ROW2_Y"], vals[1], C["UI_TXT"])
        draw_mix6(px, f6, zh, 0, L["UI_INFO1_Y"], vals[2], C["UI_TXT"])

        return im, vals

    def usb(ax, ay, d, btn, isen, pwm_on):
        im = blank()
        px = im.load()
        band(px, True, bool(pwm_on), False)
        draw_mix6(px, f6, zh, 0, L["UI_ROW1_Y"], "X  轴", C["UI_LABEL"])
        draw_mix6(px, f6, zh, 0, L["UI_ROW2_Y"], "Y  轴", C["UI_LABEL"])
        bar_mid(px, L["UI_ROW1_Y"], ax)
        bar_mid(px, L["UI_ROW2_Y"], ay)
        dirs(px, d, False)
        # 信息行只有一项：按钮串后面直接接电流
        # （同固件里 sprintf(ui_line[2] + k + 5 * 2, " 电流%6dmA", isen)）
        btn_txt = ("按键 " + "".join(f"K{i + 1}" if (btn >> i) & 1 else "--" for i in range(5))
                   + " 电流%6dmA" % isen)
        vals = [fmts[0][-1] % ax, fmts[1][-1] % ay, btn_txt]
        draw_text(px, f16, L["UI_VAL_X"], L["UI_ROW1_Y"], vals[0], C["UI_TXT"])
        draw_text(px, f16, L["UI_VAL_X"], L["UI_ROW2_Y"], vals[1], C["UI_TXT"])
        draw_mix6(px, f6, zh, 0, L["UI_INFO1_Y"], vals[2], C["UI_TXT"])
        return im, vals

    cases = [
        ("LOCAL  PWM + 额定12V（正向）", local(12000, 1094, 100, 2047, 1250, 1, False, True)),
        ("LOCAL  电机 + pwr*6+v_off（反向）", local(14329, 1094, 100, 2047, -820, -1, False, False)),
        ("LOCAL  急停：三箭头全暗 + STOP", local(12000, 341, 40, 2047, 900, 1, True, True)),
        ("LOCAL  边界 min", local(0, 34, 0, 0, -3125, -1, False, False)),
        ("LOCAL  边界 max", local(14329, 2188, 100, 2047, 3125, 1, False, True)),
        ("USB    X 右 / Y 左（K1 K3）", usb(1023, -1023, 1, 0b00101, 1250, True)),
        ("USB    轴居中（K2 K4 K5）", usb(0, 0, 0, 0b11010, 0, False)),
        ("USB    边界 min", usb(-1023, -1023, -1, 0b00000, -3125, False)),
        ("USB    边界 max", usb(1023, 1023, 1, 0b11111, 3125, True)),
    ]

    bad = []
    print(f"\n越界检查（文本右端必须 < 箭头列 {L['UI_ARROW_X']}）：")
    for name, (im, vals) in cases:
        for i, s in enumerate(vals):
            x0 = L["UI_VAL_X"] if i < 2 else 0
            end = x0 + len(s) * 8
            ok = end <= L["UI_ARROW_X"]
            if not ok:
                bad.append(f"{name} 第{i}段 '{s}' 右端 {end} > {L['UI_ARROW_X']}")
            print(f"   {name[:32]:<34} [{s:<27}] x={x0:>3}..{end:>3} {'✔' if ok else '✘ 越界'}")
    if bad:
        print("\n有问题：")
        for b in bad:
            print("   -", b)
        return 1
    print("   全部在可用区内 ✅")

    sc = 3
    out_im = Image.new("L", (SCREEN_W * sc + 16, (SCREEN_H * sc + 26) * len(cases) + 8), 40)
    d = ImageDraw.Draw(out_im)
    for i, (name, (im, _)) in enumerate(cases):
        top = 8 + i * (SCREEN_H * sc + 26)
        d.text((8, top), name, fill=255)
        out_im.paste(im.resize((SCREEN_W * sc, SCREEN_H * sc), Image.NEAREST), (8, top + 18))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out_im.save(out)
    print(f"\n写出: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
