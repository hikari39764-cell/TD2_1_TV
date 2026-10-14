# -*- coding: utf-8 -*-
"""雨夜の廃工場セット（FactorySet01）：部品ごとの FBX、配置表、確認用レンダーを一度に作る。

方針
- 部品は単体で動かせる粒度（床板・壁の一区画・柱・棚一区画・テレビ・灯具・机など）に分け、個別の FBX にする。
- 部品は Blender の -Y を正面、底面中心を原点として作り、FBX では +Y 上・+Z 正面・メートルになる。
- 同じ部品を並べてコンセプト場面を再構成し、エンジン座標の配置表（layout）と同じ視点の確認画像を出す。
- 画面は無地の発光色で仮表示する。ゲーム内容の確認画像は Runtime キャプチャを別途使用する。
実行: blender -b --factory-startup --python build_factory_set01.py -- [--views V1_menu,V5_wide] [--scale 50] [--samples 16] [--noexport]
"""

import json
import math
import random
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Euler, Matrix, Vector

ROOT = Path(__file__).resolve().parent
EXPORTS, PREVIEWS, TEMP = ROOT / "Exports", ROOT / "Previews", ROOT / "Temp"
FONT_CJK = "C:/Windows/Fonts/Deng.ttf"
FONT_TITLE = "C:/Windows/Fonts/bahnschrift.ttf"
for d in (EXPORTS, PREVIEWS, TEMP):
    d.mkdir(parents=True, exist_ok=True)


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    opts = {"views": None, "scale": 100, "samples": 64, "export": True}
    i = 0
    while i < len(argv):
        key = argv[i].lstrip("-")
        if key == "noexport":
            opts["export"] = False
            i += 1
            continue
        if key == "views":
            opts["views"] = argv[i + 1].split(",")
        elif key in ("scale", "samples"):
            opts[key] = int(argv[i + 1])
        i += 2
    return opts


OPTS = parse_args()
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.filepaths.save_version = 0
SCENE = bpy.context.scene
COLL = {}


def coll(name):
    if name not in COLL:
        c = bpy.data.collections.new(name)
        SCENE.collection.children.link(c)
        COLL[name] = c
    return COLL[name]


# ================================================================ 材質（FBX に色と粗さを載せる。質感の揺らぎは確認用の手続きノード）
def lin(hex_color):
    v = [int(hex_color[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    return tuple(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in v)


MAT = {}


def node(nt, kind, loc=(0, 0)):
    n = nt.nodes.new(kind)
    n.location = loc
    return n


def pbr(name, hex_color, rough, metal=0.0, grunge=0.0, rough_var=None, emission=None, strength=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    m.diffuse_color = (*lin(hex_color), 1)
    bsdf.inputs["Base Color"].default_value = (*lin(hex_color), 1)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    if grunge or rough_var:
        tc = node(nt, "ShaderNodeTexCoord", (-900, 0))
        noise = node(nt, "ShaderNodeTexNoise", (-700, 0))
        noise.inputs["Scale"].default_value = 0.35
        noise.inputs["Detail"].default_value = 6.0
        nt.links.new(tc.outputs["Object"], noise.inputs["Vector"])
        if grunge:
            mr = node(nt, "ShaderNodeMapRange", (-500, 100))
            mr.inputs["To Min"].default_value = 1.0 - grunge
            mr.inputs["To Max"].default_value = 1.0 + grunge * 0.6
            nt.links.new(noise.outputs["Fac"], mr.inputs["Value"])
            mul = node(nt, "ShaderNodeVectorMath", (-300, 100))
            mul.operation = "MULTIPLY"
            mul.inputs[0].default_value = lin(hex_color)
            nt.links.new(mr.outputs["Result"], mul.inputs[1])
            nt.links.new(mul.outputs["Vector"], bsdf.inputs["Base Color"])
        if rough_var:
            # 床の水たまり：ノイズの谷だけ粗さを下げて、画面と灯具を映す。
            n2 = node(nt, "ShaderNodeTexNoise", (-700, -250))
            n2.inputs["Scale"].default_value = 0.18
            n2.inputs["Detail"].default_value = 3.0
            nt.links.new(tc.outputs["Object"], n2.inputs["Vector"])
            mr2 = node(nt, "ShaderNodeMapRange", (-500, -250))
            mr2.inputs["From Min"].default_value = 0.38
            mr2.inputs["From Max"].default_value = 0.62
            mr2.inputs["To Min"].default_value = rough_var[0]
            mr2.inputs["To Max"].default_value = rough_var[1]
            nt.links.new(n2.outputs["Fac"], mr2.inputs["Value"])
            nt.links.new(mr2.outputs["Result"], bsdf.inputs["Roughness"])
    if emission:
        bsdf.inputs["Emission Color"].default_value = (*emission, 1)
        bsdf.inputs["Emission Strength"].default_value = strength
    MAT[name] = m
    return m


# 名前はエンジン側で共通材質へ差し替えるときの手掛かりになる。
# 床は暗く半光沢にして、光の島と画面を鈍く映す（明るい哑光の灰だと霧と同化して絵が平らになる）。
pbr("TD2F_Floor", "363431", 0.38, grunge=0.25, rough_var=(0.1, 0.55))
pbr("TD2F_WallLower", "3D3A37", 0.82, grunge=0.3)
pbr("TD2F_WallUpper", "57534E", 0.88, grunge=0.3)
# 作業台の天板：暗いリノリウム張り。低い粗さで画面の青緑を手前に映し、游戏画面の下辺に光の帯を作る。
pbr("TD2F_BenchTop", "2B2825", 0.28)
pbr("TD2F_Steel", "2C2D2F", 0.5, grunge=0.2)
pbr("TD2F_BareMetal", "8A8A88", 0.42, metal=1.0)
pbr("TD2F_RackPaint", "5A5246", 0.6, grunge=0.15)
pbr("TD2F_Wood", "5B4331", 0.58, grunge=0.15)
pbr("TD2F_PlasticBeige", "A9A08F", 0.5)
pbr("TD2F_PlasticDark", "2A2826", 0.45)
pbr("TD2F_Fabric", "24221F", 0.95)
pbr("TD2F_Roof", "1A1918", 0.92)
pbr("TD2F_Rubber", "141313", 0.7)
pbr("TD2F_Paper", "D8D2C4", 0.9)
pbr("TD2F_Sticky", "D9CF9A", 0.85)
pbr("TD2F_Cork", "6E5E4C", 0.92, grunge=0.2)
pbr("TD2F_Cardboard", "6F6250", 0.9, grunge=0.2)
pbr("TD2F_CapFabric", "34403F", 0.8)
pbr("TD2F_Ceramic", "BDB6A8", 0.35)
pbr("TD2_Screen", "0D0F10", 0.07)  # 既存の画面表示コードが探すスロット名
pbr("TD2F_LedRed", "C8463C", 0.4, emission=lin("C8463C"), strength=6.0)
pbr("TD2F_Bulb", "FFFFFF", 0.3, emission=(1.0, 0.95, 0.88), strength=30.0)
pbr("TD2F_BulbWarm", "FFFFFF", 0.3, emission=(1.0, 0.72, 0.42), strength=30.0)


def window_glass():
    """外の雨夜を映す窓。縦に伸ばしたノイズで雨筋状の明暗を作る（確認用。FBX には基本色のみ）。"""
    m = pbr("TD2F_WindowGlass", "06070A", 0.12)
    nt = m.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    tc = node(nt, "ShaderNodeTexCoord", (-900, 0))
    sc = node(nt, "ShaderNodeVectorMath", (-700, 0))
    sc.operation = "MULTIPLY"
    sc.inputs[1].default_value = (26.0, 26.0, 1.2)
    nt.links.new(tc.outputs["Object"], sc.inputs[0])
    noise = node(nt, "ShaderNodeTexNoise", (-500, 0))
    noise.inputs["Detail"].default_value = 4.0
    nt.links.new(sc.outputs["Vector"], noise.inputs["Vector"])
    mr = node(nt, "ShaderNodeMapRange", (-300, 0))
    mr.inputs["To Min"].default_value = 0.18
    mr.inputs["To Max"].default_value = 0.7
    nt.links.new(noise.outputs["Fac"], mr.inputs["Value"])
    bsdf.inputs["Emission Color"].default_value = (0.13, 0.17, 0.24, 1)
    # FBX は接続を無視して既定値を書くので、エンジン側の冷たい窓の明るさをここで決める。
    bsdf.inputs["Emission Strength"].default_value = 2.2
    nt.links.new(mr.outputs["Result"], bsdf.inputs["Emission Strength"])


window_glass()


def static_glow_material():
    """待機中のテレビの砂嵐。小さなノイズ画像を発光色に使い、FBX に埋め込んでエンジンへ渡す。"""
    rng = np.random.default_rng(5)
    w, h = 128, 96
    v = rng.uniform(0.08, 1.0, (h, w)).astype(np.float32)
    v *= (0.92 + 0.08 * np.sin(np.arange(h) * np.pi * 0.5))[:, None]  # 走査線をわずかに
    rgba = np.stack([v * 0.82, v * 0.9, v * 0.95, np.ones_like(v)], axis=-1)
    img = bpy.data.images.new("td2f_static_noise", width=w, height=h, alpha=False)
    img.pixels.foreach_set(rgba.ravel())
    img.filepath_raw = str(EXPORTS / "td2f_static_noise.png")
    img.file_format = "PNG"
    img.save()
    # FBX に埋め込んだ画像はエンジンの取り込みで展開されず、光追の場面から「未解決の面」として外れて
    # 場面全体の GI と光追反射が止まる。待機光は一様な冷たい灰の発光にする（砂嵐の動きは後で専用材質で）。
    m = pbr("TD2F_StaticGlow", "000000", 0.1)
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Emission Color"].default_value = (0.55, 0.62, 0.7, 1)
    bsdf.inputs["Emission Strength"].default_value = 0.45


static_glow_material()
# 床の水たまり：暗く滑らかな薄い面で、吊り灯の光と画面を映す。
pbr("TD2F_Puddle", "3C3A37", 0.06)  # 床よりわずかに暗い濡れ色。映り込みが無い所でも黒い穴にしない


# ================================================================ 画面の仮表示
# 廃止されたスプライト画像や過去の区域データは読み込まない。モデルと画面内ゲームを分離する。
# 完成した画面の比較には、後で Runtime のキャプチャを割り当てる。
SCREEN_BG = np.array([0.70, 0.86, 0.80], np.float32)
SCREEN_COLOR = tuple(float(v) for v in SCREEN_BG ** 2.2)
LEVELS = {key: (None, SCREEN_COLOR) for key in ("B", "TV2", "A", "menu")}
SCREEN_STRENGTH = 1.9


def screen_material(name):
    """外部画像なしの仮表示。既存の照明用平均色との契約だけを維持する。"""
    m = pbr(name, "000000", 0.08)
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Emission Color"].default_value = (*SCREEN_COLOR, 1)
    bsdf.inputs["Emission Strength"].default_value = SCREEN_STRENGTH


def static_material(name, strength):
    """砂嵐：UV を荒く量子化したホワイトノイズ。物体ごとの乱数で模様をずらす。"""
    m = pbr(name, "000000", 0.08)
    nt = m.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    tc = node(nt, "ShaderNodeTexCoord", (-1100, 0))
    mul = node(nt, "ShaderNodeVectorMath", (-900, 0))
    mul.operation = "MULTIPLY"
    mul.inputs[1].default_value = (120.0, 90.0, 1.0)
    nt.links.new(tc.outputs["UV"], mul.inputs[0])
    snap = node(nt, "ShaderNodeVectorMath", (-700, 0))
    snap.operation = "SNAP"
    snap.inputs[1].default_value = (1.0, 1.0, 1.0)
    nt.links.new(mul.outputs["Vector"], snap.inputs[0])
    info = node(nt, "ShaderNodeObjectInfo", (-900, -200))
    rmul = node(nt, "ShaderNodeMath", (-700, -200))
    rmul.operation = "MULTIPLY"
    rmul.inputs[1].default_value = 977.0
    nt.links.new(info.outputs["Random"], rmul.inputs[0])
    add = node(nt, "ShaderNodeVectorMath", (-500, 0))
    add.operation = "ADD"
    nt.links.new(snap.outputs["Vector"], add.inputs[0])
    nt.links.new(rmul.outputs["Value"], add.inputs[1])
    wn = node(nt, "ShaderNodeTexWhiteNoise", (-300, 0))
    wn.noise_dimensions = "3D"
    nt.links.new(add.outputs["Vector"], wn.inputs["Vector"])
    mr = node(nt, "ShaderNodeMapRange", (-100, 0))
    mr.inputs["To Min"].default_value = 0.08
    nt.links.new(wn.outputs["Value"], mr.inputs["Value"])
    tint = node(nt, "ShaderNodeVectorMath", (100, 0))
    tint.operation = "MULTIPLY"
    tint.inputs[1].default_value = (0.82, 0.9, 0.95)
    nt.links.new(mr.outputs["Result"], tint.inputs[0])
    nt.links.new(tint.outputs["Vector"], bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = strength


for key in LEVELS:
    screen_material("scr_" + key)
static_material("static", 1.6)
static_material("static_dim", 0.16)
pbr("bulb_off", "8A8A88", 0.3)


# ================================================================ 形状の組み立て
class Builder:
    """箱・梁・円柱を一つの bmesh に溜め、材質スロット付きの一物体にする。UV は 1 m = 1 の箱投影。"""

    def __init__(self, name):
        self.name, self.bm, self.mats = name, bmesh.new(), []

    def _tag(self, verts, mat):
        if mat not in self.mats:
            self.mats.append(mat)
        idx = self.mats.index(mat)
        faces = set()
        for v in verts:
            faces.update(v.link_faces)
        for f in faces:
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
        rot = Matrix((x, y, z)).transposed().to_4x4()
        m = Matrix.Translation((p1 + p2) / 2) @ rot @ Matrix.Diagonal((d.length, w, h, 1))
        self._tag(bmesh.ops.create_cube(self.bm, size=1.0, matrix=m)["verts"], mat)

    def cyl(self, center, r1, depth, mat, r2=None, segs=20, rot=(0, 0, 0), cap=True):
        m = Matrix.Translation(center) @ Euler(rot).to_matrix().to_4x4()
        res = bmesh.ops.create_cone(self.bm, cap_ends=cap, cap_tris=False, segments=segs,
                                    radius1=r1, radius2=r1 if r2 is None else r2, depth=depth, matrix=m)
        self._tag(res["verts"], mat)

    def shade(self, center, r1, depth, mat, r2, segs=24, rot=(0, 0, 0), wall=0.006):
        """口の開いた笠：外壁と、法線を内向きにした内壁の二重にして裏面が抜けないようにする。"""
        self.cyl(center, r1, depth, mat, r2=r2, segs=segs, rot=rot, cap=False)
        m = Matrix.Translation(center) @ Euler(rot).to_matrix().to_4x4()
        res = bmesh.ops.create_cone(self.bm, cap_ends=False, cap_tris=False, segments=segs, radius1=r1 - wall,
                                    radius2=max(0.001, r2 - wall), depth=depth, matrix=m)
        faces = list({f for v in res["verts"] for f in v.link_faces})
        bmesh.ops.reverse_faces(self.bm, faces=faces)
        self._tag(res["verts"], mat)

    def sphere(self, center, radius, mat, scale=(1, 1, 1)):
        m = Matrix.Translation(center) @ Matrix.Diagonal((*scale, 1))
        self._tag(bmesh.ops.create_uvsphere(self.bm, u_segments=16, v_segments=10, radius=radius, matrix=m)["verts"], mat)

    def poly(self, pts, mat):
        """任意の凸でない多角形も一枚の面として作る（水たまりの輪郭など）。"""
        vs = [self.bm.verts.new(p) for p in pts]
        self.bm.faces.new(vs)
        self._tag(vs, mat)

    def quad(self, pts, mat):
        vs = [self.bm.verts.new(p) for p in pts]
        self.bm.faces.new(vs)
        self._tag(vs, mat)

    def bottom_center(self):
        xs = [v.co.x for v in self.bm.verts]
        ys = [v.co.y for v in self.bm.verts]
        return Vector(((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, min(v.co.z for v in self.bm.verts)))

    def build(self, bevel=0.0, origin=None, keep_uv=False):
        """origin を与えると頂点をずらし、その点を物体原点にする（世界座標で作った一品物用）。"""
        bm = self.bm
        if origin is not None:
            bmesh.ops.translate(bm, vec=-Vector(origin), verts=bm.verts)
        bm.normal_update()
        if not keep_uv:
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


# ================================================================ 部品登録と配置
PART = {}
LAYOUT = []
EXPORT_COLL = coll("_Export")


def part(name, objs, info=None):
    """objs[0] が本体。残り（画面・電球）は同じ原点を共有する付属メッシュ。"""
    for i, o in enumerate(objs):
        o.name = name + ("__Body" if i == 0 else "__" + o.get("role", f"Part{i}"))
        o.data.name = o.name
    PART[name] = {"objs": objs, "info": info or {}}


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


LIT = {"A", "B", "TV2", "menu"}
SCREENS, BULBS = [], []


def inst(name, loc, yaw=0.0, scale=1.0, rot=None, group="Set", screen=None, bulb=True, label=None, extra=None):
    """部品の複製を置き、エンジン座標の配置記録を残す。rot は Blender のオイラー（ラジアン）。"""
    p = PART[name]
    objs = []
    for i, src in enumerate(p["objs"]):
        o = src.copy()
        coll(group).objects.link(o)
        objs.append(o)
        if i > 0:
            o.parent = objs[0]
    body = objs[0]
    body.location = loc
    body.rotation_euler = rot if rot else (0, 0, math.radians(yaw))
    body.scale = scale if isinstance(scale, tuple) else (scale, scale, scale)
    rec = {"part": name, "label": label or name, "group": group}
    if p["info"].get("cast_shadow") is False:
        rec["cast_shadow"] = False
        body.visible_shadow = False
    rec.update(extra or {})
    LAYOUT.append((rec, body))
    for o in objs[1:]:
        role = o.get("role")
        if role == "Screen":
            o.material_slots[0].link = "OBJECT"
            SCREENS.append([o, screen or "off", None])
            rec["screen"] = screen or "off"
            if screen == "static_dim" and name.startswith("SM_F_TV_"):
                # エンジンでは画面材質を個別に差し替えないので、砂嵐の発光片を同じ変換で重ねる（確認用描画には出さない）。
                glow = name.replace("SM_F_TV_", "SM_F_ScreenGlow_")
                LAYOUT.append(({"part": glow, "label": glow, "group": group, "cast_shadow": False}, body))
            if screen in LIT:
                spec = p["info"]["screen"]
                ld = bpy.data.lights.new("Spill", "AREA")
                ld.shape, ld.size, ld.size_y = "RECTANGLE", spec["w"], spec["h"]
                s = body.scale.x
                ld.energy = 28.0 * spec["w"] * spec["h"] / 0.36 * s * s
                ld.color = LEVELS[screen][1]
                ld.use_shadow = False
                ld.volume_factor = 0.35
                lo = bpy.data.objects.new("Spill", ld)
                coll(group).objects.link(lo)
                lo.parent = body
                lo.location = (spec["cx"], spec["y"] - 0.04, spec["bottom"] + spec["h"] / 2)
                lo.rotation_euler = (math.radians(-90), 0, 0)
                SCREENS[-1][2] = lo
        elif role == "Bulb":
            o.material_slots[0].link = "OBJECT"
            o.material_slots[0].material = MAT["TD2F_Bulb" if bulb else "bulb_off"]
            o.visible_shadow = False
            if bulb:
                BULBS.append(o)
    return body


# ================================================================ 部品：建屋
def make_hall_parts():
    b = Builder("slab")
    # 床板は隙間なく重ね、面取りもしない。細い面取りが月光を鏡面反射して、継ぎ目が光の点線として瞬いたため。
    # 目地は材質の揺らぎで表す。
    b.box((0, 0, 0.15), (6.02, 6.02, 0.3), "TD2F_Floor")  # 隣と 1 cm 重ねて、ラスタライズの割れ目を塞ぐ
    part("SM_F_FloorSlab_6m", [b.build()])

    def wall_bay(name, z_lo, z_win, rows, seed):
        """壁の一区画（幅 6 m・高さ 12 m・厚さ 0.3 m）。正面（-Y）が室内。窓ガラスは数枚欠ける。"""
        rng = random.Random(seed)
        w = Builder(name)
        g = Builder(name + "_glass")  # 月光を通すため、ガラスは影を落とさない別部品にする
        w.box((0, 0, 1.75), (6.0, 0.3, 3.5), "TD2F_WallLower")
        if z_win > 3.5:
            w.box((0, 0, (3.5 + z_win) / 2), (6.0, 0.3, z_win - 3.5), "TD2F_WallUpper")
        w.box((0, 0, 11.25), (6.0, 0.3, 1.5), "TD2F_WallUpper")
        zs = [z_win + (10.5 - z_win) * k / rows for k in range(rows + 1)]
        for z in zs:
            w.box((0, 0, z), (6.0, 0.14, 0.09), "TD2F_Steel")
        for k in range(9):
            w.box((-3.0 + 0.75 * k, 0, (z_win + 10.5) / 2), (0.07, 0.14, 10.5 - z_win), "TD2F_Steel")
        for k in range(8):
            for r in range(rows):
                if rng.random() < 0.09:
                    continue
                a0, a1 = -3.0 + 0.75 * k + 0.035, -3.0 + 0.75 * (k + 1) - 0.035
                z0, z1 = zs[r] + 0.045, zs[r + 1] - 0.045
                g.quad([(a0, 0.0, z0), (a1, 0.0, z0), (a1, 0.0, z1), (a0, 0.0, z1)], "TD2F_WindowGlass")
        part(name, [w.build(bevel=0.01)])
        part(name.replace("SM_F_WallBay_", "SM_F_WindowGlass_"), [g.build()], {"cast_shadow": False})

    wall_bay("SM_F_WallBay_Side_A", 3.5, 7.0, 2, 1)
    wall_bay("SM_F_WallBay_Side_B", 3.5, 7.0, 2, 2)
    wall_bay("SM_F_WallBay_End", 3.5, 4.5, 3, 3)

    c = Builder("column")
    for x in (-0.2, 0.2):
        c.box((x, 0, 6.0), (0.04, 0.45, 12.0), "TD2F_Steel")
    c.box((0, 0, 6.0), (0.4, 0.03, 12.0), "TD2F_Steel")
    c.box((0, -0.35, 8.45), (0.42, 0.55, 0.45), "TD2F_Steel")  # 室内側（正面）へ張り出すクレーン受け
    part("SM_F_Column_H12", [c.build(bevel=0.01)])

    r = Builder("runway")
    r.box((0, 0, 0.35), (0.36, 6.0, 0.7), "TD2F_Steel")
    part("SM_F_RunwayBeam_6m", [r.build(bevel=0.01)])

    t = Builder("truss")

    def top(x):
        return 0.15 + (1 - abs(x) / 14.6) * 1.6

    t.beam((-14.6, 0, 0.0), (14.6, 0, 0.0), 0.12, 0.16, "TD2F_Steel")
    t.beam((-14.6, 0, top(-14.6)), (0, 0, top(0)), 0.12, 0.16, "TD2F_Steel")
    t.beam((0, 0, top(0)), (14.6, 0, top(14.6)), 0.12, 0.16, "TD2F_Steel")
    xs = [-14.6 + 2.92 * k for k in range(11)]
    for k, x in enumerate(xs[1:-1], start=1):
        t.beam((x, 0, 0.0), (x, 0, top(x)), 0.08, 0.1, "TD2F_Steel")
        nx = xs[k + 1] if x < 0 else xs[k - 1]
        t.beam((x, 0, 0.0), (nx, 0, top(nx)), 0.07, 0.09, "TD2F_Steel")
    part("SM_F_RoofTruss_30m", [t.build(bevel=0.005, origin=(0, 0, -0.08))])

    rb = Builder("roofbay")
    ang = math.atan2(1.6, 14.6)
    for side in (-1, 1):
        rb.box((side * 7.4, 0, 1.0), (15.2, 6.1, 0.1), "TD2F_Roof", rot=(0, side * -ang, 0))
    for x in xs:
        rb.box((x, 0, top(x) + 0.14 - 0.15), (0.1, 6.0, 0.14), "TD2F_Steel")
    part("SM_F_RoofBay_6m", [rb.build(origin=(0, 0, -0.07))])


# ================================================================ 部品：テレビ（正面 -Y・底面中心が原点）
TV = {}


def make_tv_parts():
    def proto(kind, fn, screen, height, width, depth):
        b = Builder(kind)
        fn(b)
        w, h, cx, bottom, y = screen
        name = "SM_F_TV_" + kind
        part(name, [b.build(bevel=0.008), screen_mesh(name + "_scr", w, h, cx, bottom, y)],
             {"screen": {"w": w, "h": h, "cx": cx, "bottom": bottom, "y": y}})
        TV[kind] = {"part": name, "h": height, "w": width, "d": depth}
        # 待機テレビ用の砂嵐の発光片：同じ原点で画面のわずかに手前に重ねる。影は落とさない。
        glow = screen_mesh("SM_F_ScreenGlow_" + kind + "_q", w, h, cx, bottom, y - 0.004)
        glow.data.materials[0] = MAT["TD2F_StaticGlow"]
        part("SM_F_ScreenGlow_" + kind, [glow], {"cast_shadow": False})

    def portable(b):
        b.box((0, 0, 0.015), (0.52, 0.40, 0.03), "TD2F_PlasticDark")
        b.box((0, 0, 0.26), (0.62, 0.44, 0.46), "TD2F_PlasticBeige")
        b.box((0, 0.27, 0.26), (0.48, 0.12, 0.34), "TD2F_PlasticBeige")
        b.box((0, -0.225, 0.26), (0.60, 0.02, 0.44), "TD2F_PlasticDark")
        for z in (0.36, 0.28):
            b.cyl((0.22, -0.24, z), 0.02, 0.02, "TD2F_BareMetal", rot=(math.pi / 2, 0, 0))
        for z in (0.13, 0.15, 0.17, 0.19):
            b.box((0.22, -0.237, z), (0.07, 0.004, 0.008), "TD2F_Fabric")
        for x in (-0.16, 0.16):
            b.box((x, 0, 0.53), (0.03, 0.04, 0.08), "TD2F_PlasticDark")
        b.box((0, 0, 0.585), (0.36, 0.04, 0.035), "TD2F_PlasticDark")

    def console(b):
        for x in (-0.46, 0.46):
            for y in (-0.18, 0.18):
                b.box((x, y, 0.11), (0.045, 0.045, 0.22), "TD2F_Wood")
        b.box((0, 0, 0.60), (1.04, 0.48, 0.76), "TD2F_Wood")
        b.box((-0.15, -0.245, 0.62), (0.66, 0.02, 0.56), "TD2F_PlasticDark")
        b.box((0.33, -0.246, 0.56), (0.28, 0.02, 0.42), "TD2F_Fabric")
        for x in (0.27, 0.39):
            b.cyl((x, -0.26, 0.86), 0.022, 0.02, "TD2F_BareMetal", rot=(math.pi / 2, 0, 0))

    def broadcast(b):
        b.box((0, 0, 0.23), (0.56, 0.5, 0.46), "TD2F_Steel")
        for x in (-0.3, 0.3):
            b.box((x, -0.245, 0.23), (0.05, 0.02, 0.44), "TD2F_Steel")
        b.box((0, -0.255, 0.23), (0.54, 0.02, 0.44), "TD2F_PlasticDark")
        for k in range(5):
            b.box((0.22, -0.268, 0.12 + 0.05 * k), (0.03, 0.01, 0.022), "TD2F_BareMetal")
        b.box((0.22, -0.268, 0.40), (0.012, 0.006, 0.012), "TD2F_LedRed")
        b.box((0.10, -0.267, 0.045), (0.14, 0.003, 0.02), "TD2F_Paper")

    def wide(b):
        for x in (-0.42, 0.42):
            b.box((x, 0, 0.04), (0.14, 0.5, 0.08), "TD2F_PlasticDark")
        b.box((0, 0, 0.41), (1.2, 0.52, 0.66), "TD2F_PlasticDark")
        b.box((0, -0.265, 0.41), (1.16, 0.02, 0.62), "TD2F_PlasticBeige")
        b.box((-0.08, -0.272, 0.42), (0.88, 0.02, 0.52), "TD2F_PlasticDark")
        for k in range(6):
            b.box((0.47, -0.277, 0.28 + 0.05 * k), (0.12, 0.006, 0.012), "TD2F_Fabric")

    def portrait(b):
        b.box((0, 0, 0.02), (0.62, 0.34, 0.04), "TD2F_Steel")
        b.box((0, 0, 0.54), (0.58, 0.3, 1.0), "TD2F_Steel")
        b.box((0, -0.155, 0.54), (0.56, 0.02, 0.96), "TD2F_PlasticDark")

    def desk_crt(b):
        b.cyl((0, 0.02, 0.01), 0.12, 0.02, "TD2F_PlasticBeige")
        b.box((0, 0.02, 0.045), (0.08, 0.08, 0.05), "TD2F_PlasticBeige")
        b.box((0, 0, 0.25), (0.40, 0.36, 0.36), "TD2F_PlasticBeige")
        b.box((0, 0.24, 0.25), (0.30, 0.16, 0.28), "TD2F_PlasticBeige")
        b.box((0, -0.185, 0.25), (0.39, 0.015, 0.35), "TD2F_PlasticBeige")
        b.box((0.16, -0.194, 0.085), (0.01, 0.004, 0.01), "TD2F_LedRed")
        b.box((-0.17, -0.194, 0.37), (0.05, 0.003, 0.05), "TD2F_Sticky", rot=(0, 0.12, 0))

    proto("Portable", portable, (0.40, 0.30, -0.05, 0.11, -0.2365), 0.60, 0.62, 0.60)
    proto("Console", console, (0.56, 0.42, -0.15, 0.41, -0.2565), 0.98, 1.04, 0.48)
    proto("Broadcast", broadcast, (0.40, 0.30, -0.04, 0.08, -0.2665), 0.46, 0.60, 0.50)
    proto("Wide", wide, (0.80, 0.45, -0.08, 0.195, -0.2825), 0.74, 1.20, 0.55)
    proto("Portrait", portrait, (0.45, 0.80, 0.0, 0.14, -0.1665), 1.04, 0.62, 0.34)
    proto("DeskCRT", desk_crt, (0.30, 0.225, 0.0, 0.1375, -0.1935), 0.43, 0.40, 0.52)


# ================================================================ 部品：産業小道具・灯具
RACK_LEVELS = [0.15, 1.35, 2.55, 3.75, 4.95]
BAY, DEPTH, RACK_H = 2.4, 1.0, 6.0


def make_prop_parts():
    def upright(b, x):
        for v in (-DEPTH / 2, DEPTH / 2):
            b.box((x, v, RACK_H / 2), (0.08, 0.07, RACK_H), "TD2F_Steel")
        for z in (0.3, 3.0, 5.7):
            b.beam((x, -DEPTH / 2, z), (x, DEPTH / 2, z), 0.04, 0.04, "TD2F_Steel")
        zs = [0.3, 1.65, 3.0, 4.35, 5.7]
        for i in range(len(zs) - 1):
            v1, v2 = (-DEPTH / 2, DEPTH / 2) if i % 2 == 0 else (DEPTH / 2, -DEPTH / 2)
            b.beam((x, v1, zs[i]), (x, v2, zs[i + 1]), 0.035, 0.035, "TD2F_Steel")

    b = Builder("rackbay")
    upright(b, -BAY / 2)
    for z in RACK_LEVELS:
        for v in (-DEPTH / 2, DEPTH / 2):
            b.box((0, v, z), (BAY - 0.08, 0.05, 0.12), "TD2F_RackPaint")
        if z > 0.2:
            b.box((0, 0, z + 0.07), (BAY - 0.1, DEPTH, 0.02), "TD2F_Steel")
    part("SM_F_Rack_Bay", [b.build(bevel=0.006, origin=(0, 0, 0))])
    b = Builder("rackend")
    upright(b, 0.0)
    part("SM_F_Rack_EndFrame", [b.build(bevel=0.006)])

    b = Builder("pallet")
    for v in (-0.45, 0.0, 0.45):
        b.box((0, v, 0.05), (1.2, 0.1, 0.1), "TD2F_Wood")
    for u in [-0.55 + 0.18 * k for k in range(7)]:
        b.box((u, 0, 0.1125), (0.12, 1.0, 0.025), "TD2F_Wood")
    part("SM_F_Pallet", [b.build(bevel=0.004)])

    for name, size in (("SM_F_Box_A", (0.6, 0.5, 0.35)), ("SM_F_Box_B", (0.45, 0.4, 0.3)), ("SM_F_Box_C", (0.7, 0.55, 0.4))):
        b = Builder(name)
        b.box((0, 0, size[2] / 2), size, "TD2F_Cardboard")
        b.box((0, -size[1] / 2 - 0.001, size[2] * 0.62), (size[0] * 0.98, 0.002, 0.05), "TD2F_Cardboard")
        part(name, [b.build(bevel=0.01)])

    b = Builder("crates")
    b.box((0, 0, 0.35), (1.0, 0.8, 0.7), "TD2F_Wood")
    b.box((0.1, 0.05, 1.0), (0.8, 0.7, 0.6), "TD2F_Wood", rot=(0, 0, 0.2))
    part("SM_F_Crate_Stack", [b.build(bevel=0.01)])

    b = Builder("reel")
    for y in (-0.3, 0.3):
        b.cyl((0, y, 0.5), 0.5, 0.05, "TD2F_Wood", rot=(math.pi / 2, 0, 0), segs=28)
    b.cyl((0, 0, 0.5), 0.22, 0.55, "TD2F_Rubber", rot=(math.pi / 2, 0, 0), segs=24)
    part("SM_F_CableReel", [b.build(bevel=0.006)])

    # 試験用の長い作業台：遊べる区域のテレビを世界地図の並びどおりに接して載せる。
    b = Builder("bench")
    b.box((0, 0, 0.88), (3.6, 0.9, 0.05), "TD2F_BenchTop")
    for x in (-1.75, 1.75):
        for y in (-0.4, 0.4):
            b.box((x, y, 0.43), (0.06, 0.06, 0.86), "TD2F_Steel")
        b.box((x, 0, 0.12), (0.06, 0.8, 0.05), "TD2F_Steel")
    b.box((0, 0.4, 0.12), (3.5, 0.05, 0.05), "TD2F_Steel")
    b.box((0, 0, 0.14), (3.4, 0.8, 0.02), "TD2F_Steel")
    part("SM_F_Workbench_3m6", [b.build(bevel=0.006)])
    b = Builder("riser")
    b.box((0, 0, 0.0175), (0.9, 0.5, 0.035), "TD2F_Wood")
    part("SM_F_Riser_Plank", [b.build(bevel=0.004)])

    for name, radius, seed in (("SM_F_Puddle_A", 1.4, 3), ("SM_F_Puddle_B", 0.9, 8), ("SM_F_Puddle_C", 2.2, 13)):
        r = random.Random(seed)
        n = 18
        pts = []
        for k in range(n):
            a = k * 2 * math.pi / n
            rr = radius * (0.65 + 0.35 * r.random()) * (1.0 + 0.25 * math.sin(a * 2 + seed))
            pts.append((rr * math.cos(a) * 1.3, rr * math.sin(a), 0.0))
        b = Builder(name)
        b.poly(pts, "TD2F_Puddle")
        part(name, [b.build()], {"cast_shadow": False})

    b = Builder("chain")
    b.beam((0, 0, 0), (0, 0, 1.0), 0.02, 0.02, "TD2F_BareMetal")
    part("SM_F_Chain_1m", [b.build()])

    # 工業用ペンダント灯：笠の下端が原点。電球は影を落とさない別メッシュ。
    b = Builder("pendant")
    b.beam((0, 0, 0.3), (0, 0, 4.2), 0.012, 0.012, "TD2F_Rubber")
    b.shade((0, 0, 0.16), 0.40, 0.32, "TD2F_Steel", r2=0.08)
    b.cyl((0, 0, 0.34), 0.08, 0.06, "TD2F_Steel")
    bulb = Builder("pendant_bulb")
    bulb.sphere((0, 0, 0.05), 0.075, "TD2F_Bulb")
    bo = bulb.build()
    bo["role"] = "Bulb"
    part("SM_F_Lamp_Pendant", [b.build(), bo])

    # 天車：桁と端の台車（原点は桁中心の下端）と、移動台車＋吊り梁（原点は吊り梁の下端）。
    b = Builder("crane")
    for y in (-0.9, 0.9):
        b.box((0, y, 0.55), (28.4, 0.5, 0.9), "TD2F_RackPaint")
    for x in (-14.15, 14.15):
        b.box((x, 0, 0.3), (0.6, 3.2, 0.6), "TD2F_RackPaint")
    part("SM_F_Crane_Bridge", [b.build(bevel=0.01)])
    b = Builder("hoist")
    b.box((0, 0, 4.05), (2.2, 2.6, 0.75), "TD2F_Steel")
    b.box((0, 0, 0.1), (4.6, 0.22, 0.2), "TD2F_Steel")
    for dx in (-2.1, 2.1):
        b.beam((dx, 0, 0.2), (dx * 0.35, 0, 3.7), 0.035, 0.035, "TD2F_BareMetal")
    part("SM_F_Crane_Hoist", [b.build(bevel=0.01)])


# ================================================================ 配置：建屋・棚・塔・天車・階段・灯
X0, X1, Y0, Y1, H = -15.0, 15.0, -6.0, 24.0, 12.0
rng = random.Random(11)


def wall(variant, loc, yaw):
    inst("SM_F_WallBay_" + variant, loc, yaw, group="Arch")
    inst("SM_F_WindowGlass_" + variant, loc, yaw, group="Arch")


def place_hall():
    for i in range(5):
        for j in range(5):
            inst("SM_F_FloorSlab_6m", (X0 + 3 + 6 * i, Y0 + 3 + 6 * j, -0.3), group="Arch")
    for j in range(5):
        yc = Y0 + 3 + 6 * j
        wall("Side_" + "AB"[j % 2], (X0 - 0.15, yc, 0), 90)
        wall("Side_" + "BA"[j % 2], (X1 + 0.15, yc, 0), -90)
    for i in range(5):
        xc = X0 + 3 + 6 * i
        wall("End", (xc, Y1 + 0.15, 0), 0)
        wall("Side_" + "AB"[i % 2], (xc, Y0 - 0.15, 0), 180)
    for side, yaw in ((-1, 90), (1, -90)):
        for y in (-6, 0, 6, 12, 18, 24):
            inst("SM_F_Column_H12", (side * 14.6, y, 0), yaw, group="Arch")
        for j in range(5):
            inst("SM_F_RunwayBeam_6m", (side * 14.2, Y0 + 3 + 6 * j, 8.6), group="Arch")
    for y in (0, 6, 12, 18):
        inst("SM_F_RoofTruss_30m", (0, y, 10.92), group="Arch")
    for j in range(5):
        inst("SM_F_RoofBay_6m", (0, Y0 + 3 + 6 * j, 11.13), group="Arch")


def tv(kind, loc, yaw=0.0, scale=1.0, screen="off", rot=None, group="TVs", extra=None):
    return inst(TV[kind]["part"], loc, yaw, scale, rot, group, screen=screen, extra=extra)


def rack_row(origin, yaw, bays, group="Racks"):
    """棚の列。局所 u が列方向、v=-0.5 が正面。棚板上面の世界座標を返す関数を返す。"""
    base = Matrix.Translation(origin) @ Matrix.Rotation(math.radians(yaw), 4, "Z")

    def w(u, v, z):
        return tuple(base @ Vector((u, v, z)))

    for k in range(bays):
        inst("SM_F_Rack_Bay", w(k * BAY + BAY / 2, 0, 0), yaw, group=group)
    inst("SM_F_Rack_EndFrame", w(bays * BAY, 0, 0), yaw, group=group)
    return w


def boxes(w, u, z, yaw, r):
    hh = 0.0
    for _ in range(r.randint(1, 3)):
        name = r.choice(["SM_F_Box_A", "SM_F_Box_B", "SM_F_Box_C"])
        height = {"SM_F_Box_A": 0.35, "SM_F_Box_B": 0.3, "SM_F_Box_C": 0.4}[name]
        if hh + height > 1.0:
            break
        inst(name, w(u + r.uniform(-0.1, 0.1), r.uniform(-0.1, 0.05), z + hh), yaw + r.uniform(-8, 8), group="Props")
        hh += height


def fill_rack(w, yaw, bays, r, manual=None):
    manual = manual or {}
    for k in range(bays):
        for li, z in enumerate(RACK_LEVELS):
            ztop = z + 0.08 if li > 0 else 0.0
            if (k, li) in manual:
                for kind, u, state, sc in manual[(k, li)]:
                    d = TV[kind]["d"] * sc
                    tv(kind, w(k * BAY + u, -DEPTH / 2 + 0.05 + d / 2, ztop), yaw, sc, state, group="Racks")
                continue
            roll = r.random()
            if roll < 0.55:
                u = 0.15
                while u < BAY - 0.3:
                    kind = r.choice(["Portable", "Broadcast", "Wide", "Portable", "Console", "Broadcast"])
                    sc = r.uniform(0.85, 1.1)
                    width = TV[kind]["w"] * sc
                    if u + width > BAY - 0.1 or TV[kind]["h"] * sc > 1.05:
                        break
                    state = "static_dim" if r.random() < 0.12 else "off"
                    d = TV[kind]["d"] * sc
                    tv(kind, w(k * BAY + u + width / 2, -DEPTH / 2 + 0.06 + d / 2, ztop), yaw + r.uniform(-5, 5),
                       sc, state, group="Racks")
                    u += width + r.uniform(0.02, 0.25)
            elif roll < 0.78:
                boxes(w, k * BAY + r.uniform(0.6, 1.8), ztop, yaw, r)


def place_racks_and_towers():
    r = random.Random(11)
    # 主棚：横長 A と携帯型 B が接し、縦長 C が A の上に載る（隣接＝連結の見本）。
    row_a = rack_row((-11.0, 6.0, 0), 0, 4)
    fill_rack(row_a, 0, 4, r, manual={
        (1, 1): [("Console", 0.9, "off", 1.0)],
        (2, 1): [("Portrait", 0.6, "off", 0.98), ("Broadcast", 1.5, "static_dim", 1.0)],
        (1, 2): [("Wide", 0.75, "off", 1.0), ("Portable", 1.75, "off", 1.0)],
    })
    # 作業台：B | TV2 | A を接して並べる。1 格 = 5 cm で画面の高さ（9 行 = 0.45 m）と下端をそろえる。
    bx, by = BENCH
    inst("SM_F_Workbench_3m6", (bx, by, 0), 0, group="Bench")
    top = 0.905
    u = bx - 1.8 + 0.27
    for key, kind, sc, risers in (("B", "Portable", 1.5, 1), ("TV2", "Broadcast", 1.5, 2), ("A", "Wide", 1.0, 0)):
        width = TV[kind]["w"] * sc
        cx = u + width / 2
        for k in range(risers):
            inst("SM_F_Riser_Plank", (cx, by - 0.05, top + 0.035 * k), 0, (min(1.0, width / 0.9), 1.0, 1.0), group="Bench")
        d = TV[kind]["d"] * sc
        tv(kind, (cx, by - 0.42 + d / 2, top + 0.035 * risers), 0, sc, key, group="Bench", extra={"region": key})
        u += width
    fill_rack(rack_row((-13.6, -3.0, 0), 90, 9), 90, 9, r)
    fill_rack(rack_row((13.6, 22.0, 0), -90, 3), -90, 3, r)

    r = random.Random(23)
    for (x, y, hgt, yaw0) in ((8.6, 7.5, 7.5, -35), (11.3, 10.6, 10.5, -40), (9.2, 15.2, 9.0, -25),
                              (12.1, 18.6, 11.5, -45), (6.6, 20.2, 6.0, -15), (-9.0, 19.5, 8.5, 30),
                              # 左手前の空いた床に二本：机から左を見た時と全景の左半分に、月光を背負った輪郭を置く。
                              (-9.6, -1.6, 6.5, 22), (-12.2, 1.8, 9.5, -12)):
        inst("SM_F_Pallet", (x, y, 0), yaw0, group="Towers")
        z = 0.125
        while z < hgt:
            kind = r.choice(["Portable", "Wide", "Console", "Broadcast", "Portable", "Wide"])
            sc = r.uniform(0.95, 1.35)
            state = "static_dim" if r.random() < 0.1 else "off"
            tv(kind, (x + r.uniform(-0.08, 0.08), y + r.uniform(-0.08, 0.08), z), yaw0 + r.uniform(-14, 14), sc,
               state, group="Towers")
            z += TV[kind]["h"] * sc * (0.94 if kind == "Portable" else 1.0)
    for (kind, x, y, yaw, st) in (("Console", 5.8, 4.2, -60, "off"), ("Wide", 6.8, 5.4, -20, "off"),
                                  ("Portable", 6.1, 6.3, 25, "static_dim"), ("Broadcast", -3.0, 3.2, 10, "off"),
                                  ("Portable", -2.2, 3.6, -35, "off"), ("Wide", 1.2, 14.0, 160, "off")):
        tv(kind, (x, y, 0), yaw, 1.0, st, group="Floor")


CX, CY = 1.5, 8.5
BENCH = (-5.6, 4.1)


def place_crane():
    inst("SM_F_Crane_Bridge", (0, CY, 9.2), group="Crane")
    inst("SM_F_Crane_Hoist", (CX, CY, 6.5), group="Crane")
    for kind, dx, topz, sc, state, yaw in (("Portable", -1.9, 4.9, 1.5, "off", 10),
                                          ("Wide", -0.6, 3.9, 1.3, "static_dim", -6),
                                          ("Broadcast", 0.6, 5.3, 1.6, "off", 14),
                                          ("Console", 1.85, 3.6, 1.15, "off", -12)):
        h = TV[kind]["h"] * sc
        inst("SM_F_Chain_1m", (CX + dx, CY, topz), 0, (1.0, 1.0, 6.55 - topz), group="Crane")
        tv(kind, (CX + dx, CY, topz - h), yaw, sc, state, rot=(0.03, -0.02, math.radians(yaw)), group="Crane")


def unique(name, builder, group, bevel=0.006, extra=None):
    """世界座標で作った一品物を、底面中心を原点にして部品化し、その場所へ置く。"""
    origin = builder.bottom_center()
    objs = [builder.build(bevel=bevel, origin=origin)]
    for role, xb in (extra or []):
        o = xb.build(origin=origin)
        o["role"] = role
        objs.append(o)
    part(name, objs)
    return inst(name, tuple(origin), group=group)


def place_stair():
    st = Builder("stair")
    n = 14
    for x in (4.45, 5.35):
        st.beam((x, 13.0, 0.0), (x, 17.4, 4.2), 0.06, 0.22, "TD2F_Steel")
    for k in range(n):
        t = (k + 0.5) / n
        st.box((4.9, 13.0 + 4.4 * t, 4.2 * t + 0.08), (0.9, 0.26, 0.03), "TD2F_Steel")
    st.beam((5.4, 13.0, 0.95), (5.4, 17.4, 5.15), 0.035, 0.035, "TD2F_Steel")
    st.box((4.9, 17.9, 4.17), (1.1, 1.0, 0.06), "TD2F_Steel")
    unique("SM_F_Stair_Broken", st, "Props")
    tv("Console", (4.9, 18.0, 4.2), 0, 1.0, "static_dim", group="Props")


LIGHTS = []   # エンジンへ移す灯（確認用レンダーでも同じものを使う）


def light(kind, name, loc, color, energy, target=None, spot=None, blend=0.5, soft=0.1, volume=1.0, group="Lights",
          area=None, alive=True, tag="hall"):
    ld = bpy.data.lights.new(name, kind)
    ld.energy, ld.color = energy, color
    ld.shadow_soft_size, ld.volume_factor = soft, volume
    if kind == "SPOT":
        ld.spot_size, ld.spot_blend = math.radians(spot), blend
    if kind == "AREA":
        ld.shape, ld.size, ld.size_y = "RECTANGLE", area[0], area[1]
        ld.use_shadow = False
    lo = bpy.data.objects.new(name, ld)
    coll(group).objects.link(lo)
    lo.location = loc
    if target is not None:
        lo.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
    lo.hide_render = not alive
    LIGHTS.append({"obj": lo, "tag": tag, "alive": alive})
    return lo


HALL_COLD = (0.80, 0.89, 1.0)   # 水銀灯寄りの冷たい白。工場の闇は冷たく
WARM = (1.0, 0.74, 0.47)        # 開発者の居場所（作業台・机）だけ暖かい白熱灯


def place_lamps():
    # 明暗の階層：同じ明るさの灯を均等に並べると霧が一様な灰になり絵が平らになる。灯の数を絞り、強弱を付け、
    # 消えた灯具も残して、闇の中に光の島が奥へ連なる構図にする。電力 0 は消灯（灯具だけ置く）。
    for (x, y, power) in ((-8, 2.5, 0), (-2.5, 2.5, 0), (4.0, 2.5, 380), (-8, 11.0, 450), (-1.5, 11.0, 700),
                          (6, 11.0, 0), (-6, 16, 0), (1, 16, 420), (8, 16, 600), (-3, 21, 1000), (4, 21, 0)):
        alive = power > 0
        inst("SM_F_Lamp_Pendant", (x, y, 6.9), group="Lamps", bulb=alive)
        light("SPOT", "Pendant", (x, y, 6.88), HALL_COLD, float(power or 850), target=(x, y, 0), spot=74, blend=0.5,
              soft=0.12, alive=alive)
    light("SPOT", "CraneLight", (CX, CY - 0.3, 9.15), HALL_COLD, 300.0, target=(CX, CY - 0.3, 0), spot=52,
          soft=0.08)
    # 主光：試験台の真上に低く吊った暖色の灯。テレビの天面と台の手前だけを照らし、游戏画面の周りに温度を置く。
    bx, by = BENCH
    hz = 3.0
    inst("SM_F_Lamp_Pendant", (bx, by - 0.75, hz), group="Lamps")
    inst("SM_F_Chain_1m", (bx, by - 0.75, hz + 4.2), 0, (1.0, 1.0, 11.0 - hz - 4.2), group="Lamps")
    light("SPOT", "BenchPendant", (bx, by - 0.75, hz - 0.02), WARM, 250.0, target=(bx, by - 0.45, 0.9), spot=84,
          blend=0.55, soft=0.06, tag="desk")
    # 背光：棚の陰から台の方へ向けた冷たい作業灯。霧を背後で光らせてテレビの輪郭を浮かせる（光源は棚に隠す）。
    light("SPOT", "BenchRim", (bx + 0.3, by + 1.0, 2.4), HALL_COLD, 150.0, target=(bx, by - 0.3, 1.2), spot=60,
          blend=0.6, soft=0.05, tag="desk")


DX, DY = 3.45, -3.0


def place_desk_corner():
    d = Builder("desk")
    d.box((0, 0, 0.74), (1.6, 0.8, 0.04), "TD2F_Wood")
    for sx in (-0.74, 0.74):
        for sy in (-0.34, 0.34):
            d.box((sx, sy, 0.36), (0.04, 0.04, 0.72), "TD2F_Steel")
    d.box((0.5, 0.02, 0.38), (0.42, 0.7, 0.64), "TD2F_Steel")
    for z in (0.52, 0.26):
        d.box((0.5, -0.335, z), (0.36, 0.01, 0.02), "TD2F_BareMetal")
    part("SM_F_Desk", [d.build(bevel=0.004)])
    inst("SM_F_Desk", (DX, DY, 0), group="DeskCorner")

    k = Builder("keyboard")
    k.box((0, 0, 0.015), (0.46, 0.16, 0.03), "TD2F_PlasticBeige")
    k.box((0, -0.005, 0.032), (0.42, 0.12, 0.006), "TD2F_PlasticDark")
    part("SM_F_Keyboard", [k.build(bevel=0.003)])
    inst("SM_F_Keyboard", (DX - 0.02, DY - 0.28, 0.76), group="DeskCorner")

    m = Builder("mug")
    m.cyl((0, 0, 0.05), 0.04, 0.1, "TD2F_Ceramic")
    part("SM_F_Mug", [m.build(bevel=0.003)])
    inst("SM_F_Mug", (DX + 0.58, DY - 0.22, 0.76), group="DeskCorner")

    p = Builder("paper")
    p.box((0, 0, 0.0005), (0.21, 0.297, 0.001), "TD2F_Paper")
    part("SM_F_PaperSheet", [p.build()])
    for i, (px, py, yaw) in enumerate(((DX - 0.55, DY - 0.15, 12), (DX - 0.5, DY - 0.05, -7), (DX + 0.25, DY + 0.1, 20))):
        inst("SM_F_PaperSheet", (px, py, 0.76 + i * 0.001), yaw, group="DeskCorner")
    s = Builder("sticky")
    s.box((0, 0, 0.005), (0.14, 0.18, 0.01), "TD2F_Sticky")
    part("SM_F_StickyPad", [s.build()])
    inst("SM_F_StickyPad", (DX + 0.62, DY + 0.1, 0.76), 17, group="DeskCorner")

    # 帽子：2D の主人公と同じ帽子。
    c = Builder("cap")
    c.sphere((0, 0, 0.0), 0.1, "TD2F_CapFabric", scale=(1.0, 1.1, 0.62))
    c.box((0, -0.12, 0.005), (0.15, 0.1, 0.008), "TD2F_CapFabric", rot=(0.08, 0, 0))
    part("SM_F_Cap", [c.build(bevel=0.002, origin=(0, 0, -0.0))])
    inst("SM_F_Cap", (DX + 0.4, DY - 0.05, 0.765), group="DeskCorner")

    tv("DeskCRT", (DX - 0.02, DY + 0.15, 0.76), 0, 1.0, "menu", group="DeskCorner")

    # 卓上灯：笠の広い口（-Z）を照射方向へ向ける。
    head, target = Vector((DX - 0.45, DY - 0.12, 1.06)), Vector((DX + 0.05, DY - 0.38, 0.76))
    lb = Builder("desklamp")
    lb.cyl((DX - 0.6, DY + 0.22, 0.775), 0.07, 0.03, "TD2F_Steel")
    lb.beam((DX - 0.6, DY + 0.22, 0.79), (DX - 0.62, DY + 0.1, 1.2), 0.018, 0.018, "TD2F_Steel")
    lb.beam((DX - 0.62, DY + 0.1, 1.2), (DX - 0.47, DY - 0.07, 1.12), 0.018, 0.018, "TD2F_Steel")
    lb.shade(tuple(head), 0.075, 0.13, "TD2F_Steel", r2=0.03, segs=20,
             rot=tuple((target - head).to_track_quat("-Z", "Y").to_euler()), wall=0.004)
    bb = Builder("desklamp_bulb")
    bb.sphere(tuple(head + (target - head).normalized() * 0.02), 0.022, "TD2F_BulbWarm")
    unique("SM_F_Lamp_Desk", lb, "DeskCorner", bevel=0.004, extra=[("Bulb", bb)])
    light("SPOT", "DeskLamp", tuple(head + (target - head).normalized() * 0.05), (1.0, 0.72, 0.42), 35.0,
          target=tuple(target), spot=72, blend=0.6, soft=0.03, volume=0.6, tag="desk")

    ch = Builder("chair")
    ch.box((0, 0, 0.48), (0.5, 0.48, 0.07), "TD2F_Fabric")
    ch.box((0, 0.25, 0.82), (0.46, 0.06, 0.52), "TD2F_Fabric")  # 背もたれは後ろ（+Y）。座る向きが正面
    ch.cyl((0, 0, 0.24), 0.025, 0.42, "TD2F_Steel")
    for i in range(5):
        a = i * 2 * math.pi / 5
        ch.beam((0, 0, 0.05), (0.3 * math.cos(a), 0.3 * math.sin(a), 0.03), 0.04, 0.03, "TD2F_PlasticDark")
    part("SM_F_Chair_Office", [ch.build(bevel=0.01, origin=(0, 0, 0.015))])
    inst("SM_F_Chair_Office", (DX, DY - 0.75, 0.015), 180, group="DeskCorner")
    bp = Builder("backpack")
    bp.box((0, 0, 0.21), (0.32, 0.18, 0.42), "TD2F_CapFabric")
    bp.box((0, -0.1, 0.11), (0.24, 0.06, 0.18), "TD2F_CapFabric")
    part("SM_F_Backpack", [bp.build(bevel=0.03)])
    inst("SM_F_Backpack", (DX, DY - 1.12, 0.51), 0, group="DeskCorner")

    pb = Builder("pinboard")
    pb.box((0, 0, 1.3), (1.2, 0.03, 0.9), "TD2F_Cork")
    for x in (-0.55, 0.55):
        pb.box((x, 0.03, 0.43), (0.04, 0.04, 0.86), "TD2F_Steel")
        pb.box((x, 0.03, 0.02), (0.04, 0.5, 0.04), "TD2F_Steel")
    pb.box((-0.3, -0.017, 1.1), (0.21, 0.002, 0.29), "TD2F_Paper", rot=(0, 0.06, 0))
    for (x, z) in ((0.38, 1.12), (0.1, 1.08), (0.46, 1.6)):
        pb.box((x, -0.018, z), (0.07, 0.002, 0.07), "TD2F_Sticky")
    part("SM_F_PinBoard", [pb.build(bevel=0.004)])
    pbo = inst("SM_F_PinBoard", (5.15, -2.35, 0), -45, group="DeskCorner")
    # ピンボードは無地の紙を置く。ゲーム画像は Runtime キャプチャ取得後に追加する。
    for (x, z, w_, h_, key) in ((-0.28, 1.42, 0.34, 0.2, "A"), (0.2, 1.38, 0.26, 0.2, "B")):
        pm = pbr("print_" + key, "D8D2C4", 0.85)
        me = bpy.data.meshes.new("Print")
        y = -0.0175
        me.from_pydata([(x - w_ / 2, y, z - h_ / 2), (x + w_ / 2, y, z - h_ / 2), (x + w_ / 2, y, z + h_ / 2),
                        (x - w_ / 2, y, z + h_ / 2)], [], [(0, 1, 2, 3)])
        uv = me.uv_layers.new(name="UVMap")
        for i, q in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
            uv.data[i].uv = q
        me.materials.append(pm)
        po = bpy.data.objects.new("Print", me)
        coll("Preview").objects.link(po)
        po.parent = pbo

    # 三脚の作業灯：机周りの床に暖かい光だまりを広げる。頭部の向きは部品に焼き込む。
    pos, target = Vector((5.35, -0.95, 1.9)), Vector((3.9, -2.3, 0.0))
    wl = Builder("worklight")
    for i in range(3):
        a = i * 2 * math.pi / 3 + 0.4
        wl.beam((pos.x, pos.y, 1.15), (pos.x + 0.45 * math.cos(a), pos.y + 0.45 * math.sin(a), 0.0), 0.025, 0.025, "TD2F_Steel")
    wl.beam((pos.x, pos.y, 1.1), (pos.x, pos.y, 1.82), 0.03, 0.03, "TD2F_Steel")
    wl.box(tuple(pos), (0.26, 0.16, 0.2), "TD2F_Steel", rot=tuple((target - pos).to_track_quat("-Z", "Y").to_euler()))
    unique("SM_F_Lamp_WorkTripod", wl, "DeskCorner")
    light("SPOT", "WorkLight", tuple(pos + (target - pos).normalized() * 0.12), (1.0, 0.74, 0.48), 95.0,
          target=tuple(target), spot=70, blend=0.55, soft=0.08, volume=0.6, tag="desk")


def place_cables_and_props():
    """床を這う配線は曲線をメッシュ化して一品物の部品にする。"""
    paths = [
        [(DX, DY + 0.38, 0.6), (DX - 0.3, DY + 0.5, 0.012), (1.5, 0.5, 0.012), (-3.5, 3.6, 0.012),
         (-5.0, 4.0, 0.012), (-5.2, 4.45, 0.4), (-5.3, 4.5, 0.88)],
        [(DX + 0.2, DY + 0.38, 0.5), (DX + 0.6, DY + 0.6, 0.012), (6.0, 2.0, 0.012), (8.2, 6.4, 0.012), (8.5, 7.2, 0.3)],
        [(-6.6, 4.5, 0.88), (-6.7, 4.4, 0.4), (-6.2, 3.6, 0.012), (-1.5, 6.5, 0.012), (4.6, 12.6, 0.012), (4.9, 13.2, 0.05)],
    ]
    for i, pts in enumerate(paths, start=1):
        cu = bpy.data.curves.new("cable", "CURVE")
        cu.dimensions = "3D"
        sp = cu.splines.new("BEZIER")
        sp.bezier_points.add(len(pts) - 1)
        for bp, p in zip(sp.bezier_points, pts):
            bp.co = p
            bp.handle_left_type = bp.handle_right_type = "AUTO"
        cu.bevel_depth, cu.bevel_resolution = 0.012, 2
        tmp = bpy.data.objects.new("cable_tmp", cu)
        SCENE.collection.objects.link(tmp)
        bpy.context.view_layer.update()
        me = bpy.data.meshes.new_from_object(tmp.evaluated_get(bpy.context.evaluated_depsgraph_get()))
        bpy.data.objects.remove(tmp)
        b = Builder(f"cable{i}")
        b.bm.from_mesh(me)
        for f in b.bm.faces:
            f.material_index = 0
            f.smooth = True
        b.mats = ["TD2F_Rubber"]
        unique(f"SM_F_Cable_{i:02d}", b, "Props", bevel=0.0)
    for (x, y, z, yaw) in ((-4.6, 2.2, 0, 15), (-4.4, 2.3, 0.15, 8), (1.5, 7.5, 0, -20)):
        inst("SM_F_Pallet", (x, y, z), yaw, group="Props")
    inst("SM_F_CableReel", (6.2, 0.6, 0), 35, group="Props")
    # 水たまりは光の当たる所に置き、吊り灯・画面・窓の映り込みを拾う。
    for name, x, y, yaw in (("SM_F_Puddle_C", -8.2, 2.0, 20), ("SM_F_Puddle_A", -3.0, 1.6, -35), ("SM_F_Puddle_B", -5.4, 2.9, 10),
                            ("SM_F_Puddle_A", 3.6, 1.2, 60), ("SM_F_Puddle_C", -1.2, 10.4, -15), ("SM_F_Puddle_B", 1.6, 15.2, 40),
                            ("SM_F_Puddle_A", 8.4, 15.0, -70), ("SM_F_Puddle_B", 0.9, 5.2, 5)):
        inst(name, (x, y, 0.004), yaw, group="Props")
    inst("SM_F_Crate_Stack", (-0.8, 4.4, 0), -10, group="Props")


make_hall_parts()
make_tv_parts()
make_prop_parts()
place_hall()
place_racks_and_towers()
place_crane()
place_stair()
place_lamps()
place_desk_corner()
place_cables_and_props()


# ================================================================ FBX 書き出しと部品一覧
def export_parts():
    manifest = {"schema_version": 1, "set": "FactorySet01",
                "conventions": {"units": "meters", "up": "+Y", "front": "+Z", "pivot": "bottom_center",
                                "screen_material": "TD2_Screen", "node_transforms": "identity",
                                "uv": "box projection, 1 m = 1 UV unit; screens 0..1"},
                "parts": []}
    for name, p in PART.items():
        objs = p["objs"]
        for o in objs:
            EXPORT_COLL.objects.link(o)
        bpy.ops.object.select_all(action="DESELECT")
        for o in objs:
            o.select_set(True)
        bpy.context.view_layer.objects.active = objs[0]
        path = EXPORTS / f"{name}.fbx"
        bpy.ops.export_scene.fbx(filepath=str(path), use_selection=True, object_types={"MESH"}, global_scale=1.0,
                                 apply_unit_scale=True, apply_scale_options="FBX_SCALE_UNITS", axis_forward="-Z",
                                 axis_up="Y", use_space_transform=True, bake_space_transform=True,
                                 use_mesh_modifiers=True, mesh_smooth_type="OFF", use_triangles=True, use_tspace=False,
                                 bake_anim=False, add_leaf_bones=False, use_custom_props=False,
                                 path_mode="COPY" if p["info"].get("embed") else "AUTO",
                                 embed_textures=bool(p["info"].get("embed")))
        dg = bpy.context.evaluated_depsgraph_get()
        tris, pts = 0, []
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
                 "dimensions_m": [round(hi[0] - lo[0], 4), round(hi[2] - lo[2], 4), round(hi[1] - lo[1], 4)],
                 "materials": sorted({m.name for o in objs for m in o.data.materials}),
                 # FBX 内の出現順（本体の材質→画面）。画面材質の索引の推定に使う。
                 "material_order": list(dict.fromkeys(m.name for o in objs for m in o.data.materials))}
        if "screen" in p["info"]:
            s = p["info"]["screen"]
            entry["screen"] = {"width_m": s["w"], "height_m": s["h"], "center_x_m": s["cx"], "bottom_m": s["bottom"],
                               "front_z_m": round(-s["y"], 4)}
        manifest["parts"].append(entry)
        for o in objs:
            EXPORT_COLL.objects.unlink(o)
    (ROOT / "factory_set01_manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print("EXPORTED", len(PART))


def to_engine(v):
    """Blender (X 右, Y 奥, Z 上) → エンジン (X 右, Y 上, Z 手前)。FBX 書き出しと同じ変換。"""
    return [round(v[0], 4), round(v[2], 4), round(-v[1], 4)]


def write_layout():
    bpy.context.view_layer.update()
    out = {"schema_version": 1, "coordinate_note": "engine: +Y up, model front +Z; rotation_deg = [x, y(yaw), z]",
           "objects": [], "lights": [], "cameras": []}
    for rec, body in LAYOUT:
        rx, ry, rz = body.rotation_euler
        e = dict(rec)
        e["position"] = to_engine(body.location)
        e["rotation_deg"] = [round(math.degrees(rx), 3), round(math.degrees(rz), 3), round(-math.degrees(ry), 3)]
        e["scale"] = [round(body.scale[0], 4), round(body.scale[2], 4), round(body.scale[1], 4)]
        out["objects"].append(e)
    for spec in LIGHTS:
        lo = spec["obj"]
        d = lo.data
        fwd = lo.matrix_world.to_quaternion() @ Vector((0, 0, -1))
        item = {"name": lo.name, "type": d.type, "tag": spec["tag"], "alive": spec["alive"],
                "position": to_engine(lo.matrix_world.translation), "direction": to_engine(fwd),
                "color": [round(c, 3) for c in d.color], "blender_power_w": d.energy}
        if d.type == "SPOT":
            item["cone_deg"] = round(math.degrees(d.spot_size), 2)
            item["blend"] = d.spot_blend
        out["lights"].append(item)
    for name, (pos, target, fov, *_rest) in VIEWS.items():
        out["cameras"].append({"name": name, "position": to_engine(pos), "target": to_engine(target), "fov_y_deg": fov})
    (ROOT / "factory_set01_layout.json").write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")
    print("LAYOUT", len(out["objects"]), "objects", len(out["lights"]), "lights")


# ================================================================ 確認用：霧・月光・外景・起動画面の文字
def preview_only():
    me = bpy.data.meshes.new("FogVolume")
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0, matrix=Matrix.Translation((0, 9, 6)) @ Matrix.Diagonal((30, 30, 12, 1)))
    bm.to_mesh(me)
    bm.free()
    m = bpy.data.materials.new("Fog")
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.remove(nt.nodes["Principled BSDF"])
    vol = node(nt, "ShaderNodeVolumePrincipled")
    vol.inputs["Color"].default_value = (0.78, 0.82, 0.86, 1)
    vol.inputs["Anisotropy"].default_value = 0.45
    geo = node(nt, "ShaderNodeNewGeometry")
    sep = node(nt, "ShaderNodeSeparateXYZ")
    nt.links.new(geo.outputs["Position"], sep.inputs[0])
    hr = node(nt, "ShaderNodeMapRange")
    hr.inputs["From Max"].default_value = 9.0
    hr.inputs["To Min"].default_value = 1.0
    hr.inputs["To Max"].default_value = 0.22
    nt.links.new(sep.outputs["Z"], hr.inputs["Value"])
    noise = node(nt, "ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 0.08
    nt.links.new(geo.outputs["Position"], noise.inputs["Vector"])
    nr = node(nt, "ShaderNodeMapRange")
    nr.inputs["To Min"].default_value = 0.55
    nr.inputs["To Max"].default_value = 1.35
    nt.links.new(noise.outputs["Fac"], nr.inputs["Value"])
    m1 = node(nt, "ShaderNodeMath")
    m1.operation = "MULTIPLY"
    nt.links.new(hr.outputs["Result"], m1.inputs[0])
    nt.links.new(nr.outputs["Result"], m1.inputs[1])
    m2 = node(nt, "ShaderNodeMath")
    m2.operation = "MULTIPLY"
    m2.inputs[1].default_value = 0.03
    nt.links.new(m1.outputs["Value"], m2.inputs[0])
    nt.links.new(m2.outputs["Value"], vol.inputs["Density"])
    nt.links.new(vol.outputs["Volume"], nt.nodes["Material Output"].inputs["Volume"])
    me.materials.append(m)
    fog = bpy.data.objects.new("FogVolume", me)
    coll("Preview").objects.link(fog)
    fog.visible_shadow = False

    out = Builder("Outside")
    pbr("outside", "000000", 1.0, emission=(0.012, 0.016, 0.026), strength=1.0)
    out.box((0, Y1 + 4, 6), (60, 0.1, 30), "outside")
    out.box((0, Y0 - 4, 6), (60, 0.1, 30), "outside")
    out.box((X0 - 4, 9, 6), (0.1, 60, 30), "outside")
    out.box((X1 + 4, 9, 6), (0.1, 60, 30), "outside")
    oo = out.build()
    coll("Preview").objects.link(oo)
    oo.visible_shadow = False

    moon = bpy.data.lights.new("Moon", "SUN")
    moon.energy, moon.color, moon.angle, moon.volume_factor = 0.7, (0.62, 0.74, 1.0), math.radians(1.5), 1.0
    mo = bpy.data.objects.new("Moon", moon)
    coll("Lights").objects.link(mo)
    mo.rotation_euler = Vector((0.28, -0.82, -0.5)).to_track_quat("-Z", "Y").to_euler()
    LIGHTS.append({"obj": mo, "tag": "moon", "alive": True})

    world = bpy.data.worlds.new("Night")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.003, 0.0035, 0.0045, 1)
    SCENE.world = world

    # 起動画面の文字（エンジンでは画面 Canvas の文字描画に置き換える）
    scr = next(s for s, st, _ in SCREENS if st == "menu")
    body = scr.parent
    sp = PART[TV["DeskCRT"]["part"]]["info"]["screen"]
    w, h, cx, bottom, y = sp["w"], sp["h"], sp["cx"], sp["bottom"], sp["y"]
    font, title_font = bpy.data.fonts.load(FONT_CJK), bpy.data.fonts.load(FONT_TITLE)
    txt_mat = pbr("menu_text", "2F4D4E", 0.6)
    pbr("menu_sel", "000000", 1.0, emission=lin("E3B263"), strength=SCREEN_STRENGTH * 0.9)

    def text(s, x, z, size, fnt, align="LEFT"):
        cu = bpy.data.curves.new("MenuText", "FONT")
        cu.body, cu.font, cu.size, cu.align_x = s, fnt, size, align
        ob = bpy.data.objects.new("MenuText", cu)
        coll("Preview").objects.link(ob)
        ob.data.materials.append(txt_mat)
        ob.parent = body
        ob.location = (x, y - 0.0025, z)
        ob.rotation_euler = (math.radians(90), 0, 0)

    zc = bottom + h / 2
    text("TD2", cx, zc + 0.052, 0.05, title_font, "CENTER")
    for i, s in enumerate(["从头开始", "继续", "设置", "退出"]):
        text(s, cx - 0.04, zc + 0.012 - i * 0.026, 0.017, font)
    sel = Builder("MenuSelect")
    sel.box((cx - 0.008, y - 0.0012, zc + 0.017), (0.11, 0.0004, 0.022), "menu_sel")
    tri = Builder("MenuCursor")
    tri.quad([(cx - 0.062, y - 0.0028, zc + 0.011), (cx - 0.048, y - 0.0028, zc + 0.017),
              (cx - 0.062, y - 0.0028, zc + 0.023)], "menu_text")
    for bld in (sel, tri):
        o = bld.build()
        coll("Preview").objects.link(o)
        o.parent = body


VIEWS = {}


def set_state(state):
    """停電：工場の灯と机の灯を落とし、全テレビを砂嵐に。机のモニターだけ点いたまま。"""
    black = state == "blackout"
    for spec in LIGHTS:
        if spec["tag"] in ("hall", "desk"):
            spec["obj"].hide_render = black or not spec["alive"]
    for b in BULBS:
        b.material_slots[0].material = MAT["bulb_off" if black else ("TD2F_BulbWarm" if "Desk" in b.name else "TD2F_Bulb")]
    for scr, normal, spill in SCREENS:
        if normal == "menu":
            cur = "scr_menu"
        elif black:
            cur = "static"
        else:
            cur = "scr_" + normal if normal in LIT else ("static_dim" if normal == "static_dim" else "TD2_Screen")
        scr.material_slots[0].material = MAT[cur]
        if spill:
            spill.hide_render = black and normal != "menu"


def render_previews():
    r = SCENE.render
    r.engine = "BLENDER_EEVEE"
    r.resolution_x, r.resolution_y = 1600, 900
    r.resolution_percentage = OPTS["scale"]
    r.image_settings.file_format = "PNG"
    ee = SCENE.eevee
    ee.taa_render_samples = OPTS["samples"]
    for key, value in (("use_raytracing", True), ("use_fast_gi", True), ("use_shadows", True),
                       ("use_volumetric_shadows", True), ("volumetric_samples", 96), ("volumetric_start", 0.1),
                       ("volumetric_end", 60.0), ("volumetric_tile_size", "4" if OPTS["scale"] >= 100 else "8"),
                       ("volumetric_shadow_samples", 32)):
        if hasattr(ee, key):
            try:
                setattr(ee, key, value)
            except Exception as exc:
                print("EEVEE skip", key, exc)
    vs = SCENE.view_settings
    vs.view_transform = "AgX"
    looks = [i.identifier for i in vs.bl_rna.properties["look"].enum_items]
    vs.look = next((l for l in looks if "Medium High Contrast" in l), "None")
    cams = {}
    for name, (pos, target, fov, focus, fstop, state) in VIEWS.items():
        cd = bpy.data.cameras.new(name)
        cd.sensor_fit, cd.angle_y, cd.clip_start = "VERTICAL", math.radians(fov), 0.02
        if focus:
            cd.dof.use_dof = True
            cd.dof.focus_distance = (Vector(target) - Vector(pos)).length
            cd.dof.aperture_fstop = fstop
        co = bpy.data.objects.new(name, cd)
        coll("Cameras").objects.link(co)
        co.location = pos
        co.rotation_euler = (Vector(target) - Vector(pos)).to_track_quat("-Z", "Y").to_euler()
        cams[name] = co
    wanted = OPTS["views"] or list(VIEWS)
    for state in ("normal", "blackout"):
        todo = [k for k in wanted if VIEWS[k][5] == state]
        if not todo:
            continue
        set_state(state)
        for k in todo:
            SCENE.camera = cams[k]
            r.filepath = str(PREVIEWS / f"{k}.png")
            bpy.ops.render.render(write_still=True)
            print("RENDERED", k)
    set_state("normal")
    SCENE.camera = cams["V1_menu"]


preview_only()
bpy.context.view_layer.update()
_a = next(s for s, st, _ in SCREENS if st == "TV2")
_sa = PART[TV["Broadcast"]["part"]]["info"]["screen"]
A_CENTER = _a.parent.matrix_world @ Vector((_sa["cx"], _sa["y"], _sa["bottom"] + _sa["h"] / 2))  # TV2 の画面中心
_m = next(s for s, st, _ in SCREENS if st == "menu")
_sm = PART[TV["DeskCRT"]["part"]]["info"]["screen"]
M_CENTER = _m.parent.matrix_world @ Vector((_sm["cx"], _sm["y"], _sm["bottom"] + _sm["h"] / 2))
VIEWS.update({
    # 名前: (位置, 注視点, 縦画角, 被写界深度, F 値, 状態)
    "V1_menu": ((DX - 0.02, DY - 0.78, 1.2), tuple(M_CENTER + Vector((0, 0, 0.02))), 38, True, 4.0, "normal"),
    "V2_look_left": ((DX - 0.1, DY - 0.7, 1.25), (-5.6, 4.6, 2.0), 50, False, 0, "normal"),
    "V3_look_up": ((DX, DY - 0.6, 1.25), (CX - 0.4, CY, 5.0), 50, False, 0, "normal"),
    "V4_gameplay": (tuple(A_CENTER + Vector((0.2, -2.6, 0.1))), tuple(A_CENTER), 26, True, 2.8, "normal"),
    "V5_wide": ((13.6, -5.5, 8.0), (-5.0, 8.0, 0.4), 55, False, 0, "normal"),
    "V6_blackout_left": ((DX - 0.1, DY - 0.7, 1.25), (-5.6, 4.6, 2.0), 50, False, 0, "blackout"),
    "V7_blackout_wide": ((13.6, -5.5, 8.0), (-5.0, 8.0, 0.4), 55, False, 0, "blackout"),
})
if OPTS["export"]:
    export_parts()
write_layout()
render_previews()
bpy.data.collections.remove(EXPORT_COLL)
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / "FactorySet01.blend"))
print("DONE")
