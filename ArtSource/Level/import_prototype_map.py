# -*- coding: utf-8 -*-
"""網頁原型（留形 · ECHO FIELD）の地図 JSON を TD2 の区域（1 台の TV = 1 区域）へ変換する。

対応
- 地図块（chunks.width × height 格）ごとに 1 区域。中身（地形・機関）の無い块と、全面が実体の块は区域にしない
  （隣の区域の露出辺はルール側が壁で閉じるので、実体の块と同じ結果になる）。
- MERGES に挙げた块の縦列 / 横列は 1 台の長い画面へ合成する（竖井など、落下中に何度も切り替えないため）。
- 格：1=実体 → cells の '1'。2=单向平台 → OneWay（横に続く並びを 1 つに、高さ 1 格）。
  3/4/5/6=尖刺（上/下/左/右）→ Spikes（同じ向きの並びを 1 つに）。
- 機関：laser / plate / door / bridge / spring / checkpoint / collectible / tutorial(Decal)。
  座標・大きさは 32px = 1 格で格へ、弾力（px/秒）も格/秒へ。別の区域の目標は「区域ID/装置ID」で書く。
- 出生点は原型と同じ本体の左上（格）。入った区域を entryRegion にする。
出力: ArtSource/Level/<name>.level.json（区域ごとの regionId・寸法・世界座標・cells・devices・出生点）。
使い方: python import_prototype_map.py <地図.json> [名前]
"""

import collections
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TILE = 32
# 合成する块：("col", 列, 開始行, 終了行) の縦長、または ("row", 行, 開始列, 終了列) の横長。
MERGES = []
# 区域にしない块（列, 行）。地図 5 の c9 列は床の無い落下だけの未完成区域で、こちらのルールは露出辺を壁で
# 閉じるため底に閉じ込められる。r7 行は天井だけで、どこからも到達できない。設計が進んだらここから外す。
EXCLUDE = {(9, r) for r in range(3, 8)} | {(c, 7) for c in range(6, 9)}
SPIKE_DIRS = {3: "Up", 4: "Down", 5: "Left", 6: "Right"}


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def main():
    src = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else "map"
    d = load(src)
    W, H = d["width"], d["height"]
    cw, ch = d["chunks"]["width"], d["chunks"]["height"]
    tiles = d["tiles"]

    def tile(x, y):
        return tiles[y * W + x] if 0 <= x < W and 0 <= y < H else 1

    def chunk_of_px(x, y):
        return int(x // TILE) // cw, int(y // TILE) // ch

    # 1. 中身のある块
    used = collections.Counter()
    for i, v in enumerate(tiles):
        if v:
            used[((i % W) // cw, (i // W) // ch)] += 1
    entities_by_chunk = collections.defaultdict(list)
    for e in d["entities"]:
        entities_by_chunk[chunk_of_px(e["x"], e["y"])].append(e)
    chunks = set(used) | set(entities_by_chunk)
    solid_only = {c for c in chunks if not entities_by_chunk[c] and all(
        tile(c[0] * cw + x, c[1] * ch + y) == 1 for y in range(ch) for x in range(cw))}
    chunks -= solid_only
    chunks -= EXCLUDE

    # 2. 区域（合成を含む）
    regions = {}
    owner = {}
    for merge in MERGES:
        if merge[0] == "col":
            _, cx, y0, y1 = merge
            members = [(cx, cy) for cy in range(y0, y1 + 1) if (cx, cy) in chunks]
            rid = f"{name}.c{cx}_r{y0}-{y1}"
            regions[rid] = {"x": cx * cw, "y": y0 * ch, "cols": cw, "rows": (y1 - y0 + 1) * ch, "chunks": members}
        else:
            _, cy, x0, x1 = merge
            members = [(cx, cy) for cx in range(x0, x1 + 1) if (cx, cy) in chunks]
            rid = f"{name}.c{x0}-{x1}_r{cy}"
            regions[rid] = {"x": x0 * cw, "y": cy * ch, "cols": (x1 - x0 + 1) * cw, "rows": ch, "chunks": members}
        for c in members:
            owner[c] = rid
    for c in sorted(chunks):
        if c in owner:
            continue
        rid = f"{name}.c{c[0]}_r{c[1]}"
        regions[rid] = {"x": c[0] * cw, "y": c[1] * ch, "cols": cw, "rows": ch, "chunks": [c]}
        owner[c] = rid

    def region_at(tx, ty):
        return owner.get((tx // cw, ty // ch))

    # 3. 地形と、格から作る機関（单向平台・尖刺）
    out = []
    devices = collections.defaultdict(list)
    for rid, r in regions.items():
        cells = []
        for y in range(r["rows"]):
            for x in range(r["cols"]):
                cells.append("1" if tile(r["x"] + x, r["y"] + y) == 1 else "0")
        r["cells"] = "".join(cells)
        # 单向平台：横の並び
        for y in range(r["rows"]):
            x = 0
            while x < r["cols"]:
                if tile(r["x"] + x, r["y"] + y) == 2:
                    start = x
                    while x < r["cols"] and tile(r["x"] + x, r["y"] + y) == 2:
                        x += 1
                    devices[rid].append(f"ow_{start}_{y} OneWay {start} {y} {x - start} 1")
                else:
                    x += 1
        # 尖刺：上下向きは横に、左右向きは縦に並びをまとめる
        seen = set()
        for y in range(r["rows"]):
            for x in range(r["cols"]):
                v = tile(r["x"] + x, r["y"] + y)
                if v not in SPIKE_DIRS or (x, y) in seen:
                    continue
                horizontal = v in (3, 4)
                length = 0
                while True:
                    px, py = (x + length, y) if horizontal else (x, y + length)
                    if px >= r["cols"] or py >= r["rows"] or tile(r["x"] + px, r["y"] + py) != v:
                        break
                    seen.add((px, py))
                    length += 1
                w, h = (length, 1) if horizontal else (1, length)
                devices[rid].append(f"sp_{x}_{y} Spikes {x} {y} {w} {h} dir={SPIKE_DIRS[v]}")

    # 4. 機関
    id_region = {}
    for e in d["entities"]:
        tx, ty = int(e["x"] // TILE), int(e["y"] // TILE)
        rid = region_at(tx, ty)
        if rid:
            id_region[e["id"]] = rid
    skipped = collections.Counter()
    for e in d["entities"]:
        rid = id_region.get(e["id"])
        if not rid:
            skipped[e["type"]] += 1
            continue
        r = regions[rid]
        x, y = int(e["x"] // TILE) - r["x"], int(e["y"] // TILE) - r["y"]
        w, h = max(1, round(e["w"] / TILE)), max(1, round(e["h"] / TILE))
        t = e["type"]
        head = f"{e['id']} "
        if t == "laser":
            line = head + f"Laser {x} {y} {w} {h} dir={e.get('direction', 'right').capitalize()} range={e.get('range', 384) / TILE:g}"
            if e.get("inverted"):
                line += " invert=1"
        elif t == "plate":
            targets = [tid if id_region.get(tid) == rid else f"{id_region[tid]}/{tid}" for tid in e.get("targets", []) if tid in id_region]
            line = head + f"Plate {x} {y} {w} {h}" + (f" targets={','.join(targets)}" if targets else "")
        elif t in ("door", "bridge"):
            line = head + f"{t.capitalize()} {x} {y} {w} {h}" + (" invert=1" if e.get("inverted") else "")
        elif t == "spring":
            line = head + (f"Spring {x} {y} {w} {h} power={e.get('power', 720) / TILE:g} delay={e.get('delay', 0.85):g}"
                           f" cooldown={e.get('cooldownTime', 3):g}")
        elif t == "checkpoint":
            line = head + f"Checkpoint {x} {y} {w} {h}"
        elif t == "collectible":
            line = head + f"Collectible {x} {y} {w} {h}"
        elif t == "tutorial":
            line = head + f"Decal {x} {y} {w} {h} variant={e.get('variant', 'ad')} opacity={e.get('opacity', 0.8):g}"
        else:
            skipped[t] += 1
            continue
        devices[rid].append(line)

    # 5. 出生点
    sx, sy = d["spawn"]["x"] / TILE, d["spawn"]["y"] / TILE
    entry = region_at(int(sx), int(sy))
    for rid, r in regions.items():
        item = {"regionId": rid, "cols": r["cols"], "rows": r["rows"], "worldX": r["x"], "worldY": r["y"],
                "chunks": r["chunks"], "cells": r["cells"], "devices": "\n".join(devices[rid]) + ("\n" if devices[rid] else ""),
                "entry": rid == entry}
        if rid == entry:
            item["spawnX"], item["spawnY"] = round(sx - r["x"], 4), round(sy - r["y"], 4)
        out.append(item)
    out.sort(key=lambda it: (it["worldY"], it["worldX"]))
    level = {"source": str(src), "name": d.get("name", name), "chunkSize": [cw, ch], "regions": out,
             "skipped": dict(skipped), "solidChunks": sorted(solid_only),
             "excludedChunks": sorted(EXCLUDE)}
    target = ROOT / f"{name}.level.json"
    target.write_text(json.dumps(level, ensure_ascii=False, indent=1), encoding="utf-8")
    print("regions", len(out), "entry", entry, "skipped", dict(skipped), "solid chunks", sorted(solid_only))
    print("wrote", target)


if __name__ == "__main__":
    main()
