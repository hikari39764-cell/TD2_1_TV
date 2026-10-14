# -*- coding: utf-8 -*-
"""素描（墨色の紙に象牙の鉛筆）に纵深感を足す試作。ScreenStyle07 の描画結果（色・分類・奥行き）を使う。

足すもの
1. 面の向きで明暗を分ける：上面（焼いた明の段）は白墨を厚く、正面（基の段）は淡い色と疎らな斜線、
   側面・下面（暗の段）は紙より暗く沈める。暗い紙なので「光が当たる所ほど白墨が乗る」描き方になる。
2. 線の強弱：奥の壁に対して浮いている外形は太く、物体の中の段の境は細く淡く、壁の目盛りはかすかに。
3. 落ち影を戻す：壁に落ちた影は紙より一段暗く、疎らな斜線を添える。物体の周りの壁はわずかに暗くして接地させる。
4. 背景を遠ざける：光の輪は紙に淡い白墨のぼかしとして残し、壁全体に空気の層を作る。
比較のため、元の試作の素描関数をそのまま取り出して同じ画像から現行版も作る（紙の色はエンジンに合わせる）。
既存の 7° の描画結果に加え、エンジンと同じ回し方の俯角・側角で A と GALLERY を描き直して比べる。
実行: blender -b Library/ScreenStyle07/<palette>/ScreenStyle07.blend --python sketch_depth_study.py -- --palette teal
出力は Library/ScreenStyle07/SketchDepth/<palette>（Git 対象外）。
"""

import ast
import math
import struct
import sys
import zlib
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent
ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
PALETTE_NAME = ARGS[ARGS.index("--palette") + 1] if "--palette" in ARGS else "teal"
SOURCE = PROJECT / "Library" / "ScreenStyle07" / PALETTE_NAME
OUT = PROJECT / "Library" / "ScreenStyle07" / "SketchDepth" / PALETTE_NAME
OUT.mkdir(parents=True, exist_ok=True)
PAPER = {"ink": "1A2733", "teal": "12292A"}[PALETTE_NAME]   # エンジンの素描と同じ紙の色
ANGLES = [(15.0, 15.0), (22.0, 0.0)]                        # （俯角, 側角）。側角の正は右から見る

# ---------------------------------------------------------------- 元の試作から配色と関数だけを取り出す
WANTED = {"PX_FULL", "PALETTES", "PAL", "srgb", "lin", "PALETTE", "ID", "NO_OUTLINE", "GLOW_IDS", "ROOMS", "load",
          "save", "quantize", "neighbors", "outline_mask", "box_blur", "glow_layer", "band_index", "thick",
          "style_sketch"}
tree = ast.parse((ROOT / "build_screen_style.py").read_text(encoding="utf-8"))
body = []
for item in tree.body:
    named = isinstance(item, ast.FunctionDef) and item.name in WANTED
    assigned = isinstance(item, ast.Assign) and any(isinstance(t, ast.Name) and t.id in WANTED for t in item.targets)
    if named or assigned:
        if isinstance(item, ast.FunctionDef) and item.name == "style_sketch":
            for sub in ast.walk(item):   # 現行の素描もエンジンと同じ紙の色で比べる
                if isinstance(sub, ast.Constant) and sub.value == "1A2733":
                    sub.value = PAPER
        body.append(item)
S = {"np": np, "math": math, "bpy": bpy, "Path": Path, "struct": struct, "zlib": zlib, "PALETTE_NAME": PALETTE_NAME}
exec(compile(ast.Module(body=body, type_ignores=[]), "build_screen_style.py", "exec"), S)
PAL, ID, srgb = S["PAL"], S["ID"], S["srgb"]


def color_is(q, name):
    # q は配色へ量子化済み。光の輪の芯と目盛りは 1/255 しか違わないので、ほぼ一致だけを取る。
    return np.all(np.abs(q - np.array(srgb(PAL[name]), np.float32)) < 0.002, axis=-1)


def blend(img, mask, target, amount):
    """mask の所だけ target へ amount 寄せる（target は色か画像、amount は数か画像）。"""
    target = np.broadcast_to(np.asarray(target, np.float32), img.shape)
    amount = np.broadcast_to(np.asarray(amount, np.float32), img.shape[:2])[..., None] if np.ndim(amount) < 3 else amount
    out = img * (1 - amount) + target * amount
    return np.where(mask[..., None], out, img)


def style_sketch_depth(color, data):
    ids = np.rint(data[..., 0] * 32).astype(int)
    depth = data[..., 1] * 50
    q, band = S["band_index"](color[..., :3])
    h, w = band.shape
    rng = np.random.default_rng(7)
    yy, xx = np.mgrid[0:h, 0:w]
    paper = np.array(srgb(PAPER), np.float32)
    pencil = np.array(srgb(PAL["ivory"]), np.float32)
    grain = S["box_blur"](rng.normal(0, 1, (h, w, 1)).astype(np.float32), 1) * 0.03
    img = np.broadcast_to(paper, (h, w, 3)) + grain
    wall = (ids == ID["wall"]) | (ids == 0)
    obj = ~wall

    # 4. 背景：光の輪を淡い白墨のぼかしで残す（中心ほど濃い）。目盛りは遠くの線としてかすかに。
    wash = np.zeros((h, w, 1), np.float32)
    wash[color_is(q, "pool") | color_is(q, "pool_line")] = 0.035
    wash[color_is(q, "pool_core")] = 0.06
    wash = S["box_blur"](S["box_blur"](wash, 8), 8)
    img = np.where(wall[..., None], img * (1 - wash) + pencil * wash, img)
    grid = wall & (color_is(q, "void_line") | color_is(q, "pool_line"))
    img = blend(img, grid, pencil, 0.08)
    # 3. 落ち影：壁の影は紙より暗い淡彩（縁を少しぼかす）、疎らな斜線を添える。
    shadow = (wall & color_is(q, "void_shadow")).astype(np.float32)[..., None]
    shadow = S["box_blur"](shadow, 1)
    img = img * (1 - 0.30 * shadow)
    hatch = (shadow[..., 0] > 0.5) & ((xx - yy) % 9 == 0)
    img = blend(img, hatch, pencil, 0.07)
    #    接地：物体のすぐ周りの壁をわずかに暗くする（近いほど濃い）。
    near1 = S["thick"](obj, 2) & wall
    near2 = S["thick"](obj, 6) & wall & ~near1
    img = np.where(near1[..., None], img * 0.84, np.where(near2[..., None], img * 0.93, img))

    # 1. 物体：面の向き（焼いた段）で白墨の量を変える。人物は線で描く対象なので塗り足さない。
    tint = img * 0.78 + q * 0.22
    img = np.where(obj[..., None], tint, img)
    figure = np.isin(ids, [ID["player"], ID["echo"]])
    light, dark = obj & (band == 2) & ~figure, obj & (band == 0)
    chalk = 0.14 + 0.06 * S["box_blur"](rng.random((h, w, 1)).astype(np.float32), 1)[..., 0]
    img = blend(img, light, pencil, chalk)                                  # 上面：白墨を少し厚く
    img = np.where(dark[..., None], paper * 0.82 + q * 0.10, img)            # 側面・下面：紙より沈め、
    img = blend(img, dark & ((xx + yy) % 6 == 0), pencil, 0.28)             # 疎らな斜線で陰を描く
    red = color_is(q, "red") | color_is(q, "red_shadow")
    img = blend(img, red, np.array(srgb(PAL["red"]), np.float32), 0.6)

    # 2. 線：奥の面との奥行きの差が大きい外形は太く濃く、物体の中の段の境は細く淡く（片側 1 画素）。
    solid = ~np.isin(ids, list(S["NO_OUTLINE"]))
    gap = np.zeros((h, w), np.float32)
    for n_id, n_depth, n_solid in zip(S["neighbors"](ids), S["neighbors"](depth), S["neighbors"](solid)):
        gap = np.maximum(gap, np.where(n_solid & (n_id != ids), depth - n_depth, 0))
    outer = gap > 0.25
    heavy = S["thick"](gap > 0.6, 1) & ~(obj & (band == 2))
    idx = (q * 255).astype(int) @ np.array([65536, 256, 1])
    right = np.hstack([idx[:, 1:], idx[:, -1:]])
    down = np.vstack([idx[1:], idx[-1:]])
    inner = obj & ((right != idx) | (down != idx)) & ~outer
    jitter = rng.random((h, w)) < 0.86
    img = blend(img, inner & jitter, pencil, 0.72)
    img = blend(img, (outer | heavy) & jitter, pencil, 0.88)
    echo = np.roll(np.roll(outer, 1, 0), 1, 1) & (rng.random((h, w)) < 0.5)
    img = blend(img, echo & ~outer, pencil, 0.35)
    return np.clip(img + S["glow_layer"](ids, q, 1, 10) * 0.45, 0, 1)


# ---------------------------------------------------------------- 描画（エンジンと同じ回し方の镜头）
IDM = bpy.data.materials.new("id_depth_study")
IDM.use_nodes = True
nt = IDM.node_tree
for n in list(nt.nodes):
    nt.nodes.remove(n)
info, camd, comb = nt.nodes.new("ShaderNodeObjectInfo"), nt.nodes.new("ShaderNodeCameraData"), nt.nodes.new("ShaderNodeCombineXYZ")
d32, d50 = nt.nodes.new("ShaderNodeMath"), nt.nodes.new("ShaderNodeMath")
d32.operation, d32.inputs[1].default_value = "DIVIDE", 32.0
d50.operation, d50.inputs[1].default_value = "DIVIDE", 50.0
nt.links.new(info.outputs["Object Index"], d32.inputs[0])
nt.links.new(camd.outputs["View Z Depth"], d50.inputs[0])
nt.links.new(d32.outputs[0], comb.inputs[0])
nt.links.new(d50.outputs[0], comb.inputs[1])
emission, output = nt.nodes.new("ShaderNodeEmission"), nt.nodes.new("ShaderNodeOutputMaterial")
nt.links.new(comb.outputs[0], emission.inputs[0])
nt.links.new(emission.outputs[0], output.inputs[0])
SCENE = bpy.context.scene
CAM = SCENE.camera


def aim(room, pitch, yaw):
    """エンジンの舞台镜头と同じ：先に側角（上下軸回り）、次に俯角（左右軸回り）。行は画面で水平のまま。"""
    t, p = math.radians(pitch), math.radians(yaw)
    right = Vector((math.cos(p), math.sin(p) * math.cos(t), -math.sin(p) * math.sin(t)))
    up = Vector((0.0, math.sin(t), math.cos(t)))
    forward = Vector((-math.sin(p), math.cos(p) * math.cos(t), -math.cos(p) * math.sin(t)))
    rotation = Matrix((right, up, -forward)).transposed()
    assert abs(rotation.determinant() - 1) < 1e-6
    target = Vector((room["x"] + room["cols"] / 2, 0.0, room["rows"] / 2))
    CAM.matrix_world = Matrix.Translation(target - 30 * forward) @ rotation.to_4x4()
    CAM.data.ortho_scale = room["cols"]


def render(path, data=False):
    SCENE.render.filter_size = 0.0 if data else 1.2
    SCENE.eevee.taa_render_samples = 1 if data else 16
    SCENE.view_layers[0].material_override = IDM if data else None
    fmt = SCENE.render.image_settings
    fmt.file_format = "OPEN_EXR" if data else "PNG"
    if data:
        fmt.color_depth = "32"
    else:
        fmt.color_mode = "RGB"
        fmt.color_depth = "8"
    SCENE.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)


def compare(name, color, data):
    old, new = S["style_sketch"](color, data), style_sketch_depth(color, data)
    S["save"](OUT / f"{name}_now.png", old)
    S["save"](OUT / f"{name}_depth.png", new)
    return old, new


pairs = []
for key in ("A", "GALLERY", "TV2"):
    color, data = S["load"](SOURCE / f"{key}_full_color.png"), S["load"](SOURCE / f"{key}_full_data.exr")
    pairs.append((f"{key}_p7", *compare(f"{key}_p7", color, data)))
    print("STYLED", key)
for pitch, yaw in ANGLES:
    for key in ("GALLERY", "A"):
        room = S["ROOMS"][key]
        SCENE.render.resolution_x, SCENE.render.resolution_y = room["cols"] * S["PX_FULL"], room["rows"] * S["PX_FULL"]
        aim(room, pitch, yaw)
        name = f"{key}_p{int(pitch)}_y{int(yaw)}"
        render(OUT / f"{name}_color.png")
        render(OUT / f"{name}_data.exr", data=True)
        color, data = S["load"](OUT / f"{name}_color.png"), S["load"](OUT / f"{name}_data.exr")
        pairs.append((name, *compare(name, color, data)))
        print("RENDERED", name)

# 見本帳：左が現行、右が纵深。上から A・GALLERY（7°）、GALLERY と A の角度付き。
order = ["A_p7", "GALLERY_p7"] + [f"{k}_p{int(p)}_y{int(y)}" for p, y in ANGLES for k in ("GALLERY", "A")]
lookup = {name: (old, new) for name, old, new in pairs}
rows = []
for name in order:
    old, new = lookup[name]
    rows.append(np.hstack([old, np.full((old.shape[0], 12, 3), 0.5, np.float32), new]))
    rows.append(np.full((12, rows[-1].shape[1], 3), 0.5, np.float32))
S["save"](OUT / "SHEET.png", np.vstack(rows[:-1]))
print("DONE", OUT)
