"""对照演示：同一个打包/解码模型下，"行距 = w"（现在的固件）与"行距 = 48"（旧的 bug）
画出来是什么样。用你报的那三段：偏置右侧的 " 2047mV"、X轴/Y轴 的 "X"、状态栏的 "USB"。"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import panel_preview as pp   # noqa: E402  （它的 main() 有 __main__ 保护，import 安全）

ROOT = Path(__file__).resolve().parent.parent
F6 = pp.grab_bytes((ROOT / "font.c").read_text(encoding="utf-8"), "font_6x8")

CASES = [("偏置右侧的 \" 2047mV\"", " 2047mV", 7), ("X轴/Y轴 的 \"X\"", "X", 1), ("状态栏的 \"USB\"", "USB", 3)]
SC, ROWH = 3, 12 * 3 + 22

out = Image.new("L", (260 * SC + 20, ROWH * len(CASES) + 30), 40)
d = ImageDraw.Draw(out)
d.text((8, 6), "上=行距 w (现在的固件, 正确)   下=行距 48 (旧 bug)", fill=255)

for i, (name, s, run) in enumerate(CASES):
    w = ((6 * run + 3) // 4) * 4
    top = i * ROWH + 26
    d.text((8, top), f"{name}   w={w}", fill=255)
    for k, (buf_w, label) in enumerate(((w, "w"), (48, "48"))):
        im = Image.new("L", (260, 12), 0)
        px = im.load()
        buf = pp.pack_ascii_buf(F6, s, run, buf_w, 15, 0)
        for r, row in enumerate(pp.decode_transfer(buf, w, 8)):
            for c, v in enumerate(row):
                if c < 260 and r + 2 < 12:
                    px[c, r + 2] = v * 17
        out.paste(im.resize((260 * SC, 12 * SC), Image.NEAREST), (8, top + 14 + k * (12 * SC)))
    out.paste(Image.new("L", (260 * SC, 4), 80), (8, top + 14 + 12 * SC))

p = ROOT / "tools" / "out" / "preview_stride_demo.png"
out.save(p)
print(f"写出: {p}")
print("对照（前 24 个像素，行 0~7）：")
for name, s, run in CASES:
    w = ((6 * run + 3) // 4) * 4
    a = pp.decode_transfer(pp.pack_ascii_buf(F6, s, run, w, 15, 0), w, 8)
    b = pp.decode_transfer(pp.pack_ascii_buf(F6, s, run, 48, 15, 0), w, 8)
    差 = sum(1 for r in range(8) for c in range(w) if a[r][c] != b[r][c])
    print(f"   {name:28s} w={w:2d}  不同的像素数 = {差:3d} / {8*w}")
