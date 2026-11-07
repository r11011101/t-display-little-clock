#!/usr/bin/env python3
# 產生 170x320(直向) RGB565 big-endian 漸層背景 C 檔, 存到 sketch 資料夾
import struct

W, H = 170, 320

def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def lerp(a, b, t):
    return int(a + (b - a) * t)

# 配色: 深靛藍頂 -> 午夜紫中 -> 近黑藍底（低亮度, 不搶前景）
stops = [(16, 24, 58), (34, 22, 60), (7, 10, 22)]

def bg_color(y):
    t = y / (H - 1)
    if t < 0.55:
        u = t / 0.55
        c = [lerp(stops[0][i], stops[1][i], u) for i in range(3)]
    else:
        u = (t - 0.55) / 0.45
        c = [lerp(stops[1][i], stops[2][i], u) for i in range(3)]
    return rgb565(*c)

px = bytearray()
for y in range(H):
    v = bg_color(y)
    for x in range(W):
        px += struct.pack('>H', v)          # big-endian RGB565

out = []
out.append("// 自動生成: 170x320 RGB565 (big-endian) 漸層背景, 勿手改")
out.append("#pragma once")
out.append(f"#define BG_W {W}")
out.append(f"#define BG_H {H}")
out.append("static const uint8_t bgBitmap[] PROGMEM = {")
for i in range(0, len(px), 24):
    out.append(",".join(f"0x{b:02X}" for b in px[i:i+24]) + ",")
out.append("};")

path = r"D:/my_ipynb/t-display 小時鐘/sketch_nov7b/sketch_nov7b/bg_gradient.h"
with open(path, "w") as f:
    f.write("\n".join(out) + "\n")
print("written:", path, "bytes:", len(px))
