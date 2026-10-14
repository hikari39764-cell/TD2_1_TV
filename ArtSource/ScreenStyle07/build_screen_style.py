# -*- coding: utf-8 -*-
"""電視の中のゲーム世界（3D を 2D に寄せる）の画風試作 ScreenStyle07。

方針
- 中身は本物の 3D（厚み・面取り・影がある）。描き方で 2D に寄せる：光は左上から一つ、面ごとに 3 階調の
  セル陰影（明・基・暗）。面取りが「上と左は明るい縁、下と右は暗い縁」を作り、ドット絵のタイルと同じ読み方になる。
- 色は墨 #435969・象牙 #DDD5C1・危険の赤 #C34737 の三系統と、それぞれの明暗、背景の闇だけ（全部で 13 色）。
- 地形は「墨色の躯体 + 歩ける上面だけ象牙の天板」。歩ける所が一目で分かり、大きな象牙の面で画面が眩しくならない。
- 背景は建物を置かず、1 格の目盛りがかすかに見える暗い壁。足場の影が壁に落ちて奥行きが出る。
- 後処理は三種：像素（1 格 16 画素で描いて 4 倍、外側の 1 画素の輪郭、13 色へ量子化）、漫画（太い輪郭と暗部の網点）、
  草図（墨色の紙に象牙の鉛筆線と斜線）。どれも電視の画面の中だけに掛ける前提。
- 人物は Codex の忍者（ArtSource/Characters/PrototypeNinja/ninja-game.glb）。鉢巻は危険の赤と混同しないよう墨色にする。
出力は Library/ScreenStyle07（Git 対象外）。工厂の游戏镜头の截图に三台分を貼り込んだ見本も作る。
実行: blender -b --factory-startup --python build_screen_style.py
"""

import math
import sys
import zlib
import struct
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent
ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
PALETTE_NAME = ARGS[ARGS.index("--palette") + 1] if "--palette" in ARGS else "teal"
OUT = PROJECT / "Library" / "ScreenStyle07" / PALETTE_NAME
OUT.mkdir(parents=True, exist_ok=True)
NINJA = PROJECT / "ArtSource" / "Characters" / "PrototypeNinja" / "ninja-game.glb"
# 工厂の游戏镜头（TV2 を正面に、左に B、右に A が見える）。三台の画面に試作を貼り込む土台。
V4_CAPTURE = PROJECT / "Library" / "EditorAI" / "Captures" / "e5b155a690e68b1e6748393489ac22c1.png"
GLASS = PROJECT / "Assets" / "TD2" / "Screen" / "screen_glass.png"

PX_LOW = 16        # 像素風の内部解像度（1 格の画素数）
PX_FULL = 64       # 電視の画布と同じ解像度（1 格 64 画素）
DEPTH = 0.7        # 地形の奥行き（格）。7° の俯角で上面がおよそ 1 画素見える
WALL_Y = 0.85      # 背景の壁までの距離。足場の影が下へ 1/4 格ほどずれて落ちる
PITCH = 7.0        # 正交相機の俯角（度）

# ---------------------------------------------------------------- 色（sRGB）
# 明度の組み立ては二案で同じ（背景の闇 < 躯体 < 象牙の天板・人物、危険だけ赤）。違うのは躯体と背景の色相。
PALETTES = {
    "ink": {
        "void": "111A22", "void_line": "18232D", "void_shadow": "0D151C",
        "pool": "15212B", "pool_line": "1B2834", "pool_core": "192733",
        "ink_dark": "2C3B47", "ink": "435969", "ink_light": "5F788A",
        "ivory_shadow": "9C9E95", "ivory": "DDD5C1", "ivory_light": "F4EFE3",
        "red_shadow": "7F2A22", "red": "C34737", "red_light": "EC7A62",
        "outline": "0B1218",
    },
    "teal": {
        "void": "0D2324", "void_line": "122D2E", "void_shadow": "091B1C",
        "pool": "112B2C", "pool_line": "163536", "pool_core": "153435",
        "ink_dark": "1C4744", "ink": "2C6560", "ink_light": "47857D",
        "ivory_shadow": "9FA79C", "ivory": "DDD8C4", "ivory_light": "F4F1E4",
        "red_shadow": "7F2A22", "red": "C34737", "red_light": "EC7A62",
        "outline": "061112",
    },
}
PAL = PALETTES[PALETTE_NAME]


def srgb(hex_color):
    return tuple(int(hex_color[i:i + 2], 16) / 255 for i in (0, 2, 4))


def lin(hex_color):
    return tuple(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in srgb(hex_color))


PALETTE = np.array([srgb(v) for v in PAL.values()], dtype=np.float32)

# 物体の分類（輪郭と光りの判定に使う。pass_index に入れて ID 画像から読む）
ID = {"wall": 1, "terrain": 2, "oneway": 3, "plate": 4, "door": 5, "laser": 6, "beam": 7, "spikes": 8,
      "checkpoint": 9, "lockwall": 10, "player": 11, "echo": 12, "conduit": 13, "glow": 14}
NO_OUTLINE = {ID["wall"], ID["conduit"], ID["echo"], ID["beam"], ID["glow"]}
GLOW_IDS = {ID["beam"], ID["glow"]}

# ---------------------------------------------------------------- 部屋（主場景の実際の地図。GALLERY は八種の機関を並べた見本）
ROOMS = {
    "B": {"cols": 12, "rows": 9, "x": 0, "cells": [
        "111111111111", "100000000001", "100000000001", "100000000001", "100000000001",
        "100000000001", "000000000001", "000000000001", "111111111111"],
        "devices": [("checkpoint", 6, 7, 1, 1, {"active": False})]},
    "TV2": {"cols": 12, "rows": 9, "x": 30, "cells": [
        "000000000000", "000000000000", "111100000000", "000000000000", "000000000000",
        "000000000000", "000000000000", "000000000000", "111111111111"],
        "devices": [("plate", 7, 7, 1, 1, {})], "player": (1.6, 2, "Ninja_Jog", 6)},
    "A": {"cols": 16, "rows": 9, "x": 60, "cells": [
        "1111111111111111", "1000000000000001", "1000000000000001", "1000000000000001",
        "1000000000001111", "0000000000000001", "0000000000000000", "0000000000000000", "1111111111111111"],
        "devices": [("plate", 4, 7, 1, 1, {}), ("door", 8, 5, 1, 3, {"open": False}),
                    ("checkpoint", 11, 7, 1, 1, {"active": True}), ("spikes", 13, 3, 1, 1, {})],
        "conduits": [("plate", 4, 7, "door", 8, 5)]},
    "GALLERY": {"cols": 16, "rows": 9, "x": 90, "cells": [
        "1111111111111111", "1000000000000001", "1000000000000001", "1000000000000001",
        "1000000000001111", "0000000000000001", "0000000000000000", "0000000000000000", "1111111111111111"],
        "devices": [("plate", 4, 7, 1, 1, {"pressed": True}), ("door", 8, 5, 1, 3, {"open": True}),
                    ("checkpoint", 11, 7, 1, 1, {"active": True}), ("spikes", 13, 3, 1, 1, {}),
                    ("spikes", 12, 3, 1, 1, {}), ("oneway", 2, 4, 3, 1, {}),
                    ("laser", 9, 2, 6, 1, {}), ("lockwall", 15, 6, 1, 2, {})],
        "conduits": [("plate", 4, 7, "door", 8, 5)],
        "player": (6.4, 8, "Ninja_Jog", 6), "echo": (3.3, 4, "Ninja_Idle", 0)},
}

# ---------------------------------------------------------------- シーン
bpy.ops.wm.read_factory_settings(use_empty=True)
SCENE = bpy.context.scene
COLL = SCENE.collection


def node(nt, kind):
    return nt.nodes.new(kind)


MAT = {}
GRID_WIDTH = []      # 背景の目盛りの太さ（解像度ごとに 1 画素前後へ合わせる Value ノード）
ECHO_RES = []        # 残影の市松の画素数（解像度ごとに更新する Value ノード）


def cel(name, dark, base, light, t1=0.25, t2=0.7):
    """セル陰影：拡散の明るさ（N·L と影）を 3 段に切り、段ごとに決めた色をそのまま発光で出す（照明の量に依らない）。"""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    diffuse, to_rgb, ramp = node(nt, "ShaderNodeBsdfDiffuse"), node(nt, "ShaderNodeShaderToRGB"), node(nt, "ShaderNodeValToRGB")
    emission, out = node(nt, "ShaderNodeEmission"), node(nt, "ShaderNodeOutputMaterial")
    ramp.color_ramp.interpolation = "CONSTANT"
    els = ramp.color_ramp.elements
    els[0].position, els[0].color = 0.0, (*lin(dark), 1)
    els[1].position, els[1].color = t1, (*lin(base), 1)
    e3 = els.new(t2)
    e3.color = (*lin(light), 1)
    nt.links.new(diffuse.outputs[0], to_rgb.inputs[0])
    nt.links.new(to_rgb.outputs[0], ramp.inputs[0])
    nt.links.new(ramp.outputs[0], emission.inputs[0])
    nt.links.new(emission.outputs[0], out.inputs[0])
    MAT[name] = m
    return m


def flat(name, color, strength=1.0):
    """光らせる部分（光線・点いた灯）。陰影を付けない。"""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    e, o = node(nt, "ShaderNodeEmission"), node(nt, "ShaderNodeOutputMaterial")
    e.inputs[0].default_value = (*lin(color), 1)
    e.inputs[1].default_value = strength
    nt.links.new(e.outputs[0], o.inputs[0])
    MAT[name] = m
    return m


def wall_material():
    """背景の壁：闇の地に 1 格の目盛り、影の所は一段暗い闇。中央の上寄りに舞台の明かりのような光の輪を 2 段置き、
    暗い画面でも電視の中心が光って見えるようにする。輪は灯ではなく壁の模様として描き、足場の陰影を変えない。
    段の境目は 1 画素の市松で混ぜる（ドット絵の階調表現）。"""
    m = bpy.data.materials.new("wall")
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    diffuse, to_rgb, out = node(nt, "ShaderNodeBsdfDiffuse"), node(nt, "ShaderNodeShaderToRGB"), node(nt, "ShaderNodeOutputMaterial")
    lit = node(nt, "ShaderNodeMath")
    lit.operation, lit.inputs[1].default_value = "GREATER_THAN", 0.12
    nt.links.new(diffuse.outputs[0], to_rgb.inputs[0])
    nt.links.new(to_rgb.outputs[0], lit.inputs[0])
    coord, sep = node(nt, "ShaderNodeTexCoord"), node(nt, "ShaderNodeSeparateXYZ")
    nt.links.new(coord.outputs["Object"], sep.inputs[0])
    width = node(nt, "ShaderNodeValue")
    width.outputs[0].default_value = 1 / PX_LOW
    GRID_WIDTH.append(width)
    lines = []
    for axis in ("X", "Z"):
        frac = node(nt, "ShaderNodeMath")
        frac.operation = "FRACT"
        nt.links.new(sep.outputs[axis], frac.inputs[0])
        less = node(nt, "ShaderNodeMath")
        less.operation = "LESS_THAN"
        nt.links.new(frac.outputs[0], less.inputs[0])
        nt.links.new(width.outputs[0], less.inputs[1])
        lines.append(less)
    grid = node(nt, "ShaderNodeMath")
    grid.operation = "MAXIMUM"
    nt.links.new(lines[0].outputs[0], grid.inputs[0])
    nt.links.new(lines[1].outputs[0], grid.inputs[1])
    # 光の輪：物体の属性（部屋ごとの輪の中心と半径）からの楕円距離。
    center, radius = node(nt, "ShaderNodeAttribute"), node(nt, "ShaderNodeAttribute")
    center.attribute_type, center.attribute_name = "OBJECT", "pool_center"
    radius.attribute_type, radius.attribute_name = "OBJECT", "pool_radius"
    xz = node(nt, "ShaderNodeCombineXYZ")
    nt.links.new(sep.outputs["X"], xz.inputs[0])
    nt.links.new(sep.outputs["Z"], xz.inputs[1])
    sub, div, length = node(nt, "ShaderNodeVectorMath"), node(nt, "ShaderNodeVectorMath"), node(nt, "ShaderNodeVectorMath")
    sub.operation, div.operation, length.operation = "SUBTRACT", "DIVIDE", "LENGTH"
    nt.links.new(xz.outputs[0], sub.inputs[0])
    nt.links.new(center.outputs["Vector"], sub.inputs[1])
    nt.links.new(sub.outputs[0], div.inputs[0])
    nt.links.new(radius.outputs["Vector"], div.inputs[1])
    nt.links.new(div.outputs[0], length.inputs[0])
    win, wsep = node(nt, "ShaderNodeTexCoord"), node(nt, "ShaderNodeSeparateXYZ")
    nt.links.new(win.outputs["Window"], wsep.inputs[0])
    rx, ry = node(nt, "ShaderNodeValue"), node(nt, "ShaderNodeValue")
    ECHO_RES.append((rx, ry))
    cells = []
    for axis, res in (("X", rx), ("Y", ry)):
        mul, fl = node(nt, "ShaderNodeMath"), node(nt, "ShaderNodeMath")
        mul.operation, fl.operation = "MULTIPLY", "FLOOR"
        nt.links.new(wsep.outputs[axis], mul.inputs[0])
        nt.links.new(res.outputs[0], mul.inputs[1])
        nt.links.new(mul.outputs[0], fl.inputs[0])
        cells.append(fl)
    csum, cmod, cofs = node(nt, "ShaderNodeMath"), node(nt, "ShaderNodeMath"), node(nt, "ShaderNodeMath")
    csum.operation, cmod.operation, cofs.operation = "ADD", "MODULO", "MULTIPLY"
    nt.links.new(cells[0].outputs[0], csum.inputs[0])
    nt.links.new(cells[1].outputs[0], csum.inputs[1])
    nt.links.new(csum.outputs[0], cmod.inputs[0])
    cmod.inputs[1].default_value = 2.0
    nt.links.new(cmod.outputs[0], cofs.inputs[0])
    cofs.inputs[1].default_value = 0.07
    dist = node(nt, "ShaderNodeMath")
    dist.operation = "ADD"
    nt.links.new(length.outputs["Value"], dist.inputs[0])
    nt.links.new(cofs.outputs[0], dist.inputs[1])

    def band_color(base_hex, line_hex):
        mix = node(nt, "ShaderNodeMix")
        mix.data_type = "RGBA"
        nt.links.new(grid.outputs[0], mix.inputs["Factor"])
        mix.inputs["A"].default_value = (*lin(base_hex), 1)
        mix.inputs["B"].default_value = (*lin(line_hex), 1)
        return mix

    outer = band_color(PAL["void"], PAL["void_line"])
    ring = band_color(PAL["pool"], PAL["pool_line"])
    core = band_color(PAL["pool_core"], PAL["pool_line"])
    in_ring, in_core = node(nt, "ShaderNodeMath"), node(nt, "ShaderNodeMath")
    in_ring.operation, in_core.operation = "LESS_THAN", "LESS_THAN"
    nt.links.new(dist.outputs[0], in_ring.inputs[0])
    nt.links.new(dist.outputs[0], in_core.inputs[0])
    in_ring.inputs[1].default_value, in_core.inputs[1].default_value = 1.0, 0.62
    mix_ring, lit_color = node(nt, "ShaderNodeMix"), node(nt, "ShaderNodeMix")
    mix_ring.data_type = "RGBA"
    lit_color.data_type = "RGBA"
    nt.links.new(in_ring.outputs[0], mix_ring.inputs["Factor"])
    nt.links.new(outer.outputs["Result"], mix_ring.inputs["A"])
    nt.links.new(ring.outputs["Result"], mix_ring.inputs["B"])
    nt.links.new(in_core.outputs[0], lit_color.inputs["Factor"])
    nt.links.new(mix_ring.outputs["Result"], lit_color.inputs["A"])
    nt.links.new(core.outputs["Result"], lit_color.inputs["B"])
    final = node(nt, "ShaderNodeMix")
    final.data_type = "RGBA"
    nt.links.new(lit.outputs[0], final.inputs["Factor"])
    final.inputs["A"].default_value = (*lin(PAL["void_shadow"]), 1)
    nt.links.new(lit_color.outputs["Result"], final.inputs["B"])
    emission = node(nt, "ShaderNodeEmission")
    nt.links.new(final.outputs["Result"], emission.inputs[0])
    nt.links.new(emission.outputs[0], out.inputs[0])
    MAT["wall"] = m
    return m


def echo_material():
    """残影：墨色を画面の 1 画素市松で抜く（半透明の代わり。色を混ぜないので 13 色から外れない）。"""
    m = bpy.data.materials.new("echo")
    m.use_nodes = True
    m.surface_render_method = "DITHERED"
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    coord, sep = node(nt, "ShaderNodeTexCoord"), node(nt, "ShaderNodeSeparateXYZ")
    nt.links.new(coord.outputs["Window"], sep.inputs[0])
    rx, ry = node(nt, "ShaderNodeValue"), node(nt, "ShaderNodeValue")
    ECHO_RES.append((rx, ry))
    parts = []
    for axis, res in (("X", rx), ("Y", ry)):
        mul = node(nt, "ShaderNodeMath")
        mul.operation = "MULTIPLY"
        nt.links.new(sep.outputs[axis], mul.inputs[0])
        nt.links.new(res.outputs[0], mul.inputs[1])
        fl = node(nt, "ShaderNodeMath")
        fl.operation = "FLOOR"
        nt.links.new(mul.outputs[0], fl.inputs[0])
        parts.append(fl)
    add = node(nt, "ShaderNodeMath")
    add.operation = "ADD"
    nt.links.new(parts[0].outputs[0], add.inputs[0])
    nt.links.new(parts[1].outputs[0], add.inputs[1])
    mod = node(nt, "ShaderNodeMath")
    mod.operation, mod.inputs[1].default_value = "MODULO", 2.0
    nt.links.new(add.outputs[0], mod.inputs[0])
    emission, transparent, mix = node(nt, "ShaderNodeEmission"), node(nt, "ShaderNodeBsdfTransparent"), node(nt, "ShaderNodeMixShader")
    emission.inputs[0].default_value = (*lin(PAL["ink_light"]), 1)
    nt.links.new(mod.outputs[0], mix.inputs[0])
    nt.links.new(transparent.outputs[0], mix.inputs[1])
    nt.links.new(emission.outputs[0], mix.inputs[2])
    out = node(nt, "ShaderNodeOutputMaterial")
    nt.links.new(mix.outputs[0], out.inputs[0])
    MAT["echo"] = m
    return m


cel("ink", PAL["ink_dark"], PAL["ink"], PAL["ink_light"])
cel("ink_deep", PAL["outline"], PAL["ink_dark"], PAL["ink"])          # 溝・すき間・奥まった所
cel("ivory", PAL["ivory_shadow"], PAL["ivory"], PAL["ivory_light"])
cel("red", PAL["red_shadow"], PAL["red"], PAL["red_light"])
cel("porcelain", PAL["ivory_shadow"], PAL["ivory"], PAL["ivory_light"], t1=0.18, t2=0.62)
flat("glow_ivory", PAL["ivory_light"])
flat("glow_red", PAL["red_light"])
flat("lamp_off", PAL["ink_dark"])
wall_material()
echo_material()


# ---------------------------------------------------------------- 形
class Shape:
    """箱を集めて一つの物体にする。面取りは物体ごとの修飾子で、箱の稜線すべてに同じ幅で付ける。"""

    def __init__(self, name):
        self.name, self.bm, self.mats = name, bmesh.new(), []

    def _index(self, mat):
        if mat not in self.mats:
            self.mats.append(mat)
        return self.mats.index(mat)

    def box(self, lo, hi, mat, inset=None):
        lo, hi = Vector(lo), Vector(hi)
        center, size = (lo + hi) / 2, hi - lo
        made = bmesh.ops.create_cube(self.bm, size=1.0, matrix=Matrix.Translation(center) @ Matrix.Diagonal((*size, 1)))
        faces = {f for v in made["verts"] for f in v.link_faces}
        self.bm.normal_update()
        index = self._index(mat)
        for f in faces:
            f.material_index = index
        if inset:
            # 正面（-Y）を一段沈めた化粧板にする。面取りと同じ光の読み方で、タイルの中に枠が見える。
            thickness, depth, inner_mat = inset
            front = [f for f in faces if f.normal.y < -0.9]
            result = bmesh.ops.inset_individual(self.bm, faces=front, thickness=thickness, depth=-depth)
            for f in front:
                f.material_index = self._index(inner_mat) if inner_mat else index
        return faces

    def pyramid(self, base_center, half, height, mat, yaw=45.0):
        """四角錐。45° 回すと正面に稜線が来て、左の面が明るく右の面が暗い二色の棘になる。"""
        c = Vector(base_center)
        rot = Matrix.Rotation(math.radians(yaw), 3, "Z")
        corners = [c + rot @ Vector(v) for v in ((-half, -half, 0), (half, -half, 0), (half, half, 0), (-half, half, 0))]
        verts = [self.bm.verts.new(p) for p in corners]
        apex = self.bm.verts.new(c + Vector((0, 0, height)))
        index = self._index(mat)
        for i in range(4):
            f = self.bm.faces.new((verts[i], verts[(i + 1) % 4], apex))
            f.material_index = index
        f = self.bm.faces.new(list(reversed(verts)))
        f.material_index = index

    def octahedron(self, center, radius, mat):
        c = Vector(center)
        pts = [c + Vector(v) * radius for v in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1.3), (0, 0, -1.3))]
        v = [self.bm.verts.new(p) for p in pts]
        index = self._index(mat)
        for a, b in ((0, 2), (2, 1), (1, 3), (3, 0)):
            for pole in (4, 5):
                tri = (v[a], v[b], v[pole]) if pole == 4 else (v[b], v[a], v[pole])
                self.bm.faces.new(tri).material_index = index

    def build(self, kind, bevel=0.045, collection=None):
        bmesh.ops.recalc_face_normals(self.bm, faces=self.bm.faces)
        me = bpy.data.meshes.new(self.name)
        self.bm.to_mesh(me)
        self.bm.free()
        for m in self.mats:
            me.materials.append(MAT[m])
        ob = bpy.data.objects.new(self.name, me)
        (collection or COLL).objects.link(ob)
        if bevel:
            mod = ob.modifiers.new("chamfer", "BEVEL")
            mod.width, mod.segments, mod.limit_method = bevel, 1, "ANGLE"
            mod.angle_limit = math.radians(40)
            mod.harden_normals = False
        for p in me.polygons:
            p.use_smooth = False
        ob.pass_index = ID[kind]
        return ob


def cell_box(room, c, r):
    """格 (c, r) の左下の世界座標（X は部屋の位置を足す。行 0 が一番上）。"""
    return room["x"] + c, room["rows"] - 1 - r


def solid(room, c, r):
    if 0 <= r < room["rows"] and 0 <= c < room["cols"]:
        return room["cells"][r][c] == "1"
    return False


def build_terrain(key, room):
    body, cap = Shape(f"{key}_terrain"), Shape(f"{key}_cap")
    cap_h, over = 0.2, 0.035
    for r in range(room["rows"]):
        for c in range(room["cols"]):
            if not solid(room, c, r):
                continue
            x0, z0 = cell_box(room, c, r)
            top_open = r > 0 and not solid(room, c, r - 1)
            ztop = z0 + 1 - (cap_h if top_open else 0)
            # 躯体：墨色の箱。正面を化粧板にして、格ごとに枠が読めるようにする。
            body.box((x0, 0, z0), (x0 + 1, DEPTH, ztop), "ink", inset=(0.19, 0.04, None))
            h = (c * 73856093 ^ r * 19349663 ^ len(key) * 83492791) & 0xFFFF
            if h % 4 == 0 and not top_open:
                # 壁と天井のところどころに通気口（2 画素の溝が二本）。床は歩く所なので静かにしておく。
                for k in range(2):
                    zz = z0 + 0.38 + k * 0.25
                    body.box((x0 + 0.31, -0.012, zz), (x0 + 0.69, 0.05, zz + 0.125), "ink_deep")
            if top_open:
                # 歩ける上面：象牙の天板。少し手前へ張り出し、開いた横にも少し出して縁を立てる。
                left = over if not solid(room, c - 1, r) else 0
                right = over if not solid(room, c + 1, r) else 0
                cap.box((x0 - left, -over, ztop), (x0 + 1 + right, DEPTH, z0 + 1), "ivory")
    body.build("terrain", bevel=1 / PX_LOW)
    cap.build("terrain", bevel=1 / PX_LOW)


def build_device(key, room, kind, c, r, w, h, opt):
    x0, z1 = cell_box(room, c, r)
    z_bottom = z1 - (h - 1)
    s = Shape(f"{key}_{kind}_{c}_{r}")
    glow = Shape(f"{key}_{kind}_{c}_{r}_glow")
    has_glow = False
    if kind == "plate":
        # 圧板：墨の台座に象牙の踏み板。押すと沈み、前の灯が点く。
        s.box((x0 + 0.06, 0.06, z_bottom), (x0 + 0.94, DEPTH - 0.06, z_bottom + 0.16), "ink")
        top = 0.2 if opt.get("pressed") else 0.27
        s.box((x0 + 0.16, 0.1, z_bottom + 0.16), (x0 + 0.84, DEPTH - 0.1, z_bottom + top), "ivory")
        lamp = (x0 + 0.44, 0.02, z_bottom + 0.04), (x0 + 0.56, 0.07, z_bottom + 0.11)
        if opt.get("pressed"):
            glow.box(*lamp, "glow_ivory")
            has_glow = True
        else:
            s.box(*lamp, "lamp_off")
        s.build(kind, bevel=0.025)
    elif kind == "door":
        # 扉：墨の枠（柱と鴨居）と象牙の巻き上げ扉。開くと扉は鴨居の中へ畳まれる。
        zt = z_bottom + h
        s.box((x0, 0.0, z_bottom), (x0 + 0.16, DEPTH, zt), "ink")
        s.box((x0 + 0.84, 0.0, z_bottom), (x0 + 1, DEPTH, zt), "ink")
        s.box((x0, -0.03, zt - 0.36), (x0 + 1, DEPTH, zt), "ink", inset=(0.06, 0.02, None))
        leaf_bottom = zt - 0.62 if opt.get("open") else z_bottom
        s.box((x0 + 0.16, 0.18, leaf_bottom), (x0 + 0.84, 0.36, zt - 0.36), "ivory")
        for k in range(int((zt - 0.36 - leaf_bottom) / 0.25)):
            zz = leaf_bottom + 0.2 + k * 0.25
            s.box((x0 + 0.18, 0.16, zz), (x0 + 0.82, 0.2, zz + 0.035), "ivory")
        lamp = (x0 + 0.42, -0.06, zt - 0.22), (x0 + 0.58, -0.02, zt - 0.14)
        if opt.get("open"):
            glow.box(*lamp, "glow_ivory")
            has_glow = True
        else:
            s.box(*lamp, "ivory")
        s.build(kind, bevel=0.03)
    elif kind == "checkpoint":
        # 中継点：墨の台と細い柱、八面体の結晶。触れると象牙に光る。
        cx = x0 + 0.5
        s.box((cx - 0.26, 0.15, z_bottom), (cx + 0.26, DEPTH - 0.15, z_bottom + 0.12), "ink")
        s.box((cx - 0.04, 0.3, z_bottom + 0.12), (cx + 0.04, 0.4, z_bottom + 0.62), "ink")
        if opt.get("active"):
            glow.octahedron((cx, 0.35, z_bottom + 0.8), 0.15, "glow_ivory")
            has_glow = True
        else:
            s.octahedron((cx, 0.35, z_bottom + 0.8), 0.15, "ivory")
        s.build(kind, bevel=0.02)
    elif kind == "spikes":
        # 棘：墨の台板に赤い四角錐を三本。45° 回して、左が明るく右が暗い二色の刃にする。
        s.box((x0 + 0.04, 0.1, z_bottom), (x0 + 0.96, DEPTH - 0.1, z_bottom + 0.08), "ink")
        for k in range(3):
            s.pyramid((x0 + 0.2 + k * 0.3, DEPTH / 2, z_bottom + 0.08), 0.12, 0.52, "red")
        s.build(kind, bevel=0.0)
    elif kind == "oneway":
        # 一方通行の足場：象牙の格子の踏み板と墨の梁。梁に抜け穴を開けて「下から通れる」と読ませる。
        zt = z1 + 1
        for k in range(w):
            xx = x0 + k
            s.box((xx, 0.05, zt - 0.08), (xx + 1, DEPTH - 0.05, zt), "ivory")
            s.box((xx + 0.03, 0.12, zt - 0.24), (xx + 0.97, DEPTH - 0.12, zt - 0.08), "ink")
            for j in range(3):
                hx = xx + 0.16 + j * 0.27
                s.box((hx, 0.1, zt - 0.2), (hx + 0.14, DEPTH - 0.1, zt - 0.12), "ink_deep")
        s.build(kind, bevel=0.025)
    elif kind == "laser":
        # 激光：右の壁に付けた発射器（墨の箱に象牙のレンズ）から左へ赤い光線。
        zc = z1 + 0.5
        xe = x0 + w
        s.box((xe - 0.42, 0.1, zc - 0.24), (xe, DEPTH - 0.1, zc + 0.24), "ink", inset=(0.05, 0.02, None))
        s.box((xe - 0.5, 0.2, zc - 0.11), (xe - 0.42, DEPTH - 0.2, zc + 0.11), "ivory")
        glow.box((x0, 0.33, zc - 0.035), (xe - 0.5, 0.37, zc + 0.035), "glow_red")
        glow.box((x0 - 0.04, 0.28, zc - 0.07), (x0 + 0.04, 0.42, zc + 0.07), "glow_ivory")
        has_glow = True
        s.build(kind, bevel=0.03)
    elif kind == "lockwall":
        # 錠前の壁：重い墨の塊に象牙の縁取りと錠の紋章。
        zt = z_bottom + h
        s.box((x0 + 0.04, -0.02, z_bottom), (x0 + 0.96, DEPTH, zt), "ink", inset=(0.1, 0.035, None))
        s.box((x0 + 0.04, -0.05, z_bottom), (x0 + 0.12, DEPTH, zt), "ivory")
        s.box((x0 + 0.88, -0.05, z_bottom), (x0 + 0.96, DEPTH, zt), "ivory")
        zc = z_bottom + h / 2
        s.box((x0 + 0.3, -0.06, zc - 0.28), (x0 + 0.7, 0.1, zc + 0.22), "ivory")
        s.box((x0 + 0.37, -0.06, zc + 0.22), (x0 + 0.63, 0.1, zc + 0.42), "ivory")
        s.box((x0 + 0.44, -0.09, zc - 0.16), (x0 + 0.56, -0.04, zc + 0.06), "ink_deep")
        s.build(kind, bevel=0.03)
    if has_glow:
        glow.build("glow", bevel=0.0)
    else:
        glow.bm.free()


def build_conduits(key, room):
    """圧板と扉をつなぐ配管を背景の壁に這わせる（どの板がどの扉を開けるかを絵で示す）。"""
    for (_, pc, pr, _, dc, dr) in room.get("conduits", []):
        s = Shape(f"{key}_conduit")
        px, pz = cell_box(room, pc, pr)
        dx, dz = cell_box(room, dc, dr)
        y0, y1, t = WALL_Y - 0.06, WALL_Y - 0.01, 0.06
        xa, xb = px + 0.5, dx + 0.5
        zmid = pz + 0.55
        s.box((xa - t / 2, y0, pz + 0.1), (xa + t / 2, y1, zmid + t / 2), "ink")
        s.box((xa - t / 2, y0, zmid - t / 2), (xb + t / 2, y1, zmid + t / 2), "ink")
        s.box((xb - t / 2, y0, zmid - t / 2), (xb + t / 2, y1, dz + 1), "ink")
        s.build("conduit", bevel=0.0)


def build_wall(key, room):
    s = Shape(f"{key}_wall")
    x0 = room["x"]
    s.box((x0 - 2, WALL_Y, -2), (x0 + room["cols"] + 2, WALL_Y + 0.1, room["rows"] + 2), "wall")
    ob = s.build("wall", bevel=0.0)
    ob["pool_center"] = (x0 + room["cols"] * 0.5, room["rows"] * 1.08, 0.0)
    ob["pool_radius"] = (room["cols"] * 0.6, room["rows"] * 0.9, 1.0)
    return ob


for key, room in ROOMS.items():
    build_wall(key, room)
    build_terrain(key, room)
    build_conduits(key, room)
    for (kind, c, r, w, h, opt) in room.get("devices", []):
        build_device(key, room, kind, c, r, w, h, opt)


# ---------------------------------------------------------------- 人物（Codex の忍者）
def import_ninja(material_map, pass_kind):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(NINJA))
    new = [o for o in bpy.data.objects if o not in before]
    rig = next(o for o in new if o.type == "ARMATURE")
    for o in new:
        if o.type == "MESH":
            if o.name.startswith("Icosphere"):
                o.hide_render = True
                continue
            for slot in o.material_slots:
                slot.material = MAT[material_map(slot.material.name if slot.material else "")]
            o.pass_index = ID[pass_kind]
    return rig


def set_pose(rig, action_name, frame):
    act = next(a for a in bpy.data.actions if a.name.startswith(action_name))
    ad = rig.animation_data or rig.animation_data_create()
    ad.action = act
    if hasattr(ad, "action_slot") and getattr(act, "slots", None):
        ad.action_slot = act.slots[0]
    SCENE.frame_set(int(frame))


def place(rig, room, x_cells, row_standing):
    """足を格 row_standing の上面に置く。高さ 0.95 格、右向き、奥行きは地形の中ほど。"""
    _, z0 = cell_box(room, 0, row_standing)
    rig.location = (room["x"] + x_cells, DEPTH * 0.5, z0 + 1)
    rig.rotation_mode = "XYZ"          # glTF は四元数で読み込まれ、欧拉角の指定が効かない
    rig.rotation_euler = (0, 0, math.radians(90))
    rig.scale = (0.95 / 1.785,) * 3


PLAYER = import_ninja(lambda n: "ink" if "Headband" in n else "porcelain", "player")
ECHO = import_ninja(lambda n: "echo", "echo")

# ---------------------------------------------------------------- 光と描画
sun_data = bpy.data.lights.new("key", "SUN")
sun_data.energy = 1 / 0.2546          # 正面を向いた面の Shader to RGB が N·L になる強さ（測定値）
sun_data.angle = 0.0
sun = bpy.data.objects.new("key", sun_data)
COLL.objects.link(sun)
# 左上・手前から。上面 0.82（明）、正面 0.45（基）、右と下 0（暗）、左上の面取りは明。
to_light = Vector((-0.35, -0.45, 0.82)).normalized()
sun.rotation_euler = (-to_light).to_track_quat("-Z", "Y").to_euler()

world = bpy.data.worlds.new("void")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[1].default_value = 0.0
SCENE.world = world
SCENE.render.engine = "BLENDER_EEVEE"
SCENE.view_settings.view_transform = "Standard"
SCENE.view_settings.look = "None"
for key, value in (("use_bloom", False), ("use_gtao", False), ("use_ssr", False), ("use_raytracing", False),
                   ("use_volumetric_shadows", False), ("use_shadows", True)):
    if hasattr(SCENE.eevee, key):
        try:
            setattr(SCENE.eevee, key, value)
        except Exception:
            pass

cam_data = bpy.data.cameras.new("cam")
cam_data.type = "ORTHO"
cam_data.sensor_fit = "HORIZONTAL"
cam = bpy.data.objects.new("cam", cam_data)
COLL.objects.link(cam)
SCENE.camera = cam

# ID と奥行きを書き出すための上書き材質（R = 分類 / 32、G = 視線方向の奥行き / 50）
idm = bpy.data.materials.new("id_depth")
idm.use_nodes = True
nt = idm.node_tree
for n in list(nt.nodes):
    nt.nodes.remove(n)
info, camd, comb = node(nt, "ShaderNodeObjectInfo"), node(nt, "ShaderNodeCameraData"), node(nt, "ShaderNodeCombineXYZ")
d32, d50 = node(nt, "ShaderNodeMath"), node(nt, "ShaderNodeMath")
d32.operation, d32.inputs[1].default_value = "DIVIDE", 32.0
d50.operation, d50.inputs[1].default_value = "DIVIDE", 50.0
nt.links.new(info.outputs["Object Index"], d32.inputs[0])
nt.links.new(camd.outputs["View Z Depth"], d50.inputs[0])
nt.links.new(d32.outputs[0], comb.inputs[0])
nt.links.new(d50.outputs[0], comb.inputs[1])
em, out = node(nt, "ShaderNodeEmission"), node(nt, "ShaderNodeOutputMaterial")
nt.links.new(comb.outputs[0], em.inputs[0])
nt.links.new(em.outputs[0], out.inputs[0])


def frame_room(room, px):
    """部屋全体を正交で正面から。俯角 7°。視線の中心が遊びの平面（Y=0）の部屋の中心に当たる位置へ置く。"""
    cols, rows = room["cols"], room["rows"]
    cam_data.ortho_scale = cols
    dist = 30.0
    cx, cz = room["x"] + cols / 2, rows / 2
    pitch = math.radians(PITCH)
    cam.location = (cx, -dist * math.cos(pitch), cz + dist * math.sin(pitch))
    cam.rotation_euler = (math.radians(90 - PITCH), 0, 0)
    SCENE.render.resolution_x, SCENE.render.resolution_y = cols * px, rows * px
    SCENE.render.resolution_percentage = 100
    for w in GRID_WIDTH:
        w.outputs[0].default_value = 1 / px if px <= 16 else 1.5 / px
    for rx, ry in ECHO_RES:
        rx.outputs[0].default_value = cols * px
        ry.outputs[0].default_value = rows * px


def render(path, data=False, aa=False):
    SCENE.render.filter_size = 1.2 if aa else 0.0
    SCENE.eevee.taa_render_samples = 16 if aa else 1
    SCENE.view_layers[0].material_override = idm if data else None
    SCENE.render.image_settings.file_format = "OPEN_EXR" if data else "PNG"
    if data:
        SCENE.render.image_settings.color_depth = "32"
    else:
        SCENE.render.image_settings.color_mode = "RGB"
        SCENE.render.image_settings.color_depth = "8"
    SCENE.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)


# ---------------------------------------------------------------- 画像の読み書き（上が 0 行目の配列で扱う）
def load(path):
    im = bpy.data.images.load(str(path), check_existing=False)
    w, h = im.size
    a = np.array(im.pixels[:], dtype=np.float32).reshape(h, w, 4)[::-1].copy()
    bpy.data.images.remove(im)
    return a


def save(path, rgb):
    h, w = rgb.shape[:2]
    data = (np.clip(rgb[..., :3], 0, 1) * 255 + 0.5).astype(np.uint8)
    raw = b"".join(b"\x00" + data[y].tobytes() for y in range(h))

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def quantize(rgb):
    flat_rgb = rgb.reshape(-1, 3)
    d = ((flat_rgb[:, None, :] - PALETTE[None, :, :]) ** 2).sum(-1)
    return PALETTE[d.argmin(1)].reshape(rgb.shape)


def neighbors(a):
    """上下左右の値（端は自分）。"""
    up = np.vstack([a[:1], a[:-1]])
    down = np.vstack([a[1:], a[-1:]])
    left = np.hstack([a[:, :1], a[:, :-1]])
    right = np.hstack([a[:, 1:], a[:, -1:]])
    return up, down, left, right


def outline_mask(ids, depth):
    """外側の輪郭：物体の画素に接する、それより奥の画素（背景の壁など）に線を引く。同じ分類どうしの境には引かない。"""
    mask = np.zeros(ids.shape, bool)
    solid_obj = ~np.isin(ids, list(NO_OUTLINE))
    for n_id, n_depth, n_solid in zip(neighbors(ids), neighbors(depth), neighbors(solid_obj)):
        mask |= n_solid & (n_id != ids) & (depth > n_depth + 0.25)
    return mask


def box_blur(img, r):
    out = img.copy()
    for axis in (0, 1):
        pad = [(0, 0)] * img.ndim
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode="edge")
        c = np.cumsum(p, axis=axis, dtype=np.float64)
        c = np.concatenate([np.zeros_like(np.take(c, [0], axis=axis)), c], axis=axis)
        n = out.shape[axis]
        out = ((np.take(c, range(2 * r + 1, 2 * r + 1 + n), axis=axis) - np.take(c, range(0, n), axis=axis)) / (2 * r + 1)).astype(np.float32)
    return out


def glow_layer(ids, color, scale, radius):
    """光る部分だけをぼかして足す（CRT の蛍光体のにじみに近い）。"""
    mask = np.isin(ids, list(GLOW_IDS))[..., None] * color
    if scale > 1:
        mask = mask.repeat(scale, 0).repeat(scale, 1)
    return box_blur(box_blur(mask, radius), radius)


# ---------------------------------------------------------------- 三つの描き方
def style_pixel(color, data):
    ids = np.rint(data[..., 0] * 32).astype(int)
    depth = data[..., 1] * 50
    img = color[..., :3].copy()
    img[outline_mask(ids, depth)] = srgb(PAL["outline"])
    img = quantize(img)
    scale = PX_FULL // PX_LOW
    big = img.repeat(scale, 0).repeat(scale, 1)
    return np.clip(big + glow_layer(ids, img, scale, 6) * 0.55, 0, 1)


def band_index(img):
    """色を「どの段か」（0 暗・1 基・2 明・-1 その他）に分ける。網点や斜線を暗い段だけに置くため。"""
    q = quantize(img)
    dark = np.zeros(img.shape[:2], int) - 1
    for name, band in (("ink_dark", 0), ("ivory_shadow", 0), ("red_shadow", 0), ("ink", 1), ("ivory", 1), ("red", 1),
                       ("ink_light", 2), ("ivory_light", 2), ("red_light", 2)):
        dark[np.all(np.abs(q - srgb(PAL[name])) < 0.01, axis=-1)] = band
    return q, dark


def thick(mask, r):
    out = mask.copy()
    for _ in range(r):
        for n in neighbors(out):
            out = out | n
    return out


def style_comic(color, data):
    """漫画：暗い段は基の色に墨の網点（45°）、輪郭は 3 画素。明るい段はそのまま。"""
    ids = np.rint(data[..., 0] * 32).astype(int)
    depth = data[..., 1] * 50
    q, band = band_index(color[..., :3])
    img = q.copy()
    h, w = band.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    u, v = (xx + yy) / math.sqrt(2), (xx - yy) / math.sqrt(2)
    period = 7.0
    du, dv = (u % period) - period / 2, (v % period) - period / 2
    dots = du * du + dv * dv < 2.3 ** 2
    base_of = {PAL["ink_dark"]: PAL["ink"], PAL["ivory_shadow"]: PAL["ivory"], PAL["red_shadow"]: PAL["red"]}
    for dark_name, base_name in base_of.items():
        sel = np.all(np.abs(q - srgb(dark_name)) < 0.01, axis=-1)
        img[sel] = srgb(base_name)
        img[sel & dots] = srgb(PAL["outline"]) if dark_name != PAL["ivory_shadow"] else srgb(PAL["ink"])
    # 背景の壁：影は墨の大きな網点、光の輪は象牙寄りの小さな網点。地は一色にして網点の大小で階調を出す。
    wall = ids == ID["wall"]
    for tone, ink_r, light_r in ((PAL["void_shadow"], 2.4, 0), (PAL["void"], 0.0, 0), (PAL["void_line"], 0.0, 0),
                                 (PAL["pool"], 0.0, 1.0), (PAL["pool_line"], 0.0, 1.0),
                                 (PAL["pool_core"], 0.0, 1.6)):
        sel = wall & np.all(np.abs(q - srgb(tone)) < 0.01, axis=-1)
        img[sel] = srgb(PAL["void"])
        if ink_r:
            img[sel & (du * du + dv * dv < ink_r ** 2)] = srgb(PAL["outline"])
        if light_r:
            img[sel & (du * du + dv * dv < light_r ** 2)] = srgb(PAL["pool_line"])
    grid_line = wall & (np.all(np.abs(q - srgb(PAL["void_line"])) < 0.01, axis=-1) |
                        np.all(np.abs(q - srgb(PAL["pool_line"])) < 0.01, axis=-1))
    img[grid_line] = srgb(PAL["pool_line"])
    edge = thick(outline_mask(ids, depth), 2)
    img[edge] = srgb(PAL["outline"])
    return np.clip(img + glow_layer(ids, q, 1, 10) * 0.5, 0, 1)


def style_sketch(color, data):
    """草図：墨色の紙に象牙の鉛筆。物体は淡く塗り、輪郭と段の境を線で、暗い段に斜線。"""
    ids = np.rint(data[..., 0] * 32).astype(int)
    depth = data[..., 1] * 50
    q, band = band_index(color[..., :3])
    h, w = band.shape
    rng = np.random.default_rng(7)
    paper = np.array(srgb("1A2733"), np.float32)
    grain = box_blur(rng.normal(0, 1, (h, w, 1)).astype(np.float32), 1) * 0.03
    img = np.broadcast_to(paper, (h, w, 3)) + grain
    wall = ids == ID["wall"]
    fill = np.where(wall[..., None], 0.0, 0.22)
    img = img * (1 - fill) + q * fill
    red = np.all(np.abs(q - srgb(PAL["red"])) < 0.01, axis=-1) | np.all(np.abs(q - srgb(PAL["red_shadow"])) < 0.01, axis=-1)
    img[red] = img[red] * 0.4 + np.array(srgb(PAL["red"])) * 0.6
    # 線：物体の外形（奥行きの段差）と、色の段が変わる所。少し揺らして二度描きの鉛筆にする。
    outer = outline_mask(ids, depth)
    idx = (q * 255).astype(int) @ np.array([65536, 256, 1])
    inner = np.zeros_like(outer)
    for n in neighbors(idx):
        inner |= (n != idx) & ~wall
    lines = outer | inner
    jitter = rng.random((h, w)) < 0.85
    pencil = np.array(srgb(PAL["ivory"]), np.float32)
    img[lines & jitter] = img[lines & jitter] * 0.15 + pencil * 0.85
    shifted = np.roll(np.roll(lines, 1, 0), 1, 1) & (rng.random((h, w)) < 0.5)
    img[shifted] = img[shifted] * 0.6 + pencil * 0.4
    yy, xx = np.mgrid[0:h, 0:w]
    hatch = ((xx + yy) % 6 == 0) & (band == 0)
    img[hatch] = img[hatch] * 0.55 + pencil * 0.45
    return np.clip(img + glow_layer(ids, q, 1, 10) * 0.45, 0, 1)


# ---------------------------------------------------------------- 描画と仕上げ
SET_POSES = {}


def setup_characters(key, room):
    if "player" in room:
        x, row, action, frame = room["player"]
        set_pose(PLAYER, action, frame)
        place(PLAYER, room, x, row)
    else:
        PLAYER.location = (-1000, 0, 0)
    if "echo" in room:
        x, row, action, frame = room["echo"]
        set_pose(ECHO, action, frame)
        place(ECHO, room, x, row)
        # 残影はプレイヤーと同じ動作の別時刻ではなく、待機姿勢の抜け殻として置く。
        set_pose(PLAYER, room["player"][2], room["player"][3])
    else:
        ECHO.location = (-1000, 0, 0)


results = {}
for key, room in ROOMS.items():
    setup_characters(key, room)
    frame_room(room, PX_LOW)
    render(OUT / f"{key}_low_color.png")
    render(OUT / f"{key}_low_data.exr", data=True)
    frame_room(room, PX_FULL)
    render(OUT / f"{key}_full_color.png", aa=True)
    render(OUT / f"{key}_full_data.exr", data=True)
    low_c, low_d = load(OUT / f"{key}_low_color.png"), load(OUT / f"{key}_low_data.exr")
    full_c, full_d = load(OUT / f"{key}_full_color.png"), load(OUT / f"{key}_full_data.exr")
    results[key] = {
        "cel": full_c[..., :3],
        "pixel": style_pixel(low_c, low_d),
        "comic": style_comic(full_c, full_d),
        "sketch": style_sketch(full_c, full_d),
    }
    for style, img in results[key].items():
        save(OUT / f"{key}_{style}.png", img)
    print("ROOM", key, "done")


# ---------------------------------------------------------------- 工厂の游戏镜头へ貼り込む
def screen_rects(capture):
    """截图の中の三台の画面（青緑の地）の範囲を探す。左右の切れた画面は縦の長さと格数から横幅を補う。"""
    rgb = capture[..., :3]
    r_, g_, b_ = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    teal = (g_ > r_ + 0.05) & (b_ > r_ + 0.03) & (g_ > b_ * 0.85) & (g_ > 0.06)
    cols = teal.sum(0) > 40
    runs, start = [], None
    for x, v in enumerate(list(cols) + [False]):
        if v and start is None:
            start = x
        elif not v and start is not None:
            if x - start > 40:
                runs.append((start, x))
            start = None
    rects = []
    for (x0, x1) in runs:
        rows = np.where(teal[:, x0:x1].sum(1) > (x1 - x0) * 0.3)[0]
        # 画面の縁はガラスの減光で暗く、青緑の判定から少し外れるので数画素広げる。
        rects.append([x0 - 3, x1 + 3, rows.min() - 3, rows.max() + 4])
    return rects


def paste(dst, img, rect, cells):
    x0, x1, y0, y1 = rect
    height = y1 - y0
    width = int(round(height * cells[0] / cells[1]))
    if x0 <= 2:            # 左で切れている
        x0 = x1 - width
    elif x1 >= dst.shape[1] - 2:   # 右で切れている
        x1 = x0 + width
    glass = load(GLASS)
    h, w = dst.shape[:2]
    for y in range(max(0, y0), min(h, y1)):
        sy = int((y - y0) / (y1 - y0) * img.shape[0])
        gy = int((y - y0) / (y1 - y0) * glass.shape[0])
        xs = np.arange(max(0, x0), min(w, x1))
        sx = ((xs - x0) / (x1 - x0) * img.shape[1]).astype(int)
        gx = ((xs - x0) / (x1 - x0) * glass.shape[1]).astype(int)
        g = glass[gy, gx]
        # 工程の画面材質の増幅に近づける（線形で 1.5 倍してから sRGB に戻す）。
        row = img[sy, sx]
        row = np.where(row <= 0.04045, row / 12.92, ((row + 0.055) / 1.055) ** 2.4) * 1.5
        row = np.where(row <= 0.0031308, row * 12.92, 1.055 * np.clip(row, 0, 1) ** (1 / 2.4) - 0.055)
        dst[y, xs, :3] = row * (1 - g[:, 3:4]) + g[:, :3] * g[:, 3:4]


capture = load(V4_CAPTURE)
rects = screen_rects(capture)
print("SCREENS", rects)
if len(rects) == 3:
    for style in ("pixel", "comic", "sketch", "cel"):
        comp = capture.copy()
        for rect, key in zip(rects, ("B", "TV2", "A")):
            room = ROOMS[key]
            paste(comp, results[key][style], rect, (room["cols"], room["rows"]))
        # 画面の光が周りににじむ（CRT の明るい所だけ）。
        bright = np.clip(comp[..., :3] - 0.55, 0, 1)
        comp[..., :3] = np.clip(comp[..., :3] + box_blur(box_blur(bright, 9), 9) * 0.35, 0, 1)
        save(OUT / f"V4_{style}.png", comp[..., :3])

# 見本帳：GALLERY の四つの描き方を 2×2 に並べる
g = results["GALLERY"]
top = np.hstack([g["cel"], np.ones((576, 12, 3), np.float32) * 0.1, g["pixel"]])
bottom = np.hstack([g["comic"], np.ones((576, 12, 3), np.float32) * 0.1, g["sketch"]])
sheet = np.vstack([top, np.ones((12, top.shape[1], 3), np.float32) * 0.1, bottom])
save(OUT / "GALLERY_sheet.png", sheet)
bpy.ops.wm.save_as_mainfile(filepath=str(OUT / "ScreenStyle07.blend"))
print("DONE")
