# -*- coding: utf-8 -*-
"""電視の中のゲーム世界の部品（ScreenKit07）を作り、引擎用の glTF と資産メタを書き出す。

画風の決め方は build_screen_style.py（Blender の試作）と同じ。違いは陰影を「焼き込む」こと：
- 光は左上・手前の一方向に固定（正交・俯角 7° の舞台で部品は回らない）。面ごとに N·L を 3 段（暗・基・明）に切り、
  段の色を材質にして KHR_materials_unlit で書き出す。引擎の捕捉はこの色をそのまま写すので、工厂の灯・霧・露出に
  左右されず、後処理（像素・素描）は決まった色だけを相手にできる。「原版」はこの 3D 描画そのもの。
- 向きで光の当たり方が変わる部品（棘・激光の発射器）は向きごとに別の部品を焼く。人物は姿勢と左右の向きごとに焼く。
- 配色は ink（墨と象牙）と teal（工厂の画面の青緑）の二組。同じ形を二度焼き、runtime が配色で GUID を選ぶ。
- 背景（壁・光の輪）は輝度 0.035 未満、物体は以上になるよう色を決めている。後処理はこれで壁と物体を分ける。

座標：Blender で作り、glTF（+Y 上・+Z 手前）へ。部品の原点は「格の下辺の中央・正面」。正面が Z=0、奥へ -Z。
出力：Assets/TD2/Screen3D/Kit07/<palette>/<部品>.gltf と .bin、メタ（GUID は md5("TD2/ScreenKit07/<palette>/<部品>")）、
      ArtSource/ScreenStyle07/screen_kit07_manifest.json、Source/Runtime/TD2ScreenKitAssets.h（GUID 表、生成物）。
実行: blender -b --factory-startup --python build_screen_kit.py [-- --only 部品名,部品名]
      --only は指定した部品だけを焼き直し、既存の manifest に重ねて GUID 表を書き直す（ほかの部品の資産は触らない）。
"""

import hashlib
import json
import math
import sys
from pathlib import Path

import bmesh
import bpy
from mathutils import Matrix, Vector

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent
KIT_REL = "Assets/TD2/Screen3D/Kit07"
NINJA = PROJECT / "ArtSource" / "Characters" / "PrototypeNinja" / "ninja-game.glb"
META_TEMPLATE = PROJECT / "ProjectSettings" / "AssetMeta" / KIT_REL / "ink" / "Backdrop.gltf.hikari.asset.json"

DEPTH = 0.7
PX = 16                       # 1 格のゲーム画素数。面取りと溝の幅をこの格子に合わせる
TO_LIGHT = Vector((-0.35, -0.45, 0.82)).normalized()   # Blender 座標（-Y が手前）

PALETTES = {
    "ink": {
        "void": "111A22", "void_line": "18232D", "pool": "15212B", "pool_core": "192733",
        "ink_dark": "2C3B47", "ink": "435969", "ink_light": "5F788A",
        "ivory_shadow": "9C9E95", "ivory": "DDD5C1", "ivory_light": "F4EFE3",
        "red_shadow": "7F2A22", "red": "C34737", "red_light": "EC7A62",
        "hero_shadow": "B9781F", "hero": "F5B83D", "hero_light": "FFE07A",
        "echo_dark": "6E4BB8", "echo": "A879F0", "echo_light": "C9A2FF",
        "go_shadow": "1F8A4A", "go": "3FD17A", "go_light": "8DF2AE",
        "outline": "0B1218",
    },
    # 青緑：工厂の外が無彩色に近いので、画面の地は明度を保ったまま彩度を少し上げ、躯体はわずかに暗くする。
    # 一番高い彩度は主角（琥珀）・残影（菫）・危険（赤）だけに残す。壁は線形輝度 0.035 未満を守る。
    "teal": {
        "void": "0A2425", "void_line": "0D2F30", "pool": "0C2D2E", "pool_core": "0E3536",
        "ink_dark": "124542", "ink": "1E625B", "ink_light": "348077",
        "ivory_shadow": "9FA79C", "ivory": "DDD8C4", "ivory_light": "F4F1E4",
        "red_shadow": "7F2A22", "red": "C34737", "red_light": "EC7A62",
        "hero_shadow": "B9781F", "hero": "F5B83D", "hero_light": "FFE07A",
        "echo_dark": "6E4BB8", "echo": "A879F0", "echo_light": "C9A2FF",
        "go_shadow": "1F8A4A", "go": "3FD17A", "go_light": "8DF2AE",
        "outline": "061112",
    },
}
# 陰影の族：(暗, 基, 明, 暗→基の閾値, 基→明の閾値)。flat は向きに依らず一色（光る部分・背景）。
FAMILIES = {
    "ink": ("ink_dark", "ink", "ink_light", 0.25, 0.7),
    "ink_deep": ("outline", "ink_dark", "ink", 0.25, 0.7),
    "ivory": ("ivory_shadow", "ivory", "ivory_light", 0.25, 0.7),
    "porcelain": ("ivory_shadow", "ivory", "ivory_light", 0.18, 0.62),
    "red": ("red_shadow", "red", "red_light", 0.25, 0.7),
    # 主角は琥珀、残影は菫色。背景の墨・青緑と補色に近く、危険の赤・象牙の天板とも色相で分かれる（両配色共通）。
    "hero": ("hero_shadow", "hero", "hero_light", 0.18, 0.62),
    "echo": ("echo_dark", "echo", "echo_light", 0.18, 0.62),
    # 弾簧の「使える」の緑。素描は鮮やかな色（彩度 0.3 以上）だけを色鉛筆で残すので、状態は色で一目に読める。
    # 琥珀（主角）・赤（危険）・菫（残影）と色相で分かれ、墨と青緑の地の上でも明るい。
    "go": ("go_shadow", "go", "go_light", 0.18, 0.62),
}
FLAT = {"glow_ivory": "ivory_light", "glow_red": "red_light", "glow_go": "go_light", "dim_go": "go_shadow", "lamp_off": "ink_dark", "void": "void",
        "void_line": "void_line", "pool": "pool", "pool_core": "pool_core"}


def lin(hex_color):
    v = [int(hex_color[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    return tuple(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in v)


# ---------------------------------------------------------------- 形（すべて Blender 座標、格の単位）
class Shape:
    """箱・錐などを集め、族（ink/ivory/...）を面に記録する。焼く時に族と面の向きから色を決める。"""

    def __init__(self):
        self.bm = bmesh.new()
        self.fam = self.bm.faces.layers.string.new("family")
        # 単面（背景の板・輪）は閉じた形ではないので法線の再計算に任せず、手前（-Y）向きに固定する。
        self.fixed = self.bm.faces.layers.int.new("fixed")

    def _tag(self, faces, family):
        for f in faces:
            f[self.fam] = family.encode()

    def box(self, lo, hi, family, inset=None):
        lo, hi = Vector(lo), Vector(hi)
        made = bmesh.ops.create_cube(self.bm, size=1.0, matrix=Matrix.Translation((lo + hi) / 2) @ Matrix.Diagonal((*(hi - lo), 1)))
        faces = list({f for v in made["verts"] for f in v.link_faces})
        self.bm.normal_update()
        self._tag(faces, family)
        if inset:
            thickness, depth = inset
            front = [f for f in faces if f.normal.y < -0.9]
            out = bmesh.ops.inset_individual(self.bm, faces=front, thickness=thickness, depth=-depth)
            self._tag(out["faces"], family)
        return faces

    def poly(self, points, family, y=0.0):
        """正面向き（-Y）の平らな多角形。背景の光の輪に使う。"""
        verts = [self.bm.verts.new((x, y, z)) for x, z in points]
        f = self.bm.faces.new(verts)
        self._tag([f], family)
        f[self.fixed] = 1

    def pyramid(self, base, half, height, family, axis=(0, 0, 1), yaw=45.0):
        c = Vector(base)
        up = Vector(axis).normalized()
        rot = Vector((0, 0, 1)).rotation_difference(up).to_matrix() @ Matrix.Rotation(math.radians(yaw), 3, "Z")
        ring = [c + rot @ Vector(v) for v in ((-half, -half, 0), (half, -half, 0), (half, half, 0), (-half, half, 0))]
        vs = [self.bm.verts.new(p) for p in ring]
        apex = self.bm.verts.new(c + up * height)
        faces = [self.bm.faces.new((vs[i], vs[(i + 1) % 4], apex)) for i in range(4)]
        faces.append(self.bm.faces.new(list(reversed(vs))))
        self._tag(faces, family)

    def octahedron(self, center, radius, family):
        c = Vector(center)
        pts = [c + Vector(v) * radius for v in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1.3), (0, 0, -1.3))]
        v = [self.bm.verts.new(p) for p in pts]
        faces = []
        for a, b in ((0, 2), (2, 1), (1, 3), (3, 0)):
            faces.append(self.bm.faces.new((v[a], v[b], v[4])))
            faces.append(self.bm.faces.new((v[b], v[a], v[5])))
        self._tag(faces, family)

    def ring(self, radius, width, family, segments=24):
        outer = [self.bm.verts.new((math.cos(a) * radius, 0, math.sin(a) * radius + radius))
                 for a in (i * 2 * math.pi / segments for i in range(segments))]
        inner = [self.bm.verts.new((math.cos(a) * (radius - width), 0, math.sin(a) * (radius - width) + radius))
                 for a in (i * 2 * math.pi / segments for i in range(segments))]
        faces = [self.bm.faces.new((outer[i], outer[(i + 1) % segments], inner[(i + 1) % segments], inner[i]))
                 for i in range(segments)]
        self._tag(faces, family)
        for f in faces:
            f[self.fixed] = 1


def to_object(shape, name, bevel):
    """面取りを当ててから評価済みメッシュにし、面の族属性を保ったまま物体を作る。"""
    fixed = shape.bm.faces.layers.int.get("fixed")
    closed = [f for f in shape.bm.faces if not (fixed and f[fixed])]
    if closed:
        bmesh.ops.recalc_face_normals(shape.bm, faces=closed)
    shape.bm.normal_update()
    for f in shape.bm.faces:
        if fixed and f[fixed] and f.normal.y > 0:
            f.normal_flip()
    me = bpy.data.meshes.new(name)
    shape.bm.to_mesh(me)
    shape.bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    if bevel:
        mod = ob.modifiers.new("chamfer", "BEVEL")
        mod.width, mod.segments, mod.limit_method = bevel, 1, "ANGLE"
        mod.angle_limit = math.radians(40)
    return ob


# ---------------------------------------------------------------- 焼き込み
MATS = {}


def unlit(palette_name, color_key):
    """RGB ノードを直接出力へつなぐと glTF の書き出しは KHR_materials_unlit にする。"""
    name = f"K07_{palette_name}_{color_key}"
    if name in MATS:
        return MATS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    rgb, out = nt.nodes.new("ShaderNodeRGB"), nt.nodes.new("ShaderNodeOutputMaterial")
    rgb.outputs[0].default_value = (*lin(PALETTES[palette_name][color_key]), 1)
    nt.links.new(rgb.outputs[0], out.inputs["Surface"])
    MATS[name] = m
    return m


def bake(ob, palette_name, rotation=None, family_override=None):
    """修飾子を適用した形の各面を、族と（必要なら回した後の）向きから段の色の材質へ割り当てる。"""
    deps = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(deps), preserve_all_data_layers=True, depsgraph=deps)
    bm = bmesh.new()
    bm.from_mesh(me)
    if rotation is not None:
        bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0), matrix=rotation)
    bm.normal_update()
    layer = bm.faces.layers.string.get("family")
    keys = []
    for f in bm.faces:
        family = family_override or (f[layer].decode() if layer else "") or "ink"
        if family in FAMILIES:
            dark, base, light, t1, t2 = FAMILIES[family]
            d = f.normal.dot(TO_LIGHT)
            key = dark if d < t1 else base if d < t2 else light
        else:
            key = FLAT[family]
        if key not in keys:
            keys.append(key)
        f.material_index = keys.index(key)
    # 面取りが細い部品で潰した面だけを除く。頂点・色・有効な面は動かさず、
    # glTF の三角形化後にゼロ面積の面が残って UV 展開を壊すことを防ぐ。
    bmesh.ops.triangulate(bm, faces=list(bm.faces))
    collapsed = [f for f in bm.faces if f.calc_area() <= 5e-10]
    if collapsed:
        bmesh.ops.delete(bm, geom=collapsed, context="FACES_ONLY")
    # 材質の枠を先に作ってから面を書き戻す（後で clear すると面の材質番号まで 0 に戻る）。
    me.materials.clear()
    for k in keys:
        me.materials.append(unlit(palette_name, k))
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = False
    baked = bpy.data.objects.new(ob.name + "_baked", me)
    bpy.context.scene.collection.objects.link(baked)
    return baked


# ---------------------------------------------------------------- 部品の定義（関数は Shape と面取り幅を返す）
B = 1 / PX            # 面取り 1 ゲーム画素


def terrain_block(w, h):
    """地形の塊：w×h 格の地形を一つの部品にまとめる（見た目は 1 格の地形をそのまま並べたもの）。
    原点は塊の下辺中央。格ごとの部品だと地図全体で数千の描画実体になるので、runtime は大きい塊から当てはめる。"""
    def make():
        s = Shape()
        for i in range(w):
            for j in range(h):
                x0 = -w / 2 + i
                s.box((x0, 0, j), (x0 + 1, DEPTH, j + 1), "ink", inset=(0.19, 0.04))
        return s, B
    return make


def terrain_top_run(n, left, right):
    """天板の連なり：n 格の天板付き地形。端が空いている側だけ天板を張り出す（L/R）。"""
    def make():
        s = Shape()
        cap_h, over = 0.2, 0.035
        ztop = 1 - cap_h
        for i in range(n):
            x0 = -n / 2 + i
            s.box((x0, 0, 0), (x0 + 1, DEPTH, ztop), "ink", inset=(0.19, 0.04))
            l = over if left and i == 0 else 0
            r = over if right and i == n - 1 else 0
            s.box((x0 - l, -over, ztop), (x0 + 1 + r, DEPTH, 1.0), "ivory")
        return s, B
    return make


def grid_lines(vertical, count):
    """背景の目盛りを count 本まとめた部品（1 格おき）。横線は幅 1 を部屋の幅へ、縦線は高さ 1 を部屋の高さへ伸ばす。"""
    def make():
        s = Shape()
        w = 0.5 / PX
        for k in range(count):
            if vertical:
                s.box((k - w, -0.008, 0), (k + w, -0.006, 1), "void_line")
            else:
                s.box((-0.5, -0.008, k - w), (0.5, -0.006, k + w), "void_line")
        return s, 0
    return make


BLOCK_SIZES = [(8, 4), (4, 4), (8, 2), (4, 2), (2, 4), (8, 1), (2, 2), (4, 1), (1, 4), (2, 1), (1, 2), (1, 1)]
RUN_LENGTHS = [8, 4, 2, 1]
GRID_COUNTS = [8, 4, 2, 1]


def backdrop():
    """部屋全体の背景（単位 1×1、runtime が cols×rows に伸ばす）。上から差す二段の光の輪を闇に重ねる。"""
    def ellipse_clip(cx, cz, rx, rz):
        pts = [(cx + rx * math.cos(a), cz + rz * math.sin(a)) for a in (i * 2 * math.pi / 96 for i in range(96))]
        # 矩形 [-0.5,0.5]×[0,1] で切る（Sutherland–Hodgman）。
        for axis, bound, keep_less in ((0, 0.5, True), (0, -0.5, False), (1, 1.0, True), (1, 0.0, False)):
            out = []
            for i in range(len(pts)):
                a, b = pts[i - 1], pts[i]
                ina = a[axis] <= bound if keep_less else a[axis] >= bound
                inb = b[axis] <= bound if keep_less else b[axis] >= bound
                if inb:
                    if not ina:
                        t = (bound - a[axis]) / (b[axis] - a[axis])
                        out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
                    out.append(b)
                elif ina:
                    t = (bound - a[axis]) / (b[axis] - a[axis])
                    out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
            pts = out
        return pts

    def make():
        s = Shape()
        s.poly([(-0.5, 0), (0.5, 0), (0.5, 1), (-0.5, 1)], "void", y=0.0)
        s.poly(ellipse_clip(0, 1.08, 0.6, 0.9), "pool", y=-0.002)
        s.poly(ellipse_clip(0, 1.08, 0.6 * 0.62, 0.9 * 0.62), "pool_core", y=-0.004)
        return s, 0
    return make


def plate(pressed):
    def make():
        s = Shape()
        s.box((-0.44, 0.06, 0), (0.44, DEPTH - 0.06, 0.16), "ink")
        top = 0.2 if pressed else 0.27
        s.box((-0.34, 0.1, 0.16), (0.34, DEPTH - 0.1, top), "ivory")
        # 通電灯は別部品（Signal_Lamp）を台の正面に置く。
        return s, 0.025
    return make


def door_jamb():
    def make():
        s = Shape()
        s.box((-0.5, 0, 0), (-0.34, DEPTH, 1), "ink")
        s.box((0.34, 0, 0), (0.5, DEPTH, 1), "ink")
        return s, 0.03
    return make


def door_lintel():
    """扉の鴨居。通電灯は別部品（Signal_Lamp）を正面の中央に置く。"""
    def make():
        s = Shape()
        s.box((-0.5, -0.03, 0), (0.5, DEPTH, 0.36), "ink", inset=(0.06, 0.02))
        return s, 0.03
    return make


def door_leaf_tile():
    def make():
        s = Shape()
        s.box((-0.34, 0.18, 0), (0.34, 0.36, 1), "ivory")
        for k in range(4):
            z = 0.2 + k * 0.25
            s.box((-0.32, 0.16, z), (0.32, 0.2, z + 0.035), "ivory")
        return s, 0.03
    return make


def door_leaf_folded():
    def make():
        s = Shape()
        s.box((-0.34, 0.18, 0), (0.34, 0.36, 0.26), "ivory")
        return s, 0.03
    return make


def checkpoint(active):
    def make():
        s = Shape()
        s.box((-0.26, 0.15, 0), (0.26, DEPTH - 0.15, 0.12), "ink")
        s.box((-0.04, 0.3, 0.12), (0.04, 0.4, 0.62), "ink")
        s.octahedron((0, 0.35, 0.8), 0.15, "glow_ivory" if active else "ivory")
        return s, 0.02
    return make


SPIKE_DIRS = ("Up", "Down", "Left", "Right")


def spikes(direction):
    """棘：墨の台板と赤い四角錐三本。Up は床に、Down は天井に、Left は右の壁に、Right は左の壁に付く（錐はその向きを指す）。"""
    def make():
        s = Shape()
        y0, y1 = 0.1, DEPTH - 0.1
        if direction == "Up":
            s.box((-0.46, y0, 0), (0.46, y1, 0.08), "ink")
            for k in range(3):
                s.pyramid((-0.3 + k * 0.3, DEPTH / 2, 0.08), 0.12, 0.52, "red", axis=(0, 0, 1))
        elif direction == "Down":
            s.box((-0.46, y0, 0.92), (0.46, y1, 1.0), "ink")
            for k in range(3):
                s.pyramid((-0.3 + k * 0.3, DEPTH / 2, 0.92), 0.12, 0.52, "red", axis=(0, 0, -1))
        elif direction == "Left":
            s.box((0.42, y0, 0.04), (0.5, y1, 0.96), "ink")
            for k in range(3):
                s.pyramid((0.42, DEPTH / 2, 0.2 + k * 0.3), 0.12, 0.52, "red", axis=(-1, 0, 0))
        else:
            s.box((-0.5, y0, 0.04), (-0.42, y1, 0.96), "ink")
            for k in range(3):
                s.pyramid((-0.42, DEPTH / 2, 0.2 + k * 0.3), 0.12, 0.52, "red", axis=(1, 0, 0))
        return s, 0
    return make


def oneway():
    def make():
        s = Shape()
        s.box((-0.5, 0.05, 0.92), (0.5, DEPTH - 0.05, 1.0), "ivory")
        s.box((-0.47, 0.12, 0.76), (0.47, DEPTH - 0.12, 0.92), "ink")
        for j in range(3):
            x = -0.34 + j * 0.27
            s.box((x, 0.1, 0.8), (x + 0.14, DEPTH - 0.1, 0.88), "ink_deep")
        return s, 0.025
    return make


LASER_DIRS = {"Left": (-1, 0), "Right": (1, 0), "Up": (0, 1), "Down": (0, -1)}


def laser(direction):
    """激光の発射器（1 格）。レンズを光線の向きへ。墨の箱に象牙のレンズ。"""
    def make():
        s = Shape()
        dx, dz = LASER_DIRS[direction]
        if dx:
            s.box((-0.3, 0.1, 0.26), (0.3, DEPTH - 0.1, 0.74), "ink", inset=(0.05, 0.02))
            lx = 0.3 * dx
            s.box((min(lx, lx + 0.08 * dx), 0.2, 0.39), (max(lx, lx + 0.08 * dx), DEPTH - 0.2, 0.61), "ivory")
        else:
            s.box((-0.24, 0.1, 0.2), (0.24, DEPTH - 0.1, 0.8), "ink", inset=(0.05, 0.02))
            lz = 0.5 + 0.3 * dz
            s.box((-0.11, 0.2, min(lz, lz + 0.08 * dz)), (0.11, DEPTH - 0.2, max(lz, lz + 0.08 * dz)), "ivory")
        return s, 0.03
    return make


def laser_beam():
    def make():
        s = Shape()
        s.box((-0.5, 0.33, -0.035), (0.5, 0.37, 0.035), "glow_red")
        s.box((-0.5, 0.325, -0.01), (0.5, 0.33, 0.01), "glow_ivory")
        return s, 0
    return make


def lockwall_tile():
    def make():
        s = Shape()
        s.box((-0.46, -0.02, 0), (0.46, DEPTH, 1), "ink", inset=(0.1, 0.035))
        s.box((-0.46, -0.05, 0), (-0.38, DEPTH, 1), "ivory")
        s.box((0.38, -0.05, 0), (0.46, DEPTH, 1), "ivory")
        return s, 0.03
    return make


def lockwall_emblem():
    def make():
        s = Shape()
        s.box((-0.2, -0.06, 0), (0.2, 0.1, 0.5), "ivory")
        s.box((-0.13, -0.06, 0.5), (0.13, 0.1, 0.7), "ivory")
        s.box((-0.06, -0.09, 0.12), (0.06, -0.04, 0.34), "ink_deep")
        return s, 0.03
    return make


def bridge_deck():
    """機関の橋の一格（出ている時）。当たりは格いっぱいなので、絵も格の高さいっぱいにする。扉の羽根と同じ
    「動く機関」の象牙の羽目板（横の溝三本）で、地形の墨の塊・一方通行の薄い板と一目で見分ける。上面は天板。"""
    def make():
        s = Shape()
        s.box((-0.5, 0.06, 0.06), (0.5, DEPTH - 0.06, 0.86), "ivory")
        s.box((-0.5, 0.03, 0.86), (0.5, DEPTH - 0.03, 1.0), "ivory")
        for z in (0.24, 0.45, 0.66):
            s.box((-0.44, 0.03, z), (0.44, 0.07, z + 0.06), "ink_deep")
        return s, 0.02
    return make


def bridge_anchor():
    """橋の両端の受け金具（墨の縦の板）。橋板が出入りしても残り、通電灯を載せる。"""
    def make():
        s = Shape()
        s.box((-0.09, -0.03, 0.0), (0.09, DEPTH, 1.0), "ink", inset=(0.03, 0.01))
        return s, 0.015
    return make


def signal_lamp(lit):
    """機関の通電灯。圧力板と、それが動かす扉・橋に同じ灯を付け、通電で一緒に点く（同時に光ることで繋がりを見せる）。
    消えている時も墨の縁と暗い面が残り「ここに灯がある」と分かる。素描でも明暗の差で読めるよう面を大きめにする。"""
    def make():
        s = Shape()
        # 象牙の外枠・墨の灯座・灯面の三層：明るい板（圧力板）の上でも暗い板（鴨居）の上でも明暗の差で読める。
        s.box((-0.24, -0.08, 0.0), (0.24, 0.0, 0.28), "ivory")
        s.box((-0.2, -0.1, 0.04), (0.2, -0.02, 0.24), "ink_deep")
        s.box((-0.15, -0.12, 0.08), (0.15, -0.08, 0.2), "glow_ivory" if lit else "lamp_off")
        return s, 0.0
    return make


def ghost_dash(vertical):
    """引っ込んだ機関の輪郭（残影の菫の破線、長さ 1 格）。扉・橋の「戻るとここに出る」所を同じ線で囲む。
    横は原点の高さを中心に、縦は原点から上へ 1 格。菫は描き方で残影の横縞になり、残影と関わる物だと読める。"""
    def make():
        s = Shape()
        for j in range(3):
            a = 0.05 + j * 0.333
            if vertical:
                s.box((-0.04, 0.1, a), (0.04, 0.22, a + 0.2), "echo")
            else:
                s.box((a - 0.5, 0.1, -0.04), (a - 0.3, 0.22, 0.04), "echo")
        return s, 0
    return make


def echo_marker():
    """残影がいる間、主角の頭の上に浮かべる菫の菱形（いる＝空格で回収できる合図）。残影と同じ菫なので素描でも
    残影の横縞になり、残影に繋がっている印として読める。原点は菱形の下端。"""
    def make():
        s = Shape()
        s.octahedron((0, 0.3, 0.17), 0.13, "echo")
        return s, 0.0
    return make


def echo_streak():
    """回収の光跡：残影から主角へ伸びる菫の棒（中心が原点、長さ 1、厚み 0.12）。ゲーム側が向きと長さを掛ける。"""
    def make():
        s = Shape()
        s.box((-0.5, 0.25, -0.06), (0.5, 0.4, 0.06), "echo")
        return s, 0
    return make


# 弾簧の踏み板の上面は Rules の当たり（格の下から 10/32）と同じ高さ。
SPRING_TOP = 10.0 / 32.0


def spring(state):
    """弾簧：台、折れ線のばね、踏み板。ばねの形で圧力板・一方通行の板と見分ける。
    素描でも状態が一目で分かるよう、使える時は鮮やかな緑（色鉛筆で残る）、冷却中は暗い墨（線だけになる）にする。
    state は ready（使える：緑）、hot（乗って溜めている：踏み板が明るく光る）、off（冷却中：墨）。"""
    def make():
        s = Shape()
        y0, y1 = 0.2, DEPTH - 0.2
        coil = "ink" if state == "off" else "go"
        s.box((-0.44, 0.08, 0.0), (0.44, DEPTH - 0.08, 0.07), "ink")
        # ばね：三段の横棒を左右交互の短い柱で継いだ折れ線（横から見た巻きばね）。
        for k, z in enumerate((0.085, 0.15, 0.215)):
            s.box((-0.3, y0, z), (0.3, y1, z + 0.035), coil)
            if k < 2:
                x = 0.22 if k == 0 else -0.3
                s.box((x, y0, z + 0.035), (x + 0.08, y1, z + 0.065), coil)
        pad = {"ready": "go", "hot": "glow_go", "off": "ink"}[state]
        s.box((-0.46, 0.04, 0.245), (0.46, DEPTH - 0.04, SPRING_TOP), pad)
        s.box((-0.38, 0.015, 0.258), (0.38, 0.045, SPRING_TOP - 0.012), "lamp_off" if state == "off" else "glow_go")
        return s, 0.015
    return make


# 弾簧の矢印の形（格の単位、下辺の中央が原点）。軸は水位の棒が入るよう太め。
ARROW_HEAD = [(-0.42, 0.28), (0.42, 0.28), (0.0, 0.64)]
ARROW_SHAFT = [(-0.2, 0.0), (0.2, 0.0), (0.2, 0.28), (-0.2, 0.28)]
ARROW_FRAME = 0.06            # 冷却中の枠の太さ
GAUGE_HALF, GAUGE_TOP = 0.12, 0.44   # 水位の棒の半幅と、満ちた時の上端（枠の内側に収まる高さ）


def spring_arrow(lit):
    """弾簧の格の上の、上向きの矢印（打ち上げる向き）。奥の壁際に描く（人物の後ろ）。
    lit は鮮やかな緑の塗りの矢印（使える時だけ出す）。
    lit=False は同じ形の暗い緑の枠だけ（冷却中だけ出す）。緑のままなので冷却中も弾簧だと分かり、明るさで「まだ」と読める。
    象牙にすると地形の線と同じ色になって弾簧が埋もれるので使わない。枠は塗りより少し奥に置く。"""
    outline = [ARROW_SHAFT[0], ARROW_SHAFT[1], ARROW_SHAFT[2], ARROW_HEAD[1], ARROW_HEAD[2], ARROW_HEAD[0], ARROW_SHAFT[3]]

    def make():
        s = Shape()
        y = DEPTH - (0.07 if lit else 0.04)
        if lit:
            s.poly(ARROW_HEAD, "glow_go", y=y)
            s.poly(ARROW_SHAFT, "glow_go", y=y)
            return s, 0
        # 枠：外形の各辺を内側へ太さ t の帯にする（素描でもはっきりした線になる太さ）。
        t = ARROW_FRAME
        n = len(outline)
        cx = sum(x for x, _ in outline) / n
        cz = sum(z for _, z in outline) / n
        for i in range(n):
            (ax, az), (bx, bz) = outline[i], outline[(i + 1) % n]
            dx, dz = bx - ax, bz - az
            length = math.hypot(dx, dz)
            nx, nz = -dz / length, dx / length
            if (cx - ax) * nx + (cz - az) * nz < 0:   # 内側（重心の側）へ向ける
                nx, nz = -nx, -nz
            s.poly([(ax, az), (bx, bz), (bx + nx * t, bz + nz * t), (ax + nx * t, az + nz * t)], "dim_go", y=y)
        return s, 0
    return make


def spring_gauge():
    """冷却中の水位：枠の中を下から満ちていく明るい緑の棒（温度計）。矢印の形ではないので、満ちる途中でも
    「使える」の塗りの矢印と見間違えない。C++ が枠の内側の底へ置き、残りの冷却に合わせて縦に伸ばす。
    満ちきったら棒は消え、塗りの矢印に替わって一度弾む。原点は棒の下端。枠の前に置く。"""
    def make():
        s = Shape()
        s.poly([(-GAUGE_HALF, 0.0), (GAUGE_HALF, 0.0), (GAUGE_HALF, GAUGE_TOP - ARROW_FRAME), (-GAUGE_HALF, GAUGE_TOP - ARROW_FRAME)],
               "glow_go", y=DEPTH - 0.07)
        return s, 0
    return make


def fx_ring():
    def make():
        s = Shape()
        s.ring(0.5, 0.09, "glow_ivory")
        return s, 0
    return make


def fx_shard():
    def make():
        s = Shape()
        s.octahedron((0, 0, 0.5), 0.4, "glow_ivory")
        return s, 0
    return make


# 棘は向きごとに光の当たり方が変わるので、向きごとに別の部品を焼く。
SPIKE_AXES = ("Up", "Down", "Left", "Right")
PARTS = {
    "Backdrop": backdrop(),
    "Plate": plate(False), "Plate_Pressed": plate(True),
    "Door_Jamb": door_jamb(), "Door_Lintel": door_lintel(),
    "Door_LeafTile": door_leaf_tile(), "Door_LeafFolded": door_leaf_folded(),
    "Checkpoint_Idle": checkpoint(False), "Checkpoint_Active": checkpoint(True),
    "OneWay": oneway(), "LaserBeam": laser_beam(),
    "Bridge_Deck": bridge_deck(), "Bridge_Anchor": bridge_anchor(),
    "Signal_Lamp": signal_lamp(True), "Signal_LampOff": signal_lamp(False),
    "Ghost_DashH": ghost_dash(False), "Ghost_DashV": ghost_dash(True),
    "Echo_Marker": echo_marker(), "Echo_Streak": echo_streak(),
    "Spring": spring("ready"), "Spring_Hot": spring("hot"), "Spring_Off": spring("off"),
    "Spring_Arrow": spring_arrow(True), "Spring_ArrowDim": spring_arrow(False), "Spring_Gauge": spring_gauge(),
    "LockWall_Tile": lockwall_tile(), "LockWall_Emblem": lockwall_emblem(),
    "FX_Ring": fx_ring(), "FX_Shard": fx_shard(),
    # 地形と目盛りは数格をまとめた部品だけ（格ごとの部品は描画実体が多すぎるので作らない）。
    **{f"Terrain_Block{w}x{h}": terrain_block(w, h) for w, h in BLOCK_SIZES},
    **{f"Terrain_TopRun{n}{side}": terrain_top_run(n, "L" in side, "R" in side) for n in RUN_LENGTHS
       for side in ("", "L", "R", "LR")},
    **{f"Backdrop_Grid{'V' if v else 'H'}{n}": grid_lines(v, n) for v in (False, True) for n in GRID_COUNTS},
}
for d in SPIKE_AXES:
    PARTS[f"Spikes_{d}"] = spikes(d)
for d in LASER_DIRS:
    PARTS[f"Laser_{d}"] = laser(d)


# ---------------------------------------------------------------- 人物（Codex の忍者の姿勢と向きごとに焼く）
POSES = [("Idle", "Ninja_Idle", 0), ("Idle", "Ninja_Idle", 12)] +         [("Jog", "Ninja_Jog", f) for f in (0, 3, 7, 10, 13, 17)] + [("Air", "Ninja_WallBackJump", 4)]


def ninja_parts():
    """骨格の姿勢を評価したメッシュを、高さ 1・底面中央・正面（Z=0）を原点にして返す（左右の向き別）。"""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(NINJA))
    new = [o for o in bpy.data.objects if o not in before]
    rig = next(o for o in new if o.type == "ARMATURE")
    meshes = [o for o in new if o.type == "MESH" and not o.name.startswith("Icosphere")]
    for o in new:
        if o.name.startswith("Icosphere"):
            bpy.data.objects.remove(o)
    # 屏幕の中では 16〜30 画素の大きさなので面を減らす（姿勢ごとに焼くので一体あたり数千面に抑える）。
    for o in meshes:
        dec = o.modifiers.new("reduce", "DECIMATE")
        dec.ratio = 0.08 if "Body" in o.name else 0.12 if "Headband" in o.name else 0.4
    rig.rotation_mode = "XYZ"
    results = {}
    counters = {}
    for state, action_name, frame in POSES:
        act = next(a for a in bpy.data.actions if a.name.startswith(action_name))
        ad = rig.animation_data or rig.animation_data_create()
        ad.action = act
        if hasattr(ad, "action_slot") and getattr(act, "slots", None):
            ad.action_slot = act.slots[0]
        bpy.context.scene.frame_set(frame)
        counters[state] = counters.get(state, -1) + 1
        for facing, yaw in (("R", 90), ("L", -90)):
            rig.rotation_euler = (0, 0, math.radians(yaw))
            bpy.context.view_layer.update()
            deps = bpy.context.evaluated_depsgraph_get()
            bm = bmesh.new()
            fam = bm.faces.layers.string.new("family")
            for o in meshes:
                me = bpy.data.meshes.new_from_object(o.evaluated_get(deps), depsgraph=deps)
                me.transform(o.matrix_world)
                start = len(bm.faces)
                bm.from_mesh(me)
                bm.faces.ensure_lookup_table()
                family = "ink" if "Headband" in o.name else "hero"   # 鉢巻は墨色のまま向きの手掛かりにする
                for f in bm.faces[start:]:
                    f[fam] = family.encode()
                bpy.data.meshes.remove(me)
            lo = [min(v.co[i] for v in bm.verts) for i in range(3)]
            hi = [max(v.co[i] for v in bm.verts) for i in range(3)]
            scale = 1.0 / (hi[2] - lo[2])
            # 底面中央を原点、手前の面を Y=0 に（地形の正面と同じ基準）。高さ 1。
            bmesh.ops.translate(bm, verts=bm.verts, vec=(-(lo[0] + hi[0]) / 2, -lo[1], -lo[2]))
            bmesh.ops.scale(bm, verts=bm.verts, vec=(scale, scale, scale))
            s = Shape.__new__(Shape)
            s.bm, s.fam, s.fixed = bm, fam, bm.faces.layers.int.new("fixed")
            results[f"Ninja_{state}{counters[state]}_{facing}"] = s
    for o in meshes + [rig]:
        bpy.data.objects.remove(o, do_unlink=True)
    return results


# ---------------------------------------------------------------- 書き出し
def write_meta(rel_path, guid, display):
    meta_path = PROJECT / "ProjectSettings" / "AssetMeta" / (rel_path + ".hikari.asset.json")
    meta = json.loads(META_TEMPLATE.read_text(encoding="utf-8-sig"))
    meta.update({"guid": guid, "sourcePath": rel_path, "displayName": display})
    meta["importSettings"]["sourceFormat"] = ".gltf"
    # 舞台の相機は遠く、LOD の簡略化が 1 画素の面取りや溝を消すので、詳細度は一段だけにする。
    meta["importSettings"]["clusterGeometry"]["maxLodCount"] = 1
    meta_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def export(ob, path):
    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(filepath=str(path), export_format="GLTF_SEPARATE", use_selection=True,
                              export_yup=True, export_apply=True, export_materials="EXPORT",
                              export_normals=True, export_texcoords=False, export_animations=False,
                              export_skins=False, export_morph=False, export_extras=False)


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    only = set(args[args.index("--only") + 1].split(",")) if "--only" in args else None
    manifest_path = ROOT / "screen_kit07_manifest.json"
    if only:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    else:
        manifest = {"schema": 1, "kit": "ScreenKit07", "cellUnits": 1, "pivot": "bottom_center_front",
                    "front": "+Z", "up": "+Y", "depth": DEPTH, "gamePixelsPerCell": PX,
                    "backgroundLuminanceBelow": 0.035, "palettes": {}, "parts": {}}
    sources = {name: make for name, make in PARTS.items() if not only or name in only}
    ninja = {}
    if not only or any(name.startswith("Ninja_") for name in only):
        ninja = ninja_parts()
        # 残影：待機の形を残影の菫色で焼く。プレイヤーと同じ形で、色（と画面側の走査線）で見分ける。
        for facing in ("R", "L"):
            ninja[f"Ninja_Echo_{facing}"] = ninja[f"Ninja_Idle0_{facing}"]
        if only:
            ninja = {name: shape for name, shape in ninja.items() if name in only}
    overrides = {"Ninja_Echo_R": "echo", "Ninja_Echo_L": "echo"}
    for palette in PALETTES:
        table = dict(manifest["palettes"].get(palette, {})) if only else {}
        for name, make in list(sources.items()) + [(n, None) for n in ninja]:
            if make is not None:
                shape, bevel = make()
            else:
                shape, bevel = ninja[name], 0
                # 人物の形は配色ごとに作り直せないので、一度目の焼き込みの前に複製を残す。
                copy = bmesh.new()
                shape_bm = shape.bm
                temp = bpy.data.meshes.new("tmp")
                shape_bm.to_mesh(temp)
                copy.from_mesh(temp)
                bpy.data.meshes.remove(temp)
                shape = Shape.__new__(Shape)
                shape.bm, shape.fam = copy, copy.faces.layers.string.get("family")
                shape.fixed = copy.faces.layers.int.get("fixed") or copy.faces.layers.int.new("fixed")
            ob = to_object(shape, name, bevel)
            baked = bake(ob, palette, family_override=overrides.get(name))
            rel = f"{KIT_REL}/{palette}/{name}.gltf"
            export(baked, PROJECT / rel)
            guid = hashlib.md5(f"TD2/ScreenKit07/{palette}/{name}".encode()).hexdigest()
            write_meta(rel, guid, f"{name} ({palette})")
            table[name] = guid
            tris = sum(len(p.vertices) - 2 for p in baked.data.polygons)
            manifest["parts"].setdefault(name, {})["triangles"] = tris
            for o in (ob, baked):
                bpy.data.objects.remove(o, do_unlink=True)
            # 人物の形を使い切った後も、次の配色のために元を残す
        manifest["palettes"][palette] = table
        print("PALETTE", palette, len(table), "parts")
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=1), encoding="utf-8")
    write_header(manifest)
    print("DONE")


def write_header(manifest):
    names = sorted(manifest["parts"])
    lines = [
        "#pragma once",
        "// build_screen_kit.py が生成する表。手で直さない（部品を変えたら Blender のスクリプトを再実行する）。",
        "// 電視の中の部品（ScreenKit07）の配色ごとの模型 GUID。配色 0 = ink（墨と象牙）、1 = teal（青緑の蛍光）。",
        "#include <algorithm>",
        "#include <array>",
        "#include <string_view>",
        "",
        "namespace TD2 {",
        f"inline constexpr std::array<std::string_view, {len(names)}> kScreenKitParts{{",
    ]
    lines += [f'    "{n}",' for n in names]
    lines += ["};", f"inline constexpr std::array<std::array<std::string_view, {len(names)}>, 2> kScreenKitGuids{{{{"]
    for palette in ("ink", "teal"):
        lines.append("    {{")
        lines += [f'        "{manifest["palettes"][palette][n]}",' for n in names]
        lines.append("    }},")
    lines += ["}};",

              "// 部品名の表は整列済みなので二分探索で引く（毎フレーム数百回呼ばれる）。",
              "inline std::string_view ScreenKitGuid(std::string_view part, int palette) {",
              "    const auto it = std::lower_bound(kScreenKitParts.begin(), kScreenKitParts.end(), part);",
              "    if (it == kScreenKitParts.end() || *it != part) { return {}; }",
              "    return kScreenKitGuids[palette == 1 ? 1 : 0][static_cast<size_t>(it - kScreenKitParts.begin())];",
              "}",
              "} // namespace TD2", ""]
    (PROJECT / "Source" / "Runtime" / "TD2ScreenKitAssets.h").write_bytes("\n".join(lines).encode("utf-8"))


main()
