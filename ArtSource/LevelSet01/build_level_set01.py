# -*- coding: utf-8 -*-
"""地図 5 用の関卡セット（LevelSet01）：遊べる電視（画面 12:7 = 24×14 格）と、それを吊る・載せる部品。

方針
- FactorySet01 と同じ約束：Blender の -Y が正面、底面中心が原点、FBX は +Y 上・+Z 正面・メートル。
- 遊べる電視の画面は全て 24×14 格と同じ縦横比（12:7）。画面の材質名 TD2_Screen は Runtime の捕捉が探す槽。
- 材質は工場セットと同じ名前・同じ値（素朴な少数の材質）。吊る電視は天面に吊り環を持つ。
- 部品は一点ずつ FBX にし、確認用に一枚の見本画像を出す。
実行: blender -b --factory-startup --python build_level_set01.py -- [--noexport] [--samples 16]
"""

import json
import math
import sys
from pathlib import Path

import bmesh
import bpy
from mathutils import Euler, Matrix, Vector

ROOT = Path(__file__).resolve().parent
EXPORTS, PREVIEWS = ROOT / "Exports", ROOT / "Previews"
for d in (EXPORTS, PREVIEWS):
    d.mkdir(parents=True, exist_ok=True)
ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
EXPORT = "--noexport" not in ARGS
SAMPLES = int(ARGS[ARGS.index("--samples") + 1]) if "--samples" in ARGS else 16

bpy.ops.wm.read_factory_settings(use_empty=True)
SCENE = bpy.context.scene
MAT = {}


def lin(hex_color):
    v = [int(hex_color[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    return tuple(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in v)


def pbr(name, hex_color, rough, metal=0.0, emission=None, strength=0.0):
    """FBX が運ぶのは基本色・粗さ・金属・発光だけ。工場セットと同じ名前と値にそろえる。"""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*lin(hex_color), 1)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    if emission:
        bsdf.inputs["Emission Color"].default_value = (*emission, 1)
        bsdf.inputs["Emission Strength"].default_value = strength
    MAT[name] = m
    return m


pbr("TD2F_Steel", "2C2D2F", 0.5)
pbr("TD2F_BareMetal", "8A8A88", 0.42, metal=1.0)
pbr("TD2F_Wood", "5B4331", 0.58)
pbr("TD2F_PlasticBeige", "A9A08F", 0.5)
pbr("TD2F_PlasticDark", "2A2826", 0.45)
pbr("TD2F_Fabric", "24221F", 0.95)
pbr("TD2F_Rubber", "141313", 0.7)
pbr("TD2F_Paper", "D8D2C4", 0.9)
pbr("TD2F_Sticky", "D9CF9A", 0.85)
pbr("TD2F_Concrete", "4B4946", 0.9)
pbr("TD2F_ConcreteDark", "2E2D2B", 0.95)
pbr("TD2_Screen", "0D0F10", 0.07)  # 既存の画面表示コードが探すスロット名
pbr("TD2F_LedRed", "C8463C", 0.4, emission=lin("C8463C"), strength=6.0)
pbr("TD2F_LedAmber", "D99A3C", 0.4, emission=lin("D99A3C"), strength=5.0)
pbr("TD2F_BulbWarm", "FFFFFF", 0.3, emission=(1.0, 0.72, 0.42), strength=30.0)


class Builder:
    """箱・梁・円柱を一つの bmesh に溜め、材質スロット付きの一物体にする。UV は 1 m = 1 の箱投影。"""

    def __init__(self, name):
        self.name, self.bm, self.mats = name, bmesh.new(), []

    def _tag(self, verts, mat):
        if mat not in self.mats:
            self.mats.append(mat)
        idx = self.mats.index(mat)
        for f in {f for v in verts for f in v.link_faces}:
            f.material_index = idx

    def box(self, center, size, mat, rot=(0, 0, 0)):
        m = Matrix.Translation(center) @ Euler(rot).to_matrix().to_4x4() @ Matrix.Diagonal((*size, 1))
        self._tag(bmesh.ops.create_cube(self.bm, size=1.0, matrix=m)["verts"], mat)

    def beam(self, p1, p2, w, h, mat):
        p1, p2 = Vector(p1), Vector(p2)
        d = p2 - p1
        x = d.normalized()
        up = Vector((0, 0, 1)) if abs(x.z) < 0.95 else Vector((0, 1, 0))
        y = up.cross(x).normalized()
        z = x.cross(y)
        m = Matrix.Translation((p1 + p2) / 2) @ Matrix((x, y, z)).transposed().to_4x4() @ Matrix.Diagonal((d.length, w, h, 1))
        self._tag(bmesh.ops.create_cube(self.bm, size=1.0, matrix=m)["verts"], mat)

    def tube(self, p1, p2, r, mat, segs=10):
        """足場の鋼管：二点を結ぶ円柱。"""
        p1, p2 = Vector(p1), Vector(p2)
        d = p2 - p1
        rot = d.to_track_quat("Z", "Y").to_matrix().to_4x4()
        m = Matrix.Translation((p1 + p2) / 2) @ rot
        res = bmesh.ops.create_cone(self.bm, cap_ends=True, cap_tris=False, segments=segs, radius1=r, radius2=r,
                                    depth=d.length, matrix=m)
        self._tag(res["verts"], mat)

    def cyl(self, center, r, depth, mat, segs=16, rot=(0, 0, 0)):
        m = Matrix.Translation(center) @ Euler(rot).to_matrix().to_4x4()
        res = bmesh.ops.create_cone(self.bm, cap_ends=True, cap_tris=False, segments=segs, radius1=r, radius2=r,
                                    depth=depth, matrix=m)
        self._tag(res["verts"], mat)

    def ring(self, center, r, thick, mat, rot=(0, 0, 0), segs=12):
        """吊り環・鎖の輪：細い円環（四角断面の輪で十分）。"""
        m = Matrix.Translation(center) @ Euler(rot).to_matrix().to_4x4()
        for i in range(segs):
            a0, a1 = 2 * math.pi * i / segs, 2 * math.pi * (i + 1) / segs
            p0 = m @ Vector((r * math.cos(a0), 0, r * math.sin(a0)))
            p1 = m @ Vector((r * math.cos(a1), 0, r * math.sin(a1)))
            self.beam(p0, p1, thick, thick, mat)

    def build(self, bevel=0.0):
        bm = self.bm
        bm.normal_update()
        uv = bm.loops.layers.uv.new("UVMap")
        for f in bm.faces:
            n = f.normal
            ax = max(range(3), key=lambda i: abs(n[i]))
            for loop in f.loops:
                c = loop.vert.co
                loop[uv].uv = (c.y, c.z) if ax == 0 else (c.x, c.z) if ax == 1 else (c.x, c.y)
        me = bpy.data.meshes.new(self.name)
        bm.to_mesh(me)
        bm.free()
        for m in self.mats:
            me.materials.append(MAT[m])
        obj = bpy.data.objects.new(self.name, me)
        if bevel > 0:
            for p in me.polygons:
                p.use_smooth = True
            mod = obj.modifiers.new("Bevel", "BEVEL")
            mod.width, mod.segments, mod.limit_method = bevel, 2, "ANGLE"
            mod.harden_normals = True
            wn = obj.modifiers.new("WeightedNormal", "WEIGHTED_NORMAL")
            wn.keep_sharp = True
        return obj


PART = {}


def screen_mesh(name, w, h, cx, bottom, y):
    me = bpy.data.meshes.new(name)
    me.from_pydata([(cx - w / 2, y, bottom), (cx + w / 2, y, bottom),
                    (cx + w / 2, y, bottom + h), (cx - w / 2, y, bottom + h)], [], [(0, 1, 2, 3)])
    uv = me.uv_layers.new(name="UVMap")
    for i, p in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[i].uv = p
    me.materials.append(MAT["TD2_Screen"])
    o = bpy.data.objects.new(name, me)
    o["role"] = "Screen"
    return o


def part(name, objs, info=None):
    for i, o in enumerate(objs):
        o.name = name + ("__Body" if i == 0 else "__" + o.get("role", f"Part{i}"))
        o.data.name = o.name
    PART[name] = {"objs": objs, "info": info or {}}


# ================================================================ 遊べる電視（画面 12:7）
SCREEN_ASPECT = 24 / 14


def eyebolts(b, top, half):
    """天面の吊り環（左右二つ）。鎖の下端はこの環の頂点（top + 0.07）に来る。"""
    for x in (-half, half):
        b.box((x, 0, top + 0.012), (0.07, 0.05, 0.024), "TD2F_Steel")
        b.ring((x, 0, top + 0.045), 0.026, 0.012, "TD2F_BareMetal")


def tv(name, fn, w, bottom, y, cx=0.0, height=None, hang=None):
    b = Builder(name)
    fn(b)
    h = round(w / SCREEN_ASPECT, 4)
    info = {"screen": {"w": w, "h": h, "cx": cx, "bottom": bottom, "y": y}, "height": height}
    if hang:
        info["hang"] = hang  # 吊り点（局所 X, 高さ）
    part(name, [b.build(bevel=0.008), screen_mesh(name + "_scr", w, h, cx, bottom, y)], info)


def hub(b):
    # 開発者の愛機：クリーム色の業務用モニター。画面が一回り大きく、操作列と付箋で「戻ってくる場所」と分かる。
    b.box((0, 0.02, 0.44), (1.24, 0.6, 0.88), "TD2F_PlasticBeige")
    b.box((0, 0.40, 0.44), (0.92, 0.2, 0.64), "TD2F_PlasticBeige")
    b.box((0, -0.288, 0.47), (1.14, 0.03, 0.74), "TD2F_PlasticDark")
    b.box((0, -0.29, 0.115), (1.12, 0.03, 0.13), "TD2F_PlasticBeige")
    for k, x in enumerate((-0.42, -0.34, -0.26, -0.18)):
        b.cyl((x, -0.31, 0.115), 0.022, 0.03, "TD2F_PlasticDark", rot=(math.pi / 2, 0, 0))
    b.box((0.40, -0.307, 0.115), (0.016, 0.006, 0.016), "TD2F_LedAmber")
    b.box((0.22, -0.307, 0.115), (0.16, 0.004, 0.03), "TD2F_Paper")
    for k in range(7):
        b.box((-0.36 + 0.12 * k, 0.05, 0.882), (0.07, 0.4, 0.008), "TD2F_Fabric")
    b.box((0.555, -0.305, 0.80), (0.10, 0.004, 0.10), "TD2F_Sticky", rot=(0, 0.12, 0))   # 画面の外、右上の角
    eyebolts(b, 0.88, 0.46)


def wide(b):
    # 横長の家庭用ブラウン管：黒い筐体にベージュの前枠。
    b.box((0, 0.02, 0.39), (1.10, 0.52, 0.78), "TD2F_PlasticDark")
    b.box((0, 0.36, 0.40), (0.78, 0.2, 0.56), "TD2F_PlasticDark")
    b.box((0, -0.255, 0.39), (1.08, 0.02, 0.74), "TD2F_PlasticBeige")
    b.box((0, -0.262, 0.40), (0.92, 0.02, 0.58), "TD2F_PlasticDark")
    for k in range(9):
        b.box((-0.32 + 0.08 * k, -0.268, 0.06), (0.05, 0.006, 0.012), "TD2F_Fabric")
    b.box((0.44, -0.268, 0.06), (0.012, 0.006, 0.012), "TD2F_LedRed")
    eyebolts(b, 0.78, 0.42)


def steel(b):
    # 業務用の鋼板モニター：左右の取っ手と耳金、下に押釦の列。
    b.box((0, 0, 0.37), (1.0, 0.5, 0.74), "TD2F_Steel")
    for x in (-0.515, 0.515):
        b.box((x, -0.24, 0.37), (0.05, 0.02, 0.74), "TD2F_Steel")
        b.beam((x, -0.29, 0.14), (x, -0.29, 0.60), 0.022, 0.022, "TD2F_BareMetal")
        for z in (0.14, 0.60):
            b.beam((x, -0.25, z), (x, -0.30, z), 0.022, 0.022, "TD2F_BareMetal")
    b.box((0, -0.255, 0.39), (0.94, 0.02, 0.62), "TD2F_PlasticDark")
    for k in range(5):
        b.box((-0.38 + 0.06 * k, -0.267, 0.055), (0.035, 0.01, 0.022), "TD2F_BareMetal")
    b.box((0.40, -0.267, 0.055), (0.014, 0.006, 0.014), "TD2F_LedAmber")
    for k in range(6):
        b.box((0.0, 0.252, 0.18 + 0.08 * k), (0.7, 0.006, 0.02), "TD2F_PlasticDark")
    eyebolts(b, 0.74, 0.40)


def console(b):
    # 脚付きの木の家具テレビ：床置き用。
    for x in (-0.5, 0.5):
        for y in (-0.18, 0.18):
            b.box((x, y, 0.1), (0.045, 0.045, 0.2), "TD2F_Wood")
    b.box((0, 0, 0.6), (1.16, 0.52, 0.8), "TD2F_Wood")
    b.box((0, -0.265, 0.64), (0.98, 0.02, 0.6), "TD2F_PlasticDark")
    b.box((0, -0.267, 0.27), (0.98, 0.02, 0.1), "TD2F_Fabric")
    for x in (0.36, 0.44):
        b.cyl((x, -0.28, 0.27), 0.018, 0.02, "TD2F_BareMetal", rot=(math.pi / 2, 0, 0))


tv("SM_L_TV_Hub", hub, 0.96, 0.20, -0.304, height=0.88, hang=[(-0.46, 0.95), (0.46, 0.95)])
tv("SM_L_TV_Wide", wide, 0.84, 0.155, -0.273, height=0.78, hang=[(-0.42, 0.85), (0.42, 0.85)])
tv("SM_L_TV_Steel", steel, 0.84, 0.135, -0.266, height=0.74, hang=[(-0.40, 0.81), (0.40, 0.81)])
tv("SM_L_TV_Console", console, 0.84, 0.395, -0.276, height=1.0)


# ================================================================ 吊る・載せる部品
def cage_lamp():
    """吊り下げの籠付き作業灯（暖色）。原点は下端の中心、吊り輪の上端は +0.42。"""
    b = Builder("cagelamp")
    b.ring((0, 0, -0.03), 0.025, 0.01, "TD2F_BareMetal")
    b.cyl((0, 0, -0.09), 0.035, 0.08, "TD2F_Rubber")
    b.cyl((0, 0, -0.15), 0.07, 0.04, "TD2F_Steel")
    for i in range(6):
        a = i * math.pi / 3
        b.beam((0.07 * math.cos(a), 0.07 * math.sin(a), -0.17), (0.085 * math.cos(a), 0.085 * math.sin(a), -0.36),
               0.008, 0.008, "TD2F_BareMetal")
    b.ring((0, 0, -0.26), 0.088, 0.008, "TD2F_BareMetal", rot=(math.pi / 2, 0, 0), segs=16)
    b.cyl((0, 0, -0.37), 0.06, 0.012, "TD2F_BareMetal")
    bulb = Builder("cagelamp_bulb")
    bulb.cyl((0, 0, -0.25), 0.04, 0.12, "TD2F_BulbWarm", segs=12)
    o = bulb.build()
    o["role"] = "Bulb"
    body = b.build(bevel=0.002)
    # 原点を下端中心へ（工場セットの約束）。上端は +0.42。
    for ob in (body, o):
        ob.data.transform(Matrix.Translation((0, 0, 0.42)))
    part("SM_L_CageLamp", [body, o], {"cast_shadow": False})


def cable_sag():
    """垂れた配線：長さ 1（X 方向に伸ばして使う）、中央で 0.18 垂れる。原点は左端。"""
    b = Builder("cable")
    n = 16
    pts = [(i / n, 0, -0.18 * (1 - (2 * i / n - 1) ** 2)) for i in range(n + 1)]
    for p1, p2 in zip(pts, pts[1:]):
        b.beam(p1, p2, 0.018, 0.018, "TD2F_Rubber")
    part("SM_L_Cable_Sag", [b.build()], {"cast_shadow": False})


def monolith():
    """塔の芯：打ち放しの角柱（2.6×8.0×2.6）。型枠の目地と セパ穴だけを刻む。三面に遊べる電視を掛ける。"""
    b = Builder("monolith")
    W, H = 2.6, 8.0
    b.box((0, 0, 0.06), (W + 0.16, W + 0.16, 0.12), "TD2F_ConcreteDark")
    b.box((0, 0, H / 2), (W, W, H), "TD2F_Concrete")
    # 型枠の目地：1.2 m ごとの水平の溝と、0.9 m ごとの縦の継ぎ目（四面とも）。
    g = 0.012
    for k in range(1, int(H / 1.2) + 1):
        z = k * 1.2
        for sgn in (-1, 1):
            b.box((0, sgn * (W / 2 + 0.002), z), (W + 0.004, 0.006, g), "TD2F_ConcreteDark")
            b.box((sgn * (W / 2 + 0.002), 0, z), (0.006, W + 0.004, g), "TD2F_ConcreteDark")
    for x in (-0.9, 0.0, 0.9):
        for sgn in (-1, 1):
            b.box((x, sgn * (W / 2 + 0.002), H / 2), (g, 0.006, H), "TD2F_ConcreteDark")
            b.box((sgn * (W / 2 + 0.002), x, H / 2), (0.006, g, H), "TD2F_ConcreteDark")
    # セパ穴：目地の間に並ぶ小さな丸。
    for k in range(int(H / 1.2)):
        z = k * 1.2 + 0.6
        for x in (-0.45, 0.45):
            for sgn in (-1, 1):
                b.cyl((x, sgn * (W / 2 + 0.003), z), 0.018, 0.01, "TD2F_ConcreteDark", segs=8, rot=(math.pi / 2, 0, 0))
                b.cyl((sgn * (W / 2 + 0.003), x, z), 0.018, 0.01, "TD2F_ConcreteDark", segs=8, rot=(0, math.pi / 2, 0))
    b.box((0, 0, H + 0.03), (W + 0.04, W + 0.04, 0.06), "TD2F_ConcreteDark")
    part("SM_L_Monolith", [b.build(bevel=0.01)], {"size": [W, H, W]})


monolith()
cage_lamp()
cable_sag()


# ================================================================ 書き出しと見本
def export():
    manifest = {"schema_version": 1, "set": "LevelSet01",
                "conventions": {"units": "meters", "up": "+Y", "front": "+Z", "pivot": "bottom_center",
                                "screen_material": "TD2_Screen", "screen_aspect": "12:7 (24x14 cells)"},
                "parts": []}
    coll = bpy.data.collections.new("_Export")
    SCENE.collection.children.link(coll)
    for name, p in PART.items():
        objs = p["objs"]
        for o in objs:
            coll.objects.link(o)
        bpy.ops.object.select_all(action="DESELECT")
        for o in objs:
            o.select_set(True)
        bpy.context.view_layer.objects.active = objs[0]
        if EXPORT:
            bpy.ops.export_scene.fbx(filepath=str(EXPORTS / f"{name}.fbx"), use_selection=True, object_types={"MESH"},
                                     global_scale=1.0, apply_unit_scale=True, apply_scale_options="FBX_SCALE_UNITS",
                                     axis_forward="-Z", axis_up="Y", use_space_transform=True, bake_space_transform=True,
                                     use_mesh_modifiers=True, mesh_smooth_type="OFF", use_triangles=True,
                                     use_tspace=False, bake_anim=False, add_leaf_bones=False, use_custom_props=False)
        dg = bpy.context.evaluated_depsgraph_get()
        pts, tris = [], 0
        for o in objs:
            ev = o.evaluated_get(dg)
            me = ev.to_mesh()
            me.calc_loop_triangles()
            tris += len(me.loop_triangles)
            pts += [v.co.copy() for v in me.vertices]
            ev.to_mesh_clear()
        lo = [min(v[i] for v in pts) for i in range(3)]
        hi = [max(v[i] for v in pts) for i in range(3)]
        entry = {"id": name, "file": f"Exports/{name}.fbx", "triangles": tris,
                 "dimensions_m": [round(hi[0] - lo[0], 4), round(hi[2] - lo[2], 4), round(hi[1] - lo[1], 4)]}
        info = p["info"]
        if "screen" in info:
            s = info["screen"]
            entry["screen"] = {"width_m": s["w"], "height_m": s["h"], "center_x_m": s["cx"], "bottom_m": s["bottom"],
                               "front_z_m": round(-s["y"], 4)}
            entry["height_m"] = info["height"]
        for key in ("hang", "deck", "cast_shadow", "size"):
            if key in info:
                entry[key] = info[key]
        manifest["parts"].append(entry)
    (ROOT / "level_set01_manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print("EXPORTED", len(PART))
    return coll


def preview(coll):
    """全部品を横一列に並べた見本（確認用、Git 対象外）。"""
    x = 0.0
    for name, p in PART.items():
        body = p["objs"][0]
        w = max(v.co.x for v in body.data.vertices) - min(v.co.x for v in body.data.vertices)
        for o in p["objs"]:
            o.location.x = x + w / 2
            if o.get("role") == "Screen":
                o.data.materials[0] = pbr("prev_screen", "1E625B", 0.3, emission=lin("348077"), strength=2.0)
        x += w + 0.5
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    SCENE.collection.objects.link(cam)
    cam.location = (x / 2, -11.0, 1.6)
    cam.rotation_euler = (math.radians(82), 0, 0)
    cam.data.lens = 24
    SCENE.camera = cam
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.0
    sun.rotation_euler = (math.radians(50), math.radians(10), math.radians(-30))
    SCENE.collection.objects.link(sun)
    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.05, 0.05, 0.055, 1)
    SCENE.world = world
    SCENE.render.engine = "CYCLES"
    SCENE.cycles.samples = SAMPLES
    SCENE.render.resolution_x, SCENE.render.resolution_y = 1800, 600
    SCENE.render.filepath = str(PREVIEWS / "parts.png")
    bpy.ops.render.render(write_still=True)
    print("PREVIEW", SCENE.render.filepath)


preview(export())
