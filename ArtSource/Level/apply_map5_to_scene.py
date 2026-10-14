# -*- coding: utf-8 -*-
"""地図 5 を主場景 TD2_Rules01 の 3D 布景へ反映する（編集器を閉じた状態で実行する）。

構成の考え方：秩序と破格
- 秩序：塔（c6〜c8 の r3〜r6）は打ち放しの角柱（モノリス）の三面に、行をそろえて掛ける。正面 = c6 列、
  右面 = c7 列、背面 = c8 列。右へ進むと角を回り込み、下へ進むと面を降りる。塔の一周（c6→c7→c8→c7→c6）が
  角柱を回る動きとそのまま一致する。
- 破格：起点のまわりの電視は宙に浮かび、少しずつ向きと傾きと大きさを変えて連なる。上の行（空の部屋）は
  天窓の月明かりへ昇る階段に、下の行（回廊）は角柱へ向かって曲がりながら近づく。角柱の上にも一台ずつ浮かべ、
  角柱の面の中にも一台だけ規則を外した電視を置く。
- 起点（c3_r3）は開発者の愛機。屋根から長い鎖で下げた暖色の籠灯だけが寄り添う。何度戻っても同じ温度で迎える。
- 隣り合う区域は、遊びの向き（左右上下）と 3D の見え方がいつも一致する：B が右隣なら、A の機位から見て
  B は右にある（check_layout が確かめる）。電視の向きが変わる所はカメラが回り込む。
- 区域ごとの游戏機位は画面の正面、画面の高さが画の約 47%。電視の大きさを変えても画面上の大きさは同じなので、
  縮尺の違いは周りの世界の大きさとして感じられる。

再実行すると、この脚本が作った物体（GEN_FOLDERS の中）と演出だけを作り直す。旧区域の TD2.Region と旧演出の部品は
外す（模型は工場の飾りとして残す）。場景は実行前に Library/SceneBackups へ退避する。
実行: python apply_map5_to_scene.py [--dry | --cameras]
  --cameras は区域の電視の機位と切り替え演出だけを作り直す（STACKED_PAIRS の下の電視の位置、演出資産、
  TD2.RegionSequence の速さ）。ほかの物体・区域の地図・光は触らない。
"""

import argparse
import hashlib
import json
import math
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent
SCENE_PATH = PROJECT / "Assets" / "Scenes" / "TD2_Rules01.scene.json"
LEVEL = ROOT / "map5.level.json"
SEQ_DIR = PROJECT / "Assets" / "Sequences" / "Map5"
SEQ_META = PROJECT / "ProjectSettings" / "AssetMeta" / "Assets" / "Sequences" / "Map5"
BACKUPS = PROJECT / "Library" / "SceneBackups"
FACTORY_GUIDS = json.loads((PROJECT / "ArtSource/FactorySet01/Temp/installed_guids.json").read_text(encoding="utf-8"))
LEVEL_SET = json.loads((PROJECT / "ArtSource/LevelSet01/level_set01_manifest.json").read_text(encoding="utf-8"))
PARTS = {p["id"]: p for p in LEVEL_SET["parts"]}


def level_guid(name):
    return hashlib.md5(f"LevelSet01/{name}".encode("utf-8")).hexdigest()


CAMERA_ID = 3
GEN_FOLDERS = ["1 玩法/区域电视/地图5", "1 玩法/镜头与演出/地图5切换", "2 工厂布景/地图5布景", "3 灯光/地图5"]
F_TV, F_SEQ, F_SET, F_LIGHT = GEN_FOLDERS
OLD_GEN_FOLDERS = ["2 工厂布景/地图5支架"]
# 旧区域（作業台の B・TV2・A と試作の F01_TV_Wide_42）：TD2.Region を外して飾りに戻す。机の電視（菜单）は残す。
OLD_REGIONS = [4, 25, 26, 5358]
OLD_SEQUENCE_OBJECTS = [255]
OLD_REGION_FOLDERS = {4: "2 工厂布景/工作台", 25: "2 工厂布景/工作台", 26: "2 工厂布景/工作台", 5358: "2 工厂布景/货架电视墙"}
# 天車に吊ってあった飾りの電視と鎖。
CRANE_DECOR = [5385, 5386, 5387, 5388, 5389, 5390, 5391, 5392, 5393]
# 天車（桁と吊り具）は手前へ走らせ、浮かぶ電視の上を空ける。机から見ると画の上を横切る前景になる。
CRANE_MOVES = {5383: (None, None, -3.5), 5384: (-9.0, None, -3.5)}
# 塔の右面の機位と重なる吊り灯（8, -16）は右へ。
LAMP_MOVES = {5405: (11.0, None, None), 5444: (11.0, None, None)}
# 影を落とす局所光（起点の暖色の主光は生成時に影付き）：起点の背後の吊り灯、奥の吊り灯、机の卓上灯。
SHADOW_LIGHTS = {5442, 5445, 5449}
# 以前に退けた飾り（塔の場所にあった電視の山、床置きの電視）。元の場所にある時だけ動かす。
MOVES = {range(5265, 5276): (-15.1, 0.0, -5.0), range(5377, 5380): (-3.0, 0.0, 2.0)}

# ---------------------------------------------------------------- 角柱（塔の芯）
MONO = (4.5, 0.0, -16.0)          # 底面中心
MONO_HALF = 1.3
TOWER_ROW = {3: 5.5, 4: 3.75, 5: 2.0, 6: 0.25}   # 角柱に掛ける電視の下端（行ごとにそろえる）
TOWER_SCALE = 1.3
# 縦に続けて落ちる二部屋（上の区域 → 下の区域）。二台を寄せ（角柱の行の規則をわざと外す）、どちらの機位からも
# 相手の画面が画の端に覗くようにする。切り替えは画の半分ほど下へ振るだけになり、落ちる主角を目で追い続けられる。
STACKED_PAIRS = {"c6_r3": "c6_r4"}
STACK_GAP = 1.2          # 寄せた二台の底面の高さの差（角柱の行の間隔は 1.75）
STACK_LOOK = 0.30        # 注視点を相手の画面の中心へ寄せる割合
STACK_LEAN = 7.0         # 二台とも少し見下ろす（縦穴を落ちる向き）
STACK_PULL = 1.12        # 少し引いて相手の画面を画に入れる
# 面ごとの向き（電視の正面）と、面の外向きの単位ベクトル。
FACES = {6: (0.0, (0, 0, 1)), 7: (90.0, (1, 0, 0)), 8: (180.0, (0, 0, -1))}


def on_face(column, row, slide=0.0, out=0.0):
    """角柱の面に掛けた電視の原点（底面中心）。slide は面に沿った横ずれ（電視の右向き）、out は面からの浮き。"""
    yaw, n = FACES[column]
    depth = 0.42 * TOWER_SCALE + out
    right = (math.cos(math.radians(yaw)), 0, -math.sin(math.radians(yaw)))
    x = MONO[0] + n[0] * (MONO_HALF + depth) + right[0] * slide
    z = MONO[2] + n[2] * (MONO_HALF + depth) + right[2] * slide
    return (x, TOWER_ROW[row], z), yaw


# 区域ごとの電視：種類・位置（底面中心）・向き yaw・傾き tilt（正で画面が下を向く）・大きさ。
def layout():
    L = {}

    def put(key, kind, pos, yaw, tilt=0.0, scale=1.0, mount="float"):
        L[key] = {"kind": kind, "pos": pos, "yaw": yaw, "tilt": tilt, "scale": scale, "mount": mount}

    # 起点と、それを囲む浮かぶ電視（破格）。
    put("c3_r3", "SM_L_TV_Hub", (1.0, 4.6, -7.0), 0.0)
    put("c2_r3", "SM_L_TV_Wide", (-1.85, 4.15, -6.15), 24.0, -3.0, 0.95)
    put("c3_r2", "SM_L_TV_Wide", (1.25, 6.75, -6.65), -4.0, 13.0, 0.92)
    put("c2_r2", "SM_L_TV_Steel", (-1.6, 7.35, -7.6), 20.0, 9.0, 0.86)
    # 空の部屋の階段：右へ行くほど高く、奥へ、月明かりの天窓の方へ。
    put("c4_r2", "SM_L_TV_Steel", (3.75, 7.75, -8.6), -16.0, 7.0, 0.86)
    put("c5_r2", "SM_L_TV_Wide", (6.55, 8.35, -10.0), -34.0, 5.0, 0.8)
    # 回廊：右へ進むほど電視が回り、大きくなり、角柱の正面へ回り込む。
    put("c4_r3", "SM_L_TV_Steel", (3.55, 5.0, -8.15), 32.0, 0.0, 1.06)
    put("c5_r3", "SM_L_TV_Wide", (3.05, 5.3, -11.1), 70.0, -2.0, 1.15)
    # 角柱（秩序）。正面 c6、右面 c7、背面 c8。一台だけ（c7_r5）規則を外して斜めに飛び出させる。
    for column in (6, 7, 8):
        for row in (3, 4, 5, 6):
            key = f"c{column}_r{row}"
            if key == "c8_r6":
                continue
            kind = "SM_L_TV_Console" if row == 6 else ("SM_L_TV_Steel" if (column + row) % 2 else "SM_L_TV_Wide")
            slide = {6: -0.12, 7: 0.1, 8: 0.0}[column] + (0.08 if row % 2 else -0.08)
            pos, yaw = on_face(column, row, slide)
            tilt = 0.0
            if key == "c7_r5":
                pos, yaw = on_face(column, row, 0.35, 0.55)
                yaw += 17.0
                tilt = -6.0
            put(key, kind, pos, yaw, tilt, TOWER_SCALE, "face")
    # 縦に続く二部屋は下の電視を上の電視へ寄せる。
    for upper, lower in STACKED_PAIRS.items():
        x, _, z = L[lower]["pos"]
        L[lower]["pos"] = (x, L[upper]["pos"][1] - STACK_GAP, z)
    # 角柱の上に浮かぶ二台。
    put("c6_r2", "SM_L_TV_Steel", (4.4, 8.45, -14.25), 9.0, 6.0, 1.05)
    put("c7_r2", "SM_L_TV_Wide", (6.25, 8.8, -15.6), 74.0, 4.0, 1.0)
    return L


NAMES = {"c3_r3": "起点"}
HUB = "c3_r3"
FOV = 26.0
FILL_H = 0.40            # 画面の高さが画に占める割合（16:9 で幅の約 39%）。周りの空間も画に入れる
HUB_FILL_H = 0.42
GIRDER_Y = 9.2
ROOF_Y = 10.9


# ---------------------------------------------------------------- 幾何
def rot(v, yaw, tilt=0.0):
    """rotationEulerDeg = [tilt, yaw, 0]（X を先に、次に Y）で局所ベクトルを回す。"""
    a, b = math.radians(tilt), math.radians(yaw)
    x, y, z = v
    y, z = y * math.cos(a) - z * math.sin(a), y * math.sin(a) + z * math.cos(a)
    x, z = x * math.cos(b) + z * math.sin(b), -x * math.sin(b) + z * math.cos(b)
    return (x, y, z)


def add(a, b, k=1.0):
    return tuple(a[i] + b[i] * k for i in range(3))


def sub(a, b):
    return tuple(a[i] - b[i] for i in range(3))


def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def aim(source, target):
    """光・カメラの向き（[下向き角, 方位, 0]）。局所 +Z を source→target へ向ける。"""
    d = sub(target, source)
    n = math.sqrt(dot(d, d))
    d = [v / n for v in d]
    return (math.degrees(math.asin(-d[1])), math.degrees(math.atan2(d[0], d[2])), 0.0)


def screen_center(tv):
    s = PARTS[tv["kind"]]["screen"]
    k = tv["scale"]
    local = (s["center_x_m"] * k, (s["bottom_m"] + s["height_m"] / 2) * k, s["front_z_m"] * k)
    return add(tv["pos"], rot(local, tv["yaw"], tv["tilt"]))


def camera_pose(key, tv, tvs):
    """画面の正面の游戏機位。高い電視は少し見下ろし、低い電視は見上げる（床までの高さを感じさせる）。
    縦に続く二部屋は、注視点を相手の画面の方へ寄せ、少し引いて見下ろす（相手の画面が画の端に覗く）。"""
    s = PARTS[tv["kind"]]["screen"]
    center = screen_center(tv)
    fill = HUB_FILL_H if key == HUB else FILL_H
    dist = s["height_m"] * tv["scale"] / (fill * 2 * math.tan(math.radians(FOV / 2)))
    lean = max(-7.0, min(7.0, (center[1] - 4.8) * 1.6))      # 起点の高さを基準に、上は見下ろし下は見上げる
    partner = STACKED_PAIRS.get(key) or next((u for u, l in STACKED_PAIRS.items() if l == key), None)
    if partner:
        center = add(center, sub(screen_center(tvs[partner]), center), STACK_LOOK)
        dist *= STACK_PULL
        lean = STACK_LEAN
    pitch = -tv["tilt"] + lean
    fwd = rot((0, 0, 1), tv["yaw"], tv["tilt"])
    # 画面の法線から lean だけ上下へずらした位置から画面の中心を見る。
    off = rot((0, 0, 1), tv["yaw"], tv["tilt"] - lean)
    eye = add(center, off, dist)
    return {"eye": eye, "rot": (pitch, tv["yaw"] + 180.0, 0.0), "fwd": fwd, "dist": dist, "center": center}


def frame_axes(pose):
    """機位から見た右・上・前（画面の奥）。"""
    p, y = pose["rot"][0], pose["rot"][1]
    fwd = rot((0, 0, 1), y, p)
    up = rot((0, 1, 0), y, p)
    # 局所 +X（up×fwd）は画面の左なので、画面の右は fwd×up。
    right = (fwd[1] * up[2] - fwd[2] * up[1], fwd[2] * up[0] - fwd[0] * up[2], fwd[0] * up[1] - fwd[1] * up[0])
    return right, up, fwd


def chunk_of(region_id):
    return region_id.split(".")[-1]


def parse_chunk(key):
    c, r = key.split("_")
    return int(c[1:]), int(r[1:])


# ---------------------------------------------------------------- 場景の部品
class Scene:
    def __init__(self, data):
        self.data = data
        self.objects = data["objects"]
        self.next_id = max(o["id"] for o in self.objects) + 1

    def find(self, oid):
        return next((o for o in self.objects if o["id"] == oid), None)

    def new(self, name, folder, position, rotation=(0, 0, 0), scale=(1, 1, 1), components=()):
        oid = self.next_id
        self.next_id += 1
        comps = [{"id": i, "properties": props, "type": ctype} for i, (ctype, props) in enumerate(components, 1)]
        obj = {"components": comps, "editorFolder": folder, "editorLocked": False, "editorVisible": True,
               "enabled": True, "id": oid, "name": name, "parent": None, "sourcePrefabId": "",
               "transform": {"position": [round(v, 4) for v in position],
                             "rotationEulerDeg": [round(v, 3) for v in rotation],
                             "scale": [round(v, 4) for v in scale]}}
        self.objects.append(obj)
        return obj


def model(guid, shadow=True):
    return ("ModelComponent", {"assetId": guid, "castShadow": shadow, "receiveShadow": True, "visible": True})


def light(kind, color, intensity, rng, shadows=False, cone=(25.0, 35.0), size=(0.8, 0.45), unit="Lumens"):
    return ("LightComponent", {"angularFalloff": 1.0, "castsShadows": shadows, "color": list(color), "cookie": "",
                               "enabled": True, "height": size[1], "iesProfile": "", "innerConeAngleDegrees": cone[0],
                               "intensity": intensity, "intensityUnit": unit, "length": size[0],
                               "outerConeAngleDegrees": cone[1], "priority": 0, "range": rng,
                               "sceneRelativeIntensity": 1.0, "sourceRadius": 0.0, "type": kind})


def ref(oid):
    return {"$hikariRef": "SceneObject", "id": oid}


# ---------------------------------------------------------------- 隣接（共有辺に両側とも空いた格がある対）と向き
def adjacency(regions):
    by_id = {r["regionId"]: r for r in regions}

    def cell(r, x, y):
        return r["cells"][y * r["cols"] + x]

    pairs = []
    ids = sorted(by_id)
    for i, a_id in enumerate(ids):
        a = by_id[a_id]
        for b_id in ids[i + 1:]:
            b = by_id[b_id]
            side = None
            if a["worldX"] + a["cols"] == b["worldX"]:
                side = "right"
            elif b["worldX"] + b["cols"] == a["worldX"]:
                side = "left"
            elif a["worldY"] + a["rows"] == b["worldY"]:
                side = "down"
            elif b["worldY"] + b["rows"] == a["worldY"]:
                side = "up"
            if side is None:
                continue
            open_count = 0
            if side in ("right", "left"):
                left, right = (a, b) if side == "right" else (b, a)
                y0, y1 = max(a["worldY"], b["worldY"]), min(a["worldY"] + a["rows"], b["worldY"] + b["rows"])
                for wy in range(y0, y1):
                    open_count += cell(left, left["cols"] - 1, wy - left["worldY"]) == "0" and cell(right, 0, wy - right["worldY"]) == "0"
            else:
                top, bottom = (a, b) if side == "down" else (b, a)
                x0, x1 = max(a["worldX"], b["worldX"]), min(a["worldX"] + a["cols"], b["worldX"] + b["cols"])
                for wx in range(x0, x1):
                    open_count += cell(top, wx - top["worldX"], top["rows"] - 1) == "0" and cell(bottom, wx - bottom["worldX"], 0) == "0"
            if open_count:
                pairs.append((a_id, b_id, side))
    return pairs


def check_layout(pairs, poses, tvs):
    """隣へ移る時、行き先が今の機位から見て遊びと同じ向きにあるか（両方向）。外れたら名前を挙げる。"""
    problems = []
    opposite = {"right": "left", "left": "right", "up": "down", "down": "up"}
    for a_id, b_id, side in pairs:
        for src, dst, direction in ((a_id, b_id, side), (b_id, a_id, opposite[side])):
            # 向きは電視の画面どうしで見る（縦に続く二部屋は注視点を相手へ寄せているので、注視点では測らない）。
            right, up, _ = frame_axes(poses[src])
            d = sub(screen_center(tvs[chunk_of(dst)]), screen_center(tvs[chunk_of(src)]))
            x, y = dot(d, right), dot(d, up)
            want = {"right": x, "left": -x, "up": y, "down": -y}[direction]
            other = abs(y) if direction in ("right", "left") else abs(x)
            if want < 0.8 or want < other * 0.9:
                problems.append(f"{chunk_of(src)}→{chunk_of(dst)} {direction}: right={x:.2f} up={y:.2f}")
    # 機位の前（画面まで）に他の電視が入っていないか。
    for key, pose in poses.items():
        eye, center = pose["eye"], pose["center"]
        for other, tv in tvs.items():
            if other == chunk_of(key):
                continue
            c = screen_center(tv)
            seg = sub(center, eye)
            t = max(0.0, min(1.0, dot(sub(c, eye), seg) / dot(seg, seg)))
            closest = add(eye, seg, t)
            if math.dist(closest, c) < 0.75 * tv["scale"] and t < 0.97:
                problems.append(f"{chunk_of(key)} 机位被 {other} 挡住 (t={t:.2f})")
    return problems


# ---------------------------------------------------------------- 本体
SEQ_KEY_TIMES = (0.0, 0.25, 0.5, 0.75, 1.0)   # 切り替えの鍵の位置（長さに対する割合）
SEQ_PULL = 0.10          # 途中で引く量の下限（機位の移動距離に対する割合）
SEQ_FIT_SHARE = 0.5      # 中間で二台の画面がそれぞれこの割合以上画に入るまで引く
ASPECT = 16.0 / 9.0


def smooth(t):
    return t * t * (3.0 - 2.0 * t)


def screen_share(eye, rotation, tv):
    """機位から見て、電視の画面（5×5 の標本点）のうち画に入っている割合。"""
    s = PARTS[tv["kind"]]["screen"]
    k = tv["scale"]
    p, y = rotation[0], rotation[1]
    fwd, up = rot((0, 0, 1), y, p), rot((0, 1, 0), y, p)
    right = (fwd[1] * up[2] - fwd[2] * up[1], fwd[2] * up[0] - fwd[0] * up[2], fwd[0] * up[1] - fwd[1] * up[0])
    tan_v = math.tan(math.radians(FOV / 2))
    inside = 0
    for i in range(5):
        for j in range(5):
            local = ((s["center_x_m"] + (i / 4 - 0.5) * s["width_m"]) * k, (s["bottom_m"] + j / 4 * s["height_m"]) * k, s["front_z_m"] * k)
            d = sub(add(tv["pos"], rot(local, tv["yaw"], tv["tilt"])), eye)
            z = dot(d, fwd)
            inside += z > 0 and abs(dot(d, right) / z) <= tan_v * ASPECT and abs(dot(d, up) / z) <= tan_v
    return inside / 25


def write_sequence(a_id, b_id, poses, tvs):
    """隣り合う二区域の切り替え演出（資産とメタ）を書き、(表示名, GUID, 倍速) を返す。GUID は区域の組で決まる。"""
    pa, pb = poses[a_id], poses[b_id]
    delta = math.dist(pa["eye"], pb["eye"])
    turn = abs(((pb["rot"][1] - pa["rot"][1]) + 180) % 360 - 180)
    # 実際の長さ（秒）。引擎の演出は最短 1 秒なので、1 秒で作って倍速（1 / 実際の長さ）で流す。
    seconds = min(0.55, max(0.32, 0.27 + 0.035 * delta + 0.0012 * turn))
    speed = round(1.0 / seconds, 3)
    duration = 1.0
    # 切り替え中も操作は止めず、反対向きは同じ演出の逆再生なので、道筋は前後対称にする。
    # 中間で少し引き、二台の画面の向かい合う半分（越えた主角がいる側）が同時に画に入るようにする。
    # 引きは必要な最小量：どの瞬間もどちらかの画面が半分以上見え、どちらへ動いたかも分かる。
    tv_a, tv_b = tvs[chunk_of(a_id)], tvs[chunk_of(b_id)]
    avg_fwd = tuple((pa["fwd"][i] + pb["fwd"][i]) / 2 for i in range(3))

    def pose_at(t, pull):
        u = smooth(t)
        eye = add(add(pa["eye"], sub(pb["eye"], pa["eye"]), u), avg_fwd, pull * math.sin(math.pi * t))
        return eye, aim(eye, add(pa["center"], sub(pb["center"], pa["center"]), u))

    pull = SEQ_PULL * delta
    while pull < 8.0 and min(screen_share(*pose_at(0.5, pull), tv) for tv in (tv_a, tv_b)) < SEQ_FIT_SHARE:
        pull += 0.05
    keys = []
    for t in SEQ_KEY_TIMES:
        if t in (0.0, 1.0):
            pose = pa if t == 0.0 else pb
            eye, r = pose["eye"], pose["rot"]
        else:
            eye, r = pose_at(t, pull)
        # 方位は直前の鍵から最短の向きへ回す（359° → 1° を一周させない）。
        yaw = r[1] if not keys else r[1] + 360.0 * round((keys[-1][2][1] - r[1]) / 360.0)
        keys.append((round(t * duration, 4), eye, (r[0], yaw, 0.0)))
    ka, kb = chunk_of(a_id), chunk_of(b_id)
    name = f"{ka} ↔ {kb}"
    guid = hashlib.md5(f"TD2/Map5Sequence/{a_id}|{b_id}".encode("utf-8")).hexdigest()
    doc = {"assetType": "Sequence", "displayName": name, "formatVersion": 4, "guid": guid, "sequence": {
        "bindings": [{"id": 1, "name": "游戏摄像机", "required": True, "slotName": "Camera", "targetKind": "slot"}],
        "durationSeconds": duration, "id": 1, "name": name, "screenEffects": [],
        "tracks": [
            {"clips": [{"camera": {"owned": False}, "cameraBindingId": 1, "durationSeconds": duration, "effects": [],
                        "id": 1, "startTimeSeconds": 0, "transition": {"durationSeconds": 0, "mode": "cut"}}],
             "enabled": True, "id": 1, "type": "cameraCut"},
            {"channels": [{"cameraBindingId": 1, "keyframes": [
                {"id": i + 1, "interpolation": "smooth", "position": [round(v, 4) for v in pos],
                 "rotationEulerDeg": [round(v, 3) for v in r], "timeSeconds": t} for i, (t, pos, r) in enumerate(keys)]}],
             "enabled": True, "id": 2, "type": "cameraTransform"},
            {"channels": [{"cameraBindingId": 1, "keyframes": [
                {"farClip": 500, "id": 1, "interpolation": "smooth", "nearClip": 0.05, "timeSeconds": 0.0, "verticalFovDegrees": FOV},
                {"farClip": 500, "id": 2, "interpolation": "smooth", "nearClip": 0.05, "timeSeconds": duration, "verticalFovDegrees": FOV}]}],
             "enabled": True, "id": 3, "type": "cameraLens"}]}}
    fname = f"TD2_Map5_{ka}_{kb}.hsequence"
    (SEQ_DIR / fname).write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    meta = {"displayName": name, "guid": guid, "importSettings": {}, "importerId": "SequenceAssetImporter",
            "importerVersion": 3, "metaVersion": 1, "sourcePath": f"Assets/Sequences/Map5/{fname}", "type": "Sequence"}
    (SEQ_META / f"{fname}.hikari.asset.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return name, guid, speed


def update_cameras(tvs, poses, pairs):
    """機位・切り替え演出・寄せた電視の位置だけを場景へ反映する（ほかの物体は触らない）。"""
    if "HIKARI_Engine.exe" in subprocess.run(["tasklist"], capture_output=True, text=True).stdout:
        raise SystemExit("编辑器正在运行：不写入场景（请关闭编辑器后再执行）")
    raw = SCENE_PATH.read_bytes()
    data = json.loads(raw.decode("utf-8-sig"))
    moved = set(STACKED_PAIRS.values())
    for o in data["objects"]:
        region = next((c["properties"] for c in o["components"] if c["type"] == "TD2.Region"), None)
        key = chunk_of(region["regionId"]) if region else None
        if key not in tvs:
            continue
        tv = tvs[key]
        want = [round(v, 4) for v in tv["pos"]]
        if key in moved:
            o["transform"]["position"] = want
            print("moved", key, want)
        elif max(abs(o["transform"]["position"][i] - want[i]) for i in range(3)) > 1e-3:
            print("differs from layout (left as is)", key, o["transform"]["position"], want)
    speeds = {}
    for a_id, b_id, side in pairs:
        name, guid, speed = write_sequence(a_id, b_id, poses, tvs)
        speeds[frozenset((a_id, b_id))] = (guid, speed)
    for o in data["objects"]:
        for c in o["components"]:
            if c["type"] == "TD2.RegionSequence":
                found = speeds.get(frozenset((c["properties"]["regionA"], c["properties"]["regionB"])))
                if found:
                    c["properties"]["sequenceAssetGuid"], c["properties"]["speed"] = found
    BACKUPS.mkdir(parents=True, exist_ok=True)
    backup = BACKUPS / f"TD2_Rules01.before_cameras_{time.strftime('%m%d_%H%M%S')}.scene.json"
    backup.write_bytes(raw)
    # 編集器の保存形式（キーの辞書順・CRLF）に合わせ、差分を物体の変更だけにする。
    text = json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    SCENE_PATH.write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
    (ROOT / "map5_camera_poses.json").write_text(json.dumps({chunk_of(k): v for k, v in poses.items()}, ensure_ascii=False, indent=1), encoding="utf-8")
    print("backup", backup)
    print("wrote", SCENE_PATH, "sequences", len(speeds))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--cameras", action="store_true")
    a = ap.parse_args()

    level = json.loads(LEVEL.read_text(encoding="utf-8"))
    regions = level["regions"]
    tvs = layout()
    by_key = {chunk_of(r["regionId"]): r for r in regions}
    assert set(tvs) == set(by_key), set(tvs) ^ set(by_key)
    poses = {r["regionId"]: camera_pose(chunk_of(r["regionId"]), tvs[chunk_of(r["regionId"])], tvs) for r in regions}
    pairs = adjacency(regions)
    problems = check_layout(pairs, poses, tvs)
    for p in problems:
        print("LAYOUT", p)
    print("pairs", len(pairs), "layout problems", len(problems))
    if a.dry:
        return
    if a.cameras:
        update_cameras(tvs, poses, pairs)
        return

    data = json.loads(SCENE_PATH.read_text(encoding="utf-8-sig"))
    scene = Scene(data)
    before = len(scene.objects)
    scene.objects[:] = [o for o in scene.objects if o.get("editorFolder") not in GEN_FOLDERS + OLD_GEN_FOLDERS]
    print("removed generated", before - len(scene.objects))

    # 旧区域・旧演出・天車の飾りを外し、天車と吊り灯を退ける
    for oid in OLD_REGIONS:
        o = scene.find(oid)
        if o:
            o["components"] = [c for c in o["components"] if c["type"] != "TD2.Region"]
    cam = scene.find(CAMERA_ID)
    cam["components"] = [c for c in cam["components"] if c["type"] != "TD2.RegionSequence"]
    remove = set(OLD_SEQUENCE_OBJECTS) | set(CRANE_DECOR)
    # 使わなくなった旧布景（隠していた「9 旧布景」）は場景から外す。旧区域の電視は工厂の飾りとして作業台・棚の組へ移す。
    remove |= {o["id"] for o in scene.objects if (o.get("editorFolder") or "").startswith("9 ")}
    scene.objects[:] = [o for o in scene.objects if o["id"] not in remove]
    for oid, folder in OLD_REGION_FOLDERS.items():
        if (o := scene.find(oid)) is not None:
            o["editorFolder"] = folder
    for moves in (CRANE_MOVES, LAMP_MOVES):
        for oid, target in moves.items():
            o = scene.find(oid)
            if o:
                o["transform"]["position"] = [o["transform"]["position"][i] if v is None else v for i, v in enumerate(target)]
    for ids, delta in MOVES.items():
        for oid in ids:
            o = scene.find(oid)
            if o and o["transform"]["position"][0] > 5.5:
                o["transform"]["position"] = [round(o["transform"]["position"][i] + delta[i], 4) for i in range(3)]

    # 角柱
    scene.new("塔 · 混凝土方柱", F_SET, MONO, (0, 0, 0), components=[model(level_guid("SM_L_Monolith"))])

    # 電視と区域
    placed = {}
    for key, tv in tvs.items():
        r = by_key[key]
        label = NAMES.get(key, "")
        name = f"区域 {key}" + (f" · {label}" if label else "")
        region_props = {
            "cells": r["cells"], "cols": r["cols"], "rows": r["rows"], "devices": r["devices"],
            "entryRegion": bool(r.get("entry")), "outputHeight": 448, "outputWidth": 768, "regionId": r["regionId"],
            "screenBrightness": 2.2, "screenMaterialIndex": -1, "screenModelAssetGuid": "", "screenSlot": "TD2_Screen",
            "spawnX": r.get("spawnX", 0.5), "spawnY": r.get("spawnY", 0.5), "tilePixels": 64,
            "worldPlaced": True, "worldX": r["worldX"], "worldY": r["worldY"]}
        k = tv["scale"]
        obj = scene.new(name, F_TV, tv["pos"], (tv["tilt"], tv["yaw"], 0), (k, k, k),
                        components=[model(level_guid(tv["kind"])), ("TD2.Region", region_props)])
        obj["components"][1]["properties"]["screenObject"] = ref(obj["id"])
        placed[key] = obj["id"]

    # 浮かぶ電視どうしをつなぐ細い配線（遊びでつながる対だけ）。角柱の電視は面に寄り添うので結ばない。
    cable = level_guid("SM_L_Cable_Sag")
    for a_id, b_id, side in pairs:
        ka, kb = chunk_of(a_id), chunk_of(b_id)
        ta, tb = tvs[ka], tvs[kb]
        if ta["mount"] == "face" or tb["mount"] == "face":
            continue
        # 背面の中ほど同士を結ぶ（画面の前を横切らない）。
        pa = add(ta["pos"], rot((0, 0.45 * ta["scale"], -0.22 * ta["scale"]), ta["yaw"], ta["tilt"]))
        pb = add(tb["pos"], rot((0, 0.45 * tb["scale"], -0.22 * tb["scale"]), tb["yaw"], tb["tilt"]))
        d = sub(pb, pa)
        length = math.sqrt(dot(d, d))
        yaw = math.degrees(math.asin(max(-1.0, min(1.0, -d[2] / length))))
        roll = math.degrees(math.atan2(d[1], d[0]))
        scene.new(f"配线 {ka}-{kb}", F_SET, pa, (0, yaw, roll), (length, 1.0, 1.0), [model(cable, False)])

    # 起点の籠灯：屋根の梁から長い鎖で下げる（浮かぶ電視の中で唯一、重力に従う物）。
    hub = tvs[HUB]
    hx, hy, hz = hub["pos"]
    lamp = (hx - 0.62, hy + 0.98, hz + 0.55)
    chain = FACTORY_GUIDS["SM_F_Chain_1m"]
    scene.new("起点 · 籠灯", F_SET, lamp, (0, 0, 0), components=[model(level_guid("SM_L_CageLamp"), False)])
    scene.new("起点 · 籠灯の鎖", F_SET, (lamp[0], lamp[1] + 0.42, lamp[2]), (0, 0, 0),
              (1.0, ROOF_Y - lamp[1] - 0.42, 1.0), [model(chain, False)])
    scene.new("起点 · 籠灯の光", F_LIGHT, (lamp[0], lamp[1] + 0.17, lamp[2]), (0, 0, 0),
              components=[light("Point", (1.0, 0.68, 0.40), 320.0, 4.0, shadows=False)])
    key_pos = (hx - 1.1, hy + 2.4, hz + 1.6)
    scene.new("起点 · 暖色の主光", F_LIGHT, key_pos, aim(key_pos, (hx, hy + 0.7, hz + 0.3)),
              components=[light("Spot", (1.0, 0.72, 0.46), 2400.0, 6.0, shadows=True, cone=(16.0, 28.0))])

    # 角柱の光：真上から面をかすめる冷たい細い光（打ち放しの肌理と面の縁を出す）と、背後の縁取り。
    for name, pos, target in (("塔 · 正面の掠め光", (MONO[0], 10.6, MONO[2] + 2.2), (MONO[0], 2.0, MONO[2] + 1.35)),
                              ("塔 · 右面の掠め光", (MONO[0] + 2.2, 10.6, MONO[2]), (MONO[0] + 1.35, 2.0, MONO[2])),
                              ("塔 · 背面の掠め光", (MONO[0], 10.6, MONO[2] - 2.2), (MONO[0], 2.0, MONO[2] - 1.35))):
        scene.new(name, F_LIGHT, pos, aim(pos, target),
                  components=[light("Spot", (0.80, 0.88, 1.0), 26000.0, 14.0, shadows=False, cone=(8.0, 14.0))])

    # 性能：局所光は数と影が直接フレーム時間に効く（全局所光で約 8 ms、影付きの各光は毎フレーム影を描き直す）。
    # 画面ごとのこぼれ光・展示光は置かず、影を落とす局所光は SHADOW_LIGHTS だけに絞る（他は光だけ残す）。
    for o in scene.objects:
        for c in o["components"]:
            if (c["type"] == "LightComponent" and c["properties"]["type"] != "Directional" and o["id"] not in SHADOW_LIGHTS
                    and o.get("editorFolder") != F_LIGHT):
                c["properties"]["castsShadows"] = False
    # 主な照明は残し、Play 中に視野の外の光だけを一時的に消す印（TD2.LightCull）をすべての局所光に付ける。
    for o in scene.objects:
        lights = [c for c in o["components"] if c["type"] == "LightComponent" and c["properties"]["type"] != "Directional"]
        if lights and not any(c["type"] == "TD2.LightCull" for c in o["components"]):
            o["components"].append({"id": max(c["id"] for c in o["components"]) + 1,
                                    "properties": {"cull": True, "margin": 2.0}, "type": "TD2.LightCull"})

    # 游戏機位と切り替え演出
    hub_pose = poses[by_key[HUB]["regionId"]]
    cam["transform"]["position"] = [round(v, 4) for v in hub_pose["eye"]]
    cam["transform"]["rotationEulerDeg"] = [round(v, 3) for v in hub_pose["rot"]]
    for c in cam["components"]:
        if c["type"] == "CameraComponent":
            c["properties"]["verticalFovDegrees"] = FOV
            dof = c["properties"].setdefault("depthOfField", {})
            dof["focusDistance"] = round(hub_pose["dist"], 3)
            dof["focusRange"] = 0.9

    SEQ_DIR.mkdir(parents=True, exist_ok=True)
    SEQ_META.mkdir(parents=True, exist_ok=True)
    for old in list(SEQ_DIR.glob("*.hsequence")) + list(SEQ_META.glob("*.json")):
        old.unlink()
    for a_id, b_id, side in pairs:
        name, guid, speed = write_sequence(a_id, b_id, poses, tvs)
        scene.new(f"切换 {name}", F_SEQ, (0, 0, 0), components=[("TD2.RegionSequence", {
            "cameraObject": ref(CAMERA_ID), "regionA": a_id, "regionB": b_id, "sequenceAssetGuid": guid,
            "sequenceSlotName": "Camera", "speed": speed})])

    folders = set(f for f in data.get("editorFolders", []) if f not in OLD_GEN_FOLDERS and not f.startswith("9 "))
    for f in GEN_FOLDERS:
        parts = f.split("/")
        for i in range(1, len(parts) + 1):
            folders.add("/".join(parts[:i]))
    data["editorFolders"] = sorted(folders)
    if "HIKARI_Engine.exe" in subprocess.run(["tasklist"], capture_output=True, text=True).stdout:
        raise SystemExit("编辑器正在运行：不写入场景（请关闭编辑器后再执行）")
    BACKUPS.mkdir(parents=True, exist_ok=True)
    backup = BACKUPS / f"TD2_Rules01.before_map5_{time.strftime('%m%d_%H%M%S')}.scene.json"
    shutil.copyfile(SCENE_PATH, backup)
    SCENE_PATH.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("backup", backup)
    print("wrote", SCENE_PATH, "objects", len(scene.objects))
    (ROOT / "map5_camera_poses.json").write_text(json.dumps({chunk_of(k): v for k, v in poses.items()}, ensure_ascii=False, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
