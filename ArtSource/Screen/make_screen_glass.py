# -*- coding: utf-8 -*-
"""画面の「ガラス」重ね絵（Assets/TD2/Screen/screen_glass.png）を作る。

2D の画面が箱に貼った平らな絵に見えないよう、ブラウン管らしさを最小限だけ足す。
- 縁と角の減光：丸みのある管面の周辺が暗く落ちる。遊びに関わる中央は触らない。
- 左上のにじんだ映り込み：ガラスの存在を示す程度のごく弱い白。
走査線は 3D で斜めから見るとモアレになるので入れない。弱い階調は 8 bit で等高線状の段が出るため、
量子化の前に画素ごとの揺らぎ（ディザ）を足す。画布の縦横比に合わせて引き伸ばされる前提で、
どの比率でも破綻しない滑らかな形だけを使う。PIL に頼らず標準ライブラリだけで PNG を書く。
実行: python make_screen_glass.py
"""

import math
import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / "Assets" / "TD2" / "Screen" / "screen_glass.png"
W, H = 512, 384

EDGE_ALPHA = 0.55      # 縁の最暗部の不透明度
EDGE_START = 0.78      # 超楕円距離でこの値から暗くなり始める（1 が画面の縁）
SHAPE_POWER = 7.0      # 超楕円の次数。大きいほど四角に近く、角だけが丸く落ちる
SHEEN_ALPHA = 0.055    # 映り込みの最大の不透明度
SHEEN_CENTER = (-0.62, -0.7)
SHEEN_RADIUS = (0.8, 0.34)
SHEEN_ANGLE = math.radians(-18.0)


def smoothstep(a, b, x):
    t = min(1.0, max(0.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)


def pixel(px, py):
    # 画素中心を [-1, 1] に。v は下向き（画布と同じ）。
    u = (px + 0.5) / W * 2.0 - 1.0
    v = (py + 0.5) / H * 2.0 - 1.0
    d = (abs(u) ** SHAPE_POWER + abs(v) ** SHAPE_POWER) ** (1.0 / SHAPE_POWER)
    edge = EDGE_ALPHA * smoothstep(EDGE_START, 1.0, d) ** 1.6
    # 映り込み：傾けた楕円のガウス状の広がり。
    du, dv = u - SHEEN_CENTER[0], v - SHEEN_CENTER[1]
    cu = du * math.cos(SHEEN_ANGLE) - dv * math.sin(SHEEN_ANGLE)
    cv = du * math.sin(SHEEN_ANGLE) + dv * math.cos(SHEEN_ANGLE)
    sheen = SHEEN_ALPHA * math.exp(-((cu / SHEEN_RADIUS[0]) ** 2 + (cv / SHEEN_RADIUS[1]) ** 2) * 2.0)
    sheen *= 1.0 - smoothstep(0.86, 1.0, d)  # 縁の暗がりの上には載せない
    # 白（映り込み）を黒（減光）の上に重ねた結果を、ストレートアルファの 1 画素にまとめる。
    alpha = sheen + edge * (1.0 - sheen)
    value = sheen / alpha if alpha > 1e-6 else 0.0
    return value, alpha


def dither(x, y, salt):
    """画素ごとに決まった [0, 1) の揺らぎ。毎回同じ画像になるよう乱数は使わない。"""
    h = (x * 374761393 + y * 668265263 + salt * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65536.0


def write_png(path, rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def main():
    rows = []
    for py in range(H):
        row = []
        for px in range(W):
            value, alpha = pixel(px, py)
            c = min(255, int(255 * value + dither(px, py, 1)))
            a = min(255, int(255 * alpha + dither(px, py, 2)))
            row += [c, c, c, a]
        rows.append(row)
    write_png(OUT, rows)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
