# -*- coding: utf-8 -*-
"""既存セットを変更せず、独立したテレビと配置用支持体を作成する。"""

import json
import math
from pathlib import Path

import bmesh
import bpy
from mathutils import Matrix, Vector
from io_scene_fbx import parse_fbx

ROOT = Path(__file__).resolve().parent
EXPORTS, PREVIEWS = ROOT / "Exports", ROOT / "Previews"
for directory in (EXPORTS, PREVIEWS, ROOT / "Temp"):
    directory.mkdir(parents=True, exist_ok=True)
assert bpy.app.background, "Run in an isolated background Blender process"
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.filepaths.save_version = 0
bpy.context.preferences.filepaths.temporary_directory = str(ROOT / "Temp")


def rgb(value):
    values = [int(value[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    return tuple(v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in values)


def material(name, hex_color, roughness=0.65, metallic=0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.diffuse_color = (*rgb(hex_color), 1)
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = mat.diffuse_color
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Emission Strength"].default_value = 0
    mat.use_backface_culling = True
    return mat


# Set01 と同じ低彩度パレット。画面材質は筐体へ割り当てない。
MATS = {
    "charcoal": material("TD2_Charcoal", "343C39", 0.54),
    "sage": material("TD2_Sage", "657167", 0.68),
    "taupe": material("TD2_Taupe", "8B8173", 0.72),
    "mineral": material("TD2_Mineral", "AAA89B", 0.86),
    "metal": material("TD2_DarkMetal", "444C48", 0.40, 0.65),
    "recess": material("TD2_Recess", "1D2421", 0.85),
    "screen": material("TD2_Screen", "687972", 1.0),
}
# 既存導入レポートには自発光チャンネルがある。新画面は暗部を穏やかに持ち上げる。
# 動的なゲーム画像への自発光テクスチャ割当は、実際のエンジン統合時に確認する。
screen_bsdf = MATS["screen"].node_tree.nodes.get("Principled BSDF")
screen_bsdf.inputs["Specular IOR Level"].default_value = 0.0
screen_bsdf.inputs["Emission Color"].default_value = (*rgb("687972"), 1)
screen_bsdf.inputs["Emission Strength"].default_value = 0.65
ASSETS, CURRENT = [], None


def select(objects):
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]


def begin(identifier, title, scale_range, screen=None, support=None):
    global CURRENT
    scene = bpy.data.scenes.new(identifier)
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1
    collection = bpy.data.collections.new(identifier)
    scene.collection.children.link(collection)
    bpy.context.window.scene = scene
    root = bpy.data.objects.new(identifier, None)
    collection.objects.link(root)
    root.empty_display_size = 0.10
    root["pivot"] = "bottom_center"
    root["export_axes"] = "+Y up / +Z front"
    CURRENT = {"id": identifier, "title": title, "scene": scene, "collection": collection,
               "root": root, "parts": [], "screen": screen, "support": support,
               "suggested_uniform_scale": scale_range}
    ASSETS.append(CURRENT)


def register(obj, name, mat):
    for collection in list(obj.users_collection):
        collection.objects.unlink(obj)
    CURRENT["collection"].objects.link(obj)
    obj.name = CURRENT["id"] + "__" + name
    obj.data.materials.append(MATS[mat])
    CURRENT["parts"].append(obj)
    return obj


def finish(obj, bevel=0, segments=2):
    select([obj])
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    if bevel:
        modifier = obj.modifiers.new("Edge finish", "BEVEL")
        modifier.width, modifier.segments = bevel, segments
        modifier.limit_method = "ANGLE"
        modifier.harden_normals = True
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    for face in obj.data.polygons:
        face.use_smooth = True
    modifier = obj.modifiers.new("Planar normals", "WEIGHTED_NORMAL")
    modifier.keep_sharp = True
    modifier.weight = 50
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    return obj


def box(name, size, center, mat, bevel=0.006, segments=2):
    bpy.ops.mesh.primitive_cube_add(size=1, location=center)
    obj = register(bpy.context.object, name, mat)
    obj.dimensions = size
    # 薄板の中央でベベルが衝突しないように、厚みに対する上限を設ける。
    return finish(obj, min(bevel, min(size) * 0.45), segments)


def knob(name, radius, center, mat):
    bpy.ops.mesh.primitive_cylinder_add(vertices=20, radius=radius, depth=0.014, location=center)
    obj = register(bpy.context.object, name, mat)
    obj.rotation_euler.x = math.pi / 2
    return finish(obj, 0.002)


def mesh(name, vertices, faces, mat, closed=True):
    data = bpy.data.meshes.new(CURRENT["id"] + "__" + name)
    data.from_pydata(vertices, [], faces)
    data.update()
    if closed:
        bm = bmesh.new()
        bm.from_mesh(data)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bm.to_mesh(data)
        bm.free()
    return register(bpy.data.objects.new(data.name, data), name, mat)


def bezel(outer, outer_center, front, back, mat):
    spec = CURRENT["screen"]
    vertices = []
    # 画面の各辺に 8 mm の余白を残し、角丸や縁による画像の切り落としを防ぐ。
    inner = (spec["width_m"] + 0.016, spec["height_m"] + 0.016)
    inner_center = (spec["center_x_m"], spec["bottom_m"] + spec["height_m"] / 2)
    for depth, size, center in ((front, outer, outer_center), (front, inner, inner_center),
                                (back, outer, outer_center), (back, inner, inner_center)):
        w, h = size
        x, z = center
        vertices.extend([(x - w / 2, depth, z - h / 2), (x + w / 2, depth, z - h / 2),
                         (x + w / 2, depth, z + h / 2), (x - w / 2, depth, z + h / 2)])
    faces = []
    for i in range(4):
        n = (i + 1) % 4
        faces.extend([(i, n, n + 4, i + 4), (n + 8, i + 8, i + 12, n + 12),
                      (n, i, i + 8, n + 8), (i + 4, n + 4, n + 12, i + 12)])
    finish(mesh("Bezel", vertices, faces, mat), 0.003, 2)


def add_screen():
    spec = CURRENT["screen"]
    x, w, h = spec["center_x_m"], spec["width_m"], spec["height_m"]
    y, z = -spec["front_z_m"], spec["bottom_m"]
    # Blender -Y 正面を FBX +Z 正面へ変換する。画像用の四角形は常に平面。
    obj = mesh("Screen", [(x - w / 2, y, z), (x + w / 2, y, z),
                           (x + w / 2, y, z + h), (x - w / 2, y, z + h)],
               [(0, 1, 2, 3)], "screen", closed=False)
    uv = obj.data.uv_layers.new(name="UVMap")
    for loop in obj.data.loops:
        uv.data[loop.index].uv = ((0, 0), (1, 0), (1, 1), (0, 1))[loop.vertex_index]
    obj["screen_material"] = "TD2_Screen"
    obj["active_size_m"] = [w, h]
    spec["mesh"] = obj.name
    spec["material"] = "TD2_Screen"
    columns, rows = spec["suggested_grid_cells"]
    assert abs(w / h - columns / rows) < 1e-6
    spec["suggested_resolution_px"] = [columns * 64, rows * 64]
    spec["bounds_min_xyz_m"] = [x - w / 2, z, -y]
    spec["bounds_max_xyz_m"] = [x + w / 2, z + h, -y]


def end():
    entry = CURRENT
    parts = [obj for obj in entry["parts"] if not obj.name.endswith("__Screen")]
    select(parts)
    if len(parts) > 1:
        bpy.ops.object.join()
    body = bpy.context.object
    body.name = entry["id"] + "__Body"
    body.data.name = body.name
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=1.1519173, island_margin=0.025)
    bpy.ops.object.mode_set(mode="OBJECT")
    objects = [obj for obj in entry["collection"].objects if obj.type == "MESH"]
    triangles = 0
    for obj in objects:
        obj.data.transform(obj.matrix_world.copy())
        obj.matrix_world = Matrix.Identity(4)
        obj.data.calc_loop_triangles()
        triangles += len(obj.data.loop_triangles)
        assert len(obj.data.materials) > 0 and all(obj.data.materials)
        assert all(0 <= p.material_index < len(obj.data.materials) for p in obj.data.polygons)
        assert all(t.area > 1e-12 for t in obj.data.loop_triangles), obj.name
        assert all(-1e-5 <= v <= 1.00001 for loop in obj.data.uv_layers.active.data for v in loop.uv)
    points = [v.co for obj in objects for v in obj.data.vertices]
    low = [min(p[i] for p in points) for i in range(3)]
    high = [max(p[i] for p in points) for i in range(3)]
    assert abs(low[2]) < 1e-5
    assert abs(low[0] + high[0]) < 1e-5 and abs(low[1] + high[1]) < 1e-5
    entry["dimensions_m"] = [round(high[i] - low[i], 5) for i in (0, 2, 1)]
    entry["triangles"] = triangles
    entry["meshes"] = objects
    entry["fbx"] = EXPORTS / (entry["id"] + ".fbx")
    select(objects)
    # 親を含めず軸変換を頂点へ焼き込み、FBX のノード変換を単位行列に保つ。
    bpy.ops.export_scene.fbx(filepath=str(entry["fbx"]), use_selection=True, object_types={"MESH"},
                            global_scale=1.0, apply_unit_scale=True, apply_scale_options="FBX_SCALE_UNITS",
                            axis_forward="-Z", axis_up="Y", use_space_transform=True,
                            bake_space_transform=True, use_mesh_modifiers=True, mesh_smooth_type="OFF",
                            use_triangles=True, use_tspace=False, bake_anim=False, add_leaf_bones=False,
                            use_custom_props=True, path_mode="AUTO", embed_textures=False)
    for obj in objects:
        obj.parent = entry["root"]
        obj.matrix_parent_inverse = Matrix.Identity(4)


begin("SM_TV_PortableCRT", "01 / PORTABLE CRT", [0.85, 1.6],
      {"width_m": 0.48, "height_m": 0.36, "center_x_m": -0.06, "bottom_m": 0.11,
       "front_z_m": 0.219, "suggested_grid_cells": [16, 12]})
box("Foot", (0.61, 0.48, 0.040), (0, 0, 0.020), "charcoal", 0.009)
box("Case", (0.70, 0.42, 0.485), (0, 0, 0.2775), "sage", 0.025, 3)
box("RearCap", (0.61, 0.038, 0.37), (0, 0.221, 0.285), "charcoal", 0.012)
bezel((0.69, 0.45), (0, 0.2825), -0.231, -0.205, "taupe")
add_screen()
knob("DialUpper", 0.025, (0.25, -0.232, 0.371), "charcoal")
knob("DialLower", 0.019, (0.25, -0.232, 0.271), "metal")
for z in (0.143, 0.168, 0.193):
    box("SpeakerSlot", (0.070, 0.004, 0.009), (0.25, -0.234, z), "recess", 0.001)
for x in (-0.16, 0.16):
    box("HandleSide", (0.04, 0.055, 0.11), (x, 0, 0.565), "charcoal", 0.012, 3)
box("HandleTop", (0.36, 0.055, 0.050), (0, 0, 0.625), "charcoal", 0.012, 3)
end()

begin("SM_TV_IndustrialPortrait", "02 / PORTRAIT MONITOR", [0.8, 1.35],
      {"width_m": 0.54, "height_m": 0.96, "center_x_m": 0.0, "bottom_m": 0.135,
       "front_z_m": 0.114, "suggested_grid_cells": [9, 16]})
box("Base", (0.76, 0.36, 0.04), (0, 0, 0.02), "metal", 0.006)
box("Housing", (0.65, 0.20, 1.085), (0, 0, 0.615), "sage", 0.012)
bezel((0.68, 1.08), (0, 0.615), -0.126, -0.092, "charcoal")
add_screen()
for x in (-0.36, 0.36):
    box("ProtectiveRail", (0.03, 0.09, 1.16), (x, 0, 0.62), "metal", 0.005)
box("Cap", (0.76, 0.26, 0.034), (0, 0, 1.203), "metal", 0.005)
box("RearPanel", (0.52, 0.018, 0.94), (0, 0.106, 0.615), "charcoal", 0.007)
box("LowerSwitch", (0.037, 0.008, 0.018), (0.257, -0.128, 0.10), "metal", 0.003)
end()

begin("SM_TV_WideRetro", "03 / WIDE RETRO SET", [0.8, 1.3],
      {"width_m": 0.96, "height_m": 0.54, "center_x_m": 0.0, "bottom_m": 0.18,
       "front_z_m": 0.302, "suggested_grid_cells": [16, 9]})
for x in (-0.48, 0.48):
    box("FootRail", (0.16, 0.66, 0.11), (x, 0, 0.055), "charcoal", 0.012)
box("Cabinet", (1.40, 0.60, 0.725), (0, 0, 0.4375), "taupe", 0.024, 3)
box("RearCap", (1.20, 0.030, 0.50), (0, 0.305, 0.43), "charcoal", 0.010)
bezel((1.36, 0.675), (0, 0.4475), -0.314, -0.283, "charcoal")
add_screen()
for x in (-0.587, 0.587):
    box("SpeakerInset", (0.115, 0.004, 0.395), (x, -0.316, 0.454), "recess", 0.001)
    for offset in (-0.039, -0.013, 0.013, 0.039):
        box("Grille", (0.009, 0.008, 0.375), (x + offset, -0.320, 0.454), "taupe", 0.002)
for x in (0.31, 0.40):
    knob("Control", 0.017, (x, -0.315, 0.139), "metal")
end()

begin("SM_Block_Stack", "04 / STACKING BLOCK", [0.75, 2.0], support={"top_height_m": 0.64})
box("RecessedFoot", (1.10, 0.70, 0.05), (0, 0, 0.025), "charcoal", 0.006)
box("Mass", (1.20, 0.80, 0.61), (0, 0, 0.335), "mineral", 0.009)
end()

begin("SM_Plinth_Wedge", "05 / INCLINED PLINTH", [0.8, 1.5],
      support={"top_center_xyz_m": [0, 0.325, 0], "top_plane": "Y = 0.325 - 0.3125 * Z",
               "inclination_deg": round(math.degrees(math.atan(0.3125)), 5),
               "reference_tv_x_rotation_deg": round(math.degrees(math.atan(0.3125)), 5)})
box("RecessedFoot", (0.92, 0.72, 0.045), (0, 0, 0.0225), "charcoal", 0.006)
section = [(-0.40, 0.03), (0.40, 0.03), (0.40, 0.45), (-0.40, 0.20)]
vertices = [(x, y, z) for x in (-0.50, 0.50) for y, z in section]
faces = [(3, 2, 1, 0), (4, 5, 6, 7)]
faces.extend((i, (i + 1) % 4, 4 + (i + 1) % 4, 4 + i) for i in range(4))
wedge = mesh("Wedge", vertices, faces, "sage")
wedge.data.materials.append(MATS["taupe"])
wedge.data.polygons[4].material_index = 1
finish(wedge, 0.006)
end()

begin("SM_Frame_Cantilever", "06 / CANTILEVER FRAME", [0.8, 1.5],
      support={"top_height_m": 1.20, "clear_opening_height_m": 1.02,
               "clear_opening_width_m": 1.015, "upright_side": "-X"})
box("Counterweight", (1.20, 0.72, 0.10), (0, 0, 0.05), "charcoal", 0.012)
box("Upright", (0.15, 0.60, 1.09), (-0.49, 0, 0.585), "sage", 0.008)
box("Cantilever", (1.20, 0.72, 0.08), (0, 0, 1.16), "taupe", 0.009)
end()


def aim(obj, target):
    obj.rotation_euler = (Vector(target) - obj.location).to_track_quat("-Z", "Y").to_euler()


ground_mat = material("Preview_Only_Ground", "D7D8D2", 0.9)
for entry in ASSETS:
    scene = entry["scene"]
    bpy.context.window.scene = scene
    helpers = bpy.data.collections.new(entry["id"] + "__PreviewOnly")
    scene.collection.children.link(helpers)
    data = bpy.data.meshes.new("Studio ground")
    data.from_pydata([(-100, -100, -0.004), (100, -100, -0.004),
                     (100, 100, -0.004), (-100, 100, -0.004)], [], [(0, 1, 2, 3)])
    obj = bpy.data.objects.new("Studio ground - not exported", data)
    helpers.objects.link(obj)
    data.materials.append(ground_mat)
    w, h, d = entry["dimensions_m"]
    target = Vector((0, 0, h / 2))
    camera_data = bpy.data.cameras.new("Studio camera")
    camera = bpy.data.objects.new("Studio camera - not exported", camera_data)
    helpers.objects.link(camera)
    camera.location = target + Vector((2.8, -6.5, 3.1)).normalized() * 8
    aim(camera, target)
    inv = camera.rotation_euler.to_matrix().transposed()
    projected = [inv @ (Vector((x, y, z)) - target) for x in (-w / 2, w / 2)
                 for y in (-d / 2, d / 2) for z in (0, h)]
    span_x = max(v.x for v in projected) - min(v.x for v in projected)
    span_y = max(v.y for v in projected) - min(v.y for v in projected)
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = max(span_x, span_y * 1.4) * 1.25
    scene.camera = camera
    for name, location, energy, size, color in (
        ("Key", (-3, -4, 6), 680, 4.5, (1, 0.94, 0.84)),
        ("Fill", (4, -1, 4), 420, 4, (0.84, 0.91, 1)),
        ("Rim", (1, 4, 5), 780, 3.5, (1, 0.97, 0.92)),
    ):
        light = bpy.data.lights.new(name, "AREA")
        light.energy, light.size, light.color = energy, size, color
        obj = bpy.data.objects.new(name + " - not exported", light)
        helpers.objects.link(obj)
        obj.location = location
        aim(obj, target)
    world = bpy.data.worlds.new(entry["id"] + "__StudioWorld")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.42, 0.46, 0.44, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.32
    scene.world = world
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 40
    scene.cycles.use_denoising = True
    scene.render.resolution_x, scene.render.resolution_y = 980, 700
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.view_settings.view_transform = "AgX"
    entry["preview"] = PREVIEWS / (entry["id"] + ".png")
    scene.render.filepath = str(entry["preview"])
    bpy.ops.render.render(write_still=True)
    for obj in helpers.objects:
        obj.hide_select = True
    select([entry["root"]])


# 部品の比較用一覧。撮影補助と画像は FBX に含めない。
board = bpy.data.scenes.new("00_ModularSet02_Overview")
bpy.context.window.scene = board


def flat_material(name, color, image=None):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    nodes.clear()
    output, surface = nodes.new("ShaderNodeOutputMaterial"), nodes.new("ShaderNodeEmission")
    surface.inputs["Color"].default_value = (*rgb(color), 1)
    mat.node_tree.links.new(surface.outputs[0], output.inputs["Surface"])
    if image:
        tex = nodes.new("ShaderNodeTexImage")
        tex.image = image
        mat.node_tree.links.new(tex.outputs["Color"], surface.inputs["Color"])
    return mat


def panel(name, x, y, w, h, mat, z=0):
    data = bpy.data.meshes.new(name)
    data.from_pydata([(x, y, z), (x + w, y, z), (x + w, y + h, z), (x, y + h, z)], [], [(0, 1, 2, 3)])
    obj = bpy.data.objects.new(name, data)
    board.collection.objects.link(obj)
    data.materials.append(mat)
    uv = data.uv_layers.new(name="UVMap")
    for i, p in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[i].uv = p


def label(body, x, y, size, mat):
    data = bpy.data.curves.new(body, "FONT")
    data.body, data.size, data.space_character = body, size, 1.07
    obj = bpy.data.objects.new(body, data)
    board.collection.objects.link(obj)
    obj.location = (x, y, 0.02)
    data.materials.append(mat)


paper = flat_material("Preview_Paper", "ECEDE7")
ink = flat_material("Preview_Ink", "253D34")
muted = flat_material("Preview_Muted", "737C72")
panel("Paper", 0, 0, 25.2, 17.2, paper, -0.01)
label("TD2 / MODULAR SET 02", 0.75, 16.11, 0.58, ink)
label("DISPLAY VARIATIONS + STACKING SUPPORTS   /   SIX INDEPENDENT FBX ASSETS", 0.77, 15.54, 0.21, muted)
panel("Header rule", 0.77, 15.17, 23.66, 0.018, muted)
for index, entry in enumerate(ASSETS):
    x, top = 0.7 + (index % 3) * 8.15, 14.88 - (index // 3) * 6.85
    image = bpy.data.images.load(str(entry["preview"]))
    image.pack()
    mat = flat_material(entry["id"] + "__PreviewImage", "FFFFFF", image)
    panel(entry["id"], x, top - 5.55, 7.77, 5.55, mat)
    label(entry["title"], x + 0.1, top - 5.91, 0.245, ink)
    w, h, d = entry["dimensions_m"]
    label("%.3f W / %.3f H / %.3f D m" % (w, h, d), x + 0.1, top - 6.25, 0.19, muted)
    if entry["screen"]:
        s = entry["screen"]
        extra = "SCREEN %.2f x %.2f m / %d x %d CELLS" % (s["width_m"], s["height_m"], *s["suggested_grid_cells"])
    else:
        extra = {"SM_Block_Stack": "FLAT TOP +0.64 m", "SM_Plinth_Wedge": "INCLINE 17.35 DEG / CENTER +0.325 m",
                 "SM_Frame_Cantilever": "FLAT TOP +1.20 m / OPEN SIDE +X"}[entry["id"]]
    label(extra, x + 0.1, top - 6.57, 0.18, muted)
panel("Footer rule", 0.77, 0.96, 23.66, 0.016, muted)
label("+Z FRONT / +Y UP / METERS / BOTTOM-CENTER ORIGINS", 0.78, 0.48, 0.19, muted)
label("TD2_Screen / FULL RECTANGLE UV / SOFT SCREEN EMISSION", 12.85, 0.48, 0.19, muted)
data = bpy.data.cameras.new("Contact sheet camera")
camera = bpy.data.objects.new("Contact sheet camera", data)
board.collection.objects.link(camera)
camera.location = (12.6, 8.6, 10)
data.type, data.ortho_scale = "ORTHO", 25.2
board.camera = camera
board.render.engine = "CYCLES"
board.cycles.samples, board.cycles.use_denoising = 1, False
board.render.resolution_x, board.render.resolution_y = 2520, 1720
board.render.resolution_percentage = 100
board.view_settings.view_transform = "Standard"
board.render.image_settings.file_format = "PNG"
board.render.filepath = str(PREVIEWS / "ModularSet02_ContactSheet.png")
bpy.ops.render.render(write_still=True)
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / "ModularSet02.blend"))

manifest = {
    "schema_version": 1, "set": "ModularSet02", "source": str(ROOT / "ModularSet02.blend"),
    "preview": str(PREVIEWS / "ModularSet02_ContactSheet.png"), "engine_import_verified": False,
    "conventions": {"units": "meters", "up": "+Y", "front": "+Z", "pivot": "bottom_center",
                    "dimension_order": "width_height_depth", "fbx_unit_scale_factor_cm": 100,
                    "node_transforms": "identity", "screen_material": "TD2_Screen",
                    "fbx_uv": "bottom-left 0,0; top-right 1,1", "default_cell_px": [64, 64],
                    "collision_changes_with_visual_scale": False},
    "notes": ["Scale ranges are visual composition suggestions; gameplay geometry stays separately configured.",
              "Portable CRT describes the case silhouette; its active screen is planar.",
              "Slope placement reference assumes usual right-handed X rotation; verify editor convention.",
              "Supports are static props, not physical stability or collision definitions.",
              "Do not import the blend collection as a combined scene. Import each FBX separately."],
    "screen_appearance": {"base_color_srgb_hex": "687972", "metallic": 0, "roughness": 1,
                          "specular_ior_level": 0, "emission_color_linear": list(rgb("687972")),
                          "emission_strength": 0.65,
                          "existing_set01_import_report": "TD2_Screen emissiveFactor [0,0,0], roughness 0.48",
                          "runtime_note": "Verify game render texture feeds emission or an unlit screen material; constant FBX emission alone does not guarantee shadow-free game pixels.",
                          "hikari_emission_roundtrip_verified": False},
    "assets": [],
}
for entry in ASSETS:
    manifest["assets"].append({"id": entry["id"], "file": str(entry["fbx"]),
                               "dimensions_m": entry["dimensions_m"], "triangles": entry["triangles"],
                               "mesh_names": [obj.name for obj in entry["meshes"]],
                               "materials": sorted({m.name for obj in entry["meshes"] for m in obj.data.materials}),
                               "screen": entry["screen"], "support": entry["support"],
                               "suggested_uniform_scale": entry["suggested_uniform_scale"]})


def child(element, name):
    return next(item for item in element.elems if item.id == name)


# 最終 FBX の材質接続・軸・単位・面割当と、一回の Blender 回読を確認する。
for entry in manifest["assets"]:
    document, _ = parse_fbx.parse(entry["file"])
    settings = {p.props[0]: p.props[-1] for p in child(child(document, b"GlobalSettings"), b"Properties70").elems}
    assert settings[b"UnitScaleFactor"] == 100 and settings[b"UpAxis"] == 1 and settings[b"UpAxisSign"] == 1
    objects = child(document, b"Objects").elems
    models = [e for e in objects if e.id == b"Model"]
    materials = [e for e in objects if e.id == b"Material"]
    connections = child(document, b"Connections").elems
    positions = []
    for mat in materials:
        if b"TD2_Screen" in mat.props[1]:
            props = {p.props[0]: p.props[4:] for p in child(mat, b"Properties70").elems}
            assert props[b"EmissiveFactor"][0] > 0 and sum(props[b"EmissiveColor"]) > 0
    for model in models:
        properties = {p.props[0]: p.props[-3:] for p in child(model, b"Properties70").elems}
        for key, expected in ((b"Lcl Translation", 0), (b"Lcl Rotation", 0), (b"Lcl Scaling", 1)):
            assert all(abs(v - expected) < 1e-5 for v in properties.get(key, [expected] * 3))
    for geo in (e for e in objects if e.id == b"Geometry"):
        model_id = next(c.props[2] for c in connections if c.props[1] == geo.props[0])
        linked_materials = [c.props[1] for c in connections if c.props[2] == model_id and c.props[1] in {m.props[0] for m in materials}]
        face_slots = child(child(geo, b"LayerElementMaterial"), b"Materials").props[0]
        assert len(linked_materials) > 0 and all(0 <= index < len(linked_materials) for index in face_slots)
        verts = child(geo, b"Vertices").props[0]
        points = list(zip(verts[0::3], verts[1::3], verts[2::3]))
        positions.extend(points)
        if b"__Screen" in geo.props[1]:
            assert all(abs(p[2] - entry["screen"]["front_z_m"]) < 1e-5 for p in points)
            normal = child(child(geo, b"LayerElementNormal"), b"Normals").props[0]
            assert all(abs(n) < 1e-5 for n in normal[0::3]) and all(abs(n) < 1e-5 for n in normal[1::3])
            assert all(abs(n - 1) < 1e-5 for n in normal[2::3])
    measured = [max(p[i] for p in positions) - min(p[i] for p in positions) for i in range(3)]
    assert all(abs(a - b) < 1e-5 for a, b in zip(measured, entry["dimensions_m"]))
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=entry["file"], use_anim=False)
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    assert sorted(obj.name for obj in meshes) == sorted(entry["mesh_names"])
    count = 0
    for obj in meshes:
        obj.data.calc_loop_triangles()
        count += len(obj.data.loop_triangles)
        assert all(abs(v - 1) < 1e-5 for v in obj.scale)
        assert all(0 <= p.material_index < len(obj.data.materials) for p in obj.data.polygons)
        assert all(math.isfinite(v) for vertex in obj.data.vertices for v in vertex.normal)
        if obj.name.endswith("__Screen"):
            assert [m.name for m in obj.data.materials] == ["TD2_Screen"]
            shader = obj.data.materials[0].node_tree.nodes.get("Principled BSDF")
            assert sum(shader.inputs["Emission Color"].default_value[:3]) > 0
            spec = entry["screen"]
            for loop in obj.data.loops:
                p = obj.matrix_world @ obj.data.vertices[loop.vertex_index].co
                uv = obj.data.uv_layers.active.data[loop.index].uv
                assert abs(uv.x - ((p.x - spec["center_x_m"]) / spec["width_m"] + 0.5)) < 1e-5
                assert abs(uv.y - (p.z - spec["bottom_m"]) / spec["height_m"]) < 1e-5
        else:
            assert all(m.name != "TD2_Screen" for m in obj.data.materials)
    assert count == entry["triangles"]
    entry["fbx_roundtrip_verified"] = True
    print("FBX_READY", entry["id"], entry["dimensions_m"], count, flush=True)
(ROOT / "asset_manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print("MODULAR_SET02_COMPLETE", len(manifest["assets"]), flush=True)
