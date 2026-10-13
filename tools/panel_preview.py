#!/usr/bin/env python3
"""
面板显示预览 —— 把 main.c 里 oledShowLocal() / oledShowUsb() 要画的两屏，
用 font.c 里**真实**的 font_8x16 字模渲染成 PNG，不用上板就能核对布局。

为什么值得单独放一个工具：
  * 字段宽度不够会在屏上留下上一帧的残影，这种问题肉眼看代码很难发现；
  * "行会不会超过屏宽 32 字符" 一算就知道，但手算容易漏掉边界的负号；
  * 字库只有 ASCII，想加中文标签得先看到"放不下"；
  * 方向那一格来自 AIN_2，屏上长什么样要能直接看。

防漂移：布局和格式**都从 main.c 里解析**，不在本脚本里再抄一份。
  从 main.c 取：OLED_TITLE_Y / OLED_LINE0_Y / OLED_LINE_DY、各组 sprintf 格式串、
                标题文字与颜色（oledStringC 那一行）。
  只有"方向三字母"和"按钮位图"这两处是手工拼的，这里按同样规则复刻并断言字面量还在。

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
MAIN_C = ROOT / "main.c"

SCREEN_W, SCREEN_H = 256, 64
CHAR_W, CHAR_H = 8, 16
COLS = SCREEN_W // CHAR_W                               # 一行 32 个字符
FONT_NAME = "font_8x16"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def strip_comments(s: str) -> str:
    """两种注释都要剥！font.c 的 font_8x16 用的是 /* 0x21 [!] */ 块注释，
    只剥 // 的话注释里的 0x21 会被当成数据收进来（1536 会变 1632）。"""
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\r\n]*", "", s)


def load_font() -> bytes:
    m = re.search(rf"uint8_t\s+{FONT_NAME}\s*\[\s*\]\s*=\s*\{{(.*?)\n\}};",
                  read(FONT_C), re.S)
    if not m:
        sys.exit(f"在 {FONT_C} 里找不到 {FONT_NAME}[]")
    data = bytes(int(x, 16) for x in
                 re.findall(r"0x([0-9A-Fa-f]{2})", strip_comments(m.group(1))))
    if len(data) % 16:
        sys.exit(f"{FONT_NAME} 长度 {len(data)} 不是 16 的倍数，字模布局变了？")
    return data


def load_layout() -> dict:
    """从 main.c 抓布局常量与标题，避免和固件脱节。"""
    src = strip_comments(read(MAIN_C))

    def macro(name: str) -> int:
        m = re.search(rf"#define\s+{name}\s+(\d+)", src)
        if not m:
            sys.exit(f"main.c 里找不到 #define {name}")
        return int(m.group(1))

    m = re.search(r'oledStringC\(\s*\d+\s*,\s*(\w+)\s*,\s*(\d+)\s*,\s*'
                  r'(0x[0-9A-Fa-f]+)\s*,\s*"([^"]*)"', src)
    if not m:
        sys.exit("main.c 里找不到画标题的 oledStringC(...)")
    if m.group(1) != "OLED_TITLE_Y":
        sys.exit(f"标题的 y 不是 OLED_TITLE_Y 而是 {m.group(1)}，本脚本要同步")

    return {
        "title_y": macro("OLED_TITLE_Y"),
        "line0_y": macro("OLED_LINE0_Y"),
        "line_dy": macro("OLED_LINE_DY"),
        "title_color": int(m.group(3), 16),
        "title_text": m.group(4),
    }


def load_formats() -> dict[int, list[str]]:
    """从 main.c 抓所有 sprintf(oled_line[n], "...") 的格式串。"""
    src = strip_comments(read(MAIN_C))
    out: dict[int, list[str]] = {}
    for n, fmt in re.findall(r'sprintf\(oled_line\[(\d)\],\s*"((?:[^"\\]|\\.)*)"', src):
        out.setdefault(int(n), []).append(fmt)
    if not out:
        sys.exit(f"没能从 {MAIN_C} 里抓到任何 sprintf(oled_line[...]) 格式串")
    return out


def require_literals(*lits: str) -> None:
    src = strip_comments(read(MAIN_C))
    for lit in lits:
        if lit not in src:
            sys.exit(f"main.c 里找不到 {lit!r} —— 手工拼接那部分改过了，请同步本脚本")


def draw_text(px, font: bytes, x0: int, y0: int, s: str, color: int) -> None:
    """按 oledBinary() 的规则画：每个字符格先填背景色，再把点亮的像素涂前景色。
    color 的低 4 位 = 前景（亮），高 4 位 = 背景。"""
    fg, bg = color & 0x0F, (color >> 4) & 0x0F
    for i, ch in enumerate(s):
        o = (ord(ch) - 0x20) * 16
        if o < 0 or o + 16 > len(font):
            continue
        for col in range(CHAR_W):
            bits = font[o + col] | (font[o + 8 + col] << 8)     # 低 8 行 | 高 8 行
            for row in range(CHAR_H):
                x, y = x0 + i * CHAR_W + col, y0 + row
                if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H:
                    px[x, y] = ((fg if (bits >> row) & 1 else bg)) * 17


def put_screen(im: Image.Image, font: bytes, layout: dict, lines: list[str]) -> None:
    px = im.load()
    for i, s in enumerate(lines):
        draw_text(px, font, 0, layout["line0_y"] + i * layout["line_dy"], s, 0x0F)
    t = layout["title_text"]
    draw_text(px, font, (SCREEN_W - len(t) * CHAR_W) // 2,
              layout["title_y"], t, layout["title_color"])


# ---------------------------------------------------------------- 两屏的内容
#
# 取值按各自实际范围给：Vset 0~14329mV、Fpwm 3.4~218.8kHz、Duty 0~100%、
# Voff 0~2047mV、Isen -3125~3125mA、轴 -1023~1023。

DIR_TXT = {1: "FWD", 0: "NEU", -1: "REV"}


def local_screen(fmt: dict, vset: int, dhz: int, duty: int, voff: int,
                 isen: int, d: int, estop: bool) -> list[str]:
    dt = "STP" if estop else DIR_TXT[d]
    return [fmt[0][0] % (vset // 1000, (vset % 1000) // 10, dhz // 10, dhz % 10),
            fmt[1][0] % (duty, voff),
            (fmt[2][0] % isen) + dt]


def usb_screen(fmt: dict, ax: int, ay: int, d: int, btn: int, isen: int) -> list[str]:
    require_literals("Dir ", "Btn ", "oled_dir_txt")
    # 与 main.c 的循环逐字对应：bit0~bit4 -> K1~K5
    btn_txt = "".join(f"K{i + 1}" if (btn >> i) & 1 else "--" for i in range(5))
    return [fmt[0][-1] % isen,
            (fmt[1][-1] % (ax, ay)) + DIR_TXT[d],
            "Btn " + btn_txt]


def main() -> int:
    ap = argparse.ArgumentParser(description="渲染 main.c 的两块 OLED 屏")
    ap.add_argument("--out", default=str(ROOT / "tools" / "out" / "panel_preview.png"))
    args = ap.parse_args()

    font = load_font()
    fmts = load_formats()
    layout = load_layout()
    print(f"字模 {FONT_NAME}: {len(font)} 字节 = {len(font)//16} 个字形")
    print(f"布局（从 main.c 解析）: 标题 y={layout['title_y']} 色=0x{layout['title_color']:02X} "
          f"{layout['title_text']!r}；读数 y={layout['line0_y']}/"
          f"{layout['line0_y']+layout['line_dy']}/{layout['line0_y']+2*layout['line_dy']}")
    print(f"格式串: 抓到 {sum(len(v) for v in fmts.values())} 条 (oled_line 下标 {sorted(fmts)})")

    cases = [
        ("LOCAL  典型(PWM+额定12V)", local_screen(fmts, 12000, 1094, 100, 2047, 1250, 1, False)),
        ("LOCAL  典型(电机+v_off*6)", local_screen(fmts, 12282, 341, 50, 613, -820, -1, False)),
        ("LOCAL  急停（三灯灭 + Dir=STP）", local_screen(fmts, 12000, 1094, 80, 2047, 900, 1, True)),
        ("LOCAL  边界 min", local_screen(fmts, 0, 34, 0, 0, -3125, -1, False)),
        ("LOCAL  边界 max", local_screen(fmts, 14329, 2188, 100, 2047, 3125, 1, False)),
        ("USB    X/Y + Dir FWD + K1 K3", usb_screen(fmts, 1023, -1023, 1, 0b00101, 1250)),
        ("USB    Dir REV + K2 K4 K5", usb_screen(fmts, -500, 0, -1, 0b11010, 0)),
        ("USB    边界 min", usb_screen(fmts, -1023, -1023, -1, 0b00000, -3125)),
        ("USB    边界 max", usb_screen(fmts, 1023, 1023, 1, 0b11111, 3125)),
    ]

    bad = []
    print(f"\n每行最多 {COLS} 字符；同一块屏的行长必须彼此恒定：")
    groups: dict[str, list[list[int]]] = {}
    for name, lines in cases:
        lens = [len(s) for s in lines]
        groups.setdefault(name.split()[0], []).append(lens)
        if any(n > COLS for n in lens):
            bad.append(f"{name}: 行长 {lens} 超出 {COLS}")
        print(f"   {name:<30} 行长 {lens} {'✘ 超宽!' if any(n > COLS for n in lens) else '✔'}")
        for s in lines:
            print(f"        |{s}|")

    for tag, group in groups.items():
        uniq = {tuple(g) for g in group}
        if len(uniq) != 1:
            bad.append(f"{tag} 屏在不同取值下行长不一致：{sorted(uniq)} —— 会留残影")
    if bad:
        print("\n有问题：")
        for b in bad:
            print("   -", b)
        return 1
    print("\n行长检查：全部恒定且在屏宽内 ✅")

    sc = 3
    im = Image.new("L", (SCREEN_W * sc + 16, (SCREEN_H * sc + 26) * len(cases) + 8), 40)
    d = ImageDraw.Draw(im)
    for i, (name, lines) in enumerate(cases):
        top = 8 + i * (SCREEN_H * sc + 26)
        d.text((8, top), name, fill=255)
        screen = Image.new("L", (SCREEN_W, SCREEN_H), 0)
        put_screen(screen, font, layout, lines)
        im.paste(screen.resize((SCREEN_W * sc, SCREEN_H * sc), Image.NEAREST),
                 (8, top + 18))

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    im.save(out)
    print(f"\n写出: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
