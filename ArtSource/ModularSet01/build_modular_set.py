# -*- coding: utf-8 -*-
"""TD2_TV の独立した背景部品を、専用のバックグラウンド Blender で生成する。"""

import json
import math
from pathlib import Path

import bpy
from mathutils import Matrix, Vector


ROOT = Path(__file__).resolve().parent
EXPORTS = ROOT / "Exports"
PREVIEWS = ROOT / "Previews"
for directory in (EXPORTS, PREVIEWS, ROOT / "Temp"):
    directory.mkdir(parents=True, exist_ok=True)

# 対話中の Blender は使用せず、起動直後の専用プロセスだけを初期化する。
if not bpy.app.background:
    raise RuntimeError("Run with --background --factory-startup only")
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.filepaths.save_version = 0
bpy.context.preferences.filepaths.temporary_directory = str(ROOT / "Temp")


def linear_hex(value):
    channels = [int(value[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    return tuple(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in channels)


def material(name, color, roughness=0.65, metallic=0.0):
    result = bpy.data.materials.new(name)
    result.use_nodes = True
    rgba = (*linear_hex(color), 1.0)
    result.diffuse_color = rgba
    surface = result.node_tree.nodes.get("Principled BSDF")
    surface.inputs["Base Color"].default_value = rgba
    surface.inputs["Roughness"].default_value = roughness
    surface.inputs["Metallic"].default_value = metallic
    surface.inputs["Emission Strength"].default_value = 0.0
    result.use_backface_culling = True
    return result


MATERIALS = {
    "charcoal": material("TD2_Charcoal", "343C39", 0.54),
    "sage": material("TD2_Sage", "657167", 0.68),
    "taupe": material("TD2_Taupe", "8B8173", 0.72),
    "mineral": material("TD2_Mineral", "AAA89B", 0.86),
    "metal": material("TD2_DarkMetal", "444C48", 0.40, 0.65),
    "recess": material("TD2_Recess", "1D2421", 0.85),
    "diffuser": material("TD2_Diffuser", "C3BEA9", 0.58),
    # この名前はランタイムの画面割り当てに使用するため、変更・共有先追加をしない。
    "screen": material("TD2_Screen", "3E5550", 0.48),
}
ASSETS = []
CURRENT = None


def select_only(objects):
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]


def asset(identifier, title, purpose, screen=None):
    global CURRENT
    scene = bpy.data.scenes.new(identifier)
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0
    collection = bpy.data.collections.new(identifier)
    scene.collection.children.link(collection)
    bpy.context.window.scene = scene
    root = bpy.data.objects.new(identifier, None)
    collection.objects.link(root)
    root.empty_display_type = "PLAIN_AXES"
    root.empty_display_size = 0.15
    root["asset_id"] = identifier
    root["export_front"] = "+Z"
    root["export_up"] = "+Y"
    root["origin"] = "bottom_center"
    root["unit_meters"] = 1.0
    CURRENT = {"id": identifier, "title": title, "purpose": purpose,
               "scene": scene, "collection": collection, "root": root,
               "parts": [], "screen": screen}
    ASSETS.append(CURRENT)


def register(obj, name, mat):
    obj.name = CURRENT["id"] + "__" + name
    for old_collection in list(obj.users_collection):
        old_collection.objects.unlink(obj)
    CURRENT["collection"].objects.link(obj)
    obj.data.materials.append(MATERIALS[mat])
    CURRENT["parts"].append(obj)
    return obj


def finish_shape(obj, bevel=0.0, segments=2):
    select_only([obj])
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    if bevel:
        modifier = obj.modifiers.new("Small edge highlights", "BEVEL")
        modifier.width = bevel
        modifier.segments = segments
        modifier.limit_method = "ANGLE"
        modifier.harden_normals = True
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    modifier = obj.modifiers.new("Stable planar normals", "WEIGHTED_NORMAL")
    modifier.keep_sharp = True
    modifier.weight = 50
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    return obj


def box(name, dimensions, center, mat, bevel=0.006, segments=2):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=center)
    obj = register(bpy.context.object, name, mat)
    obj.dimensions = dimensions
    # 薄板の厚み中央でベベルが衝突するとゼロ面積の三角形が生じるため、余白を残す。
    return finish_shape(obj, min(bevel, min(dimensions) * 0.45), segments)


def cylinder(name, radius, depth, center, mat, vertices=32, bevel=0.003, front=False):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius,
                                      depth=depth, location=center)
    obj = register(bpy.context.object, name, mat)
    if front:
        obj.rotation_euler.x = math.pi / 2.0
    return finish_shape(obj, min(bevel, depth * 0.45, radius * 0.45))


def custom_mesh(name, vertices, faces, mat):
    mesh = bpy.data.meshes.new(CURRENT["id"] + "__" + name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(mesh.name, mesh)
    CURRENT["collection"].objects.link(obj)
    return register(obj, name, mat)


def frame(name, outer, inner, center_z, front, back, mat, bevel):
    # 内側開口は実画面より余裕を持たせ、四隅のベベルが画像を隠さないようにする。
    vertices = []
    for depth, size in ((front, outer), (front, inner), (back, outer), (back, inner)):
        w, h = size
        vertices.extend([(-w / 2, depth, center_z - h / 2),
                         (w / 2, depth, center_z - h / 2),
                         (w / 2, depth, center_z + h / 2),
                         (-w / 2, depth, center_z + h / 2)])
    faces = []
    for i in range(4):
        n = (i + 1) % 4
        faces.extend([(i, n, 4 + n, 4 + i),
                      (8 + n, 8 + i, 12 + i, 12 + n),
                      (n, i, 8 + i, 8 + n),
                      (4 + i, 4 + n, 12 + n, 12 + i)])
    return finish_shape(custom_mesh(name, vertices, faces, mat), bevel, 3)


def screen(width, height, bottom, front):
    # Blender の -Y 正面は glTF 出力時の +Z になる。UV は下左から順に定義する。
    obj = custom_mesh("Screen", [(-width / 2, front, bottom),
                                 (width / 2, front, bottom),
                                 (width / 2, front, bottom + height),
                                 (-width / 2, front, bottom + height)],
                      [(0, 1, 2, 3)], "screen")
    uv = obj.data.uv_layers.new(name="UVMap")
    coordinates = [(0, 0), (1, 0), (1, 1), (0, 1)]
    for loop in obj.data.loops:
        uv.data[loop.index].uv = coordinates[loop.vertex_index]
    obj["screen_material"] = "TD2_Screen"
    obj["active_width_m"] = width
    obj["active_height_m"] = height
    obj["front_axis_gltf"] = "+Z"
    return obj


def seal_asset():
    # 小部品は筐体へまとめ、画面だけは別 mesh として保持する。
    body_parts = [obj for obj in CURRENT["parts"] if not obj.name.endswith("__Screen")]
    select_only(body_parts)
    if len(body_parts) > 1:
        bpy.ops.object.join()
    body = bpy.context.object
    body.name = CURRENT["id"] + "__Body"
    body.data.name = body.name
    select_only([body])
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=1.1519173, island_margin=0.025)
    bpy.ops.object.mode_set(mode="OBJECT")
    meshes = [obj for obj in CURRENT["collection"].objects if obj.type == "MESH"]
    for obj in meshes:
        # 頂点へ変換を焼き込み、子 mesh も親も単位変換にそろえる。
        obj.data.transform(obj.matrix_world.copy())
        obj.matrix_world = Matrix.Identity(4)
        obj.parent = CURRENT["root"]
        obj.matrix_parent_inverse = Matrix.Identity(4)
    CURRENT["meshes"] = meshes
    bounds = [vertex.co for obj in meshes for vertex in obj.data.vertices]
    minimum = [min(p[i] for p in bounds) for i in range(3)]
    maximum = [max(p[i] for p in bounds) for i in range(3)]
    assert abs(minimum[2]) < 1e-5, (CURRENT["id"], minimum)
    assert abs(minimum[0] + maximum[0]) < 1e-5
    assert abs(minimum[1] + maximum[1]) < 1e-5
    CURRENT["dimensions_m"] = [round(maximum[0] - minimum[0], 5),
                               round(maximum[2] - minimum[2], 5),
                               round(maximum[1] - minimum[1], 5)]
    select_only([CURRENT["root"], *meshes])
    path = EXPORTS / (CURRENT["id"] + ".glb")
    bpy.ops.export_scene.gltf(filepath=str(path), export_format="GLB", use_selection=True,
                             use_active_scene=True,
                             export_yup=True, export_apply=True, export_extras=True,
                             export_cameras=False, export_lights=False, export_animations=False,
                             export_materials="EXPORT", export_texcoords=True,
                             export_normals=True, export_tangents=False)
    CURRENT["export"] = path


asset("SM_TV_16x9", "01  /  WIDE DISPLAY", "16:9 screen carrier",
      {"width_m": 1.6, "height_m": 0.9, "bottom_m": 0.27,
       "front_z_m": 0.104, "mesh": "SM_TV_16x9__Screen", "material": "TD2_Screen"})
box("Base", (0.80, 0.40, 0.055), (0, 0, 0.0275), "metal", 0.014, 3)
box("Stem", (0.18, 0.11, 0.17), (0, 0.025, 0.135), "charcoal", 0.008)
box("Shell", (1.72, 0.18, 1.04), (0, 0.012, 0.72), "charcoal", 0.021, 3)
box("BackPanel", (1.54, 0.015, 0.80), (0, 0.1095, 0.72), "sage", 0.012)
frame("Bezel", (1.74, 1.05), (1.616, 0.916), 0.72, -0.116, -0.078, "charcoal", 0.004)
screen(1.6, 0.9, 0.27, -0.104)
box("Badge", (0.036, 0.003, 0.004), (0, -0.118, 0.225), "sage", 0.001)
cylinder("Button", 0.004, 0.003, (0.745, -0.118, 0.224), "diffuser", 12, 0.0005, True)
for i in range(5):
    box("BackVent_%02d" % i, (0.23, 0.003, 0.008), (0, 0.118, 0.945 + i * 0.020), "recess", 0.002)
seal_asset()

asset("SM_TV_4x3", "02  /  COMPACT DISPLAY", "4:3 screen carrier",
      {"width_m": 1.2, "height_m": 0.9, "bottom_m": 0.265,
       "front_z_m": 0.247, "mesh": "SM_TV_4x3__Screen", "material": "TD2_Screen"})
box("Base", (0.96, 0.54, 0.055), (0, 0, 0.0275), "metal", 0.014, 3)
box("Stand", (0.32, 0.25, 0.15), (0, 0.025, 0.12), "charcoal", 0.014)
box("Shell", (1.42, 0.46, 1.105), (0, 0, 0.7175), "sage", 0.024, 3)
box("RearInset", (1.14, 0.012, 0.82), (0, 0.231, 0.76), "charcoal", 0.018)
frame("Bezel", (1.44, 1.105), (1.216, 0.916), 0.715, -0.259, -0.219, "charcoal", 0.004)
screen(1.2, 0.9, 0.265, -0.247)
for i in range(7):
    box("FrontVent_%02d" % i, (0.050, 0.004, 0.009), (-0.43 + i * 0.063, -0.261, 0.214), "recess", 0.002)
for x in (0.45, 0.53):
    cylinder("Control", 0.016, 0.014, (x, -0.257, 0.215), "metal", 20, 0.002, True)
for i in range(6):
    box("BackVent_%02d" % i, (0.34, 0.003, 0.012), (0, 0.239, 0.885 + i * 0.035), "recess", 0.003)
seal_asset()

asset("SM_Cabinet_Low", "03  /  LOW CABINET", "Low display cabinet")
for x in (-0.76, 0.76):
    for y in (-0.19, 0.19):
        box("Foot", (0.075, 0.075, 0.135), (x, y, 0.0675), "charcoal", 0.005)
box("Top", (1.90, 0.56, 0.05), (0, 0, 0.555), "taupe", 0.008)
box("Bottom", (1.86, 0.50, 0.045), (0, 0.015, 0.1575), "taupe", 0.006)
for x in (-0.9075, 0.9075):
    box("Side", (0.045, 0.50, 0.375), (x, 0.015, 0.35), "taupe", 0.005)
box("Back", (1.78, 0.025, 0.355), (0, 0.24, 0.3475), "sage", 0.003)
box("Partition", (0.035, 0.485, 0.355), (0, 0.015, 0.3475), "taupe", 0.004)
box("Door", (0.864, 0.035, 0.342), (-0.445, -0.236, 0.350), "sage", 0.006)
box("Pull", (0.24, 0.018, 0.015), (-0.445, -0.267, 0.460), "metal", 0.004)
box("OpenShelf", (0.862, 0.45, 0.024), (0.449, 0.015, 0.343), "taupe", 0.004)
seal_asset()

asset("SM_Plinth_Tall", "04  /  TALL PLINTH", "Narrow freestanding display pedestal")
box("Foot", (0.48, 0.48, 0.055), (0, 0, 0.0275), "charcoal", 0.007)
box("Column", (0.415, 0.415, 1.105), (0, 0, 0.6075), "sage", 0.007)
box("Reveal", (0.432, 0.432, 0.018), (0, 0, 1.151), "recess", 0.002)
box("Cap", (0.52, 0.52, 0.04), (0, 0, 1.18), "taupe", 0.006)
seal_asset()

asset("SM_Bench", "05  /  BENCH", "Simple seating and display surface")
box("Seat", (1.65, 0.42, 0.075), (0, 0, 0.4225), "taupe", 0.012, 3)
for x in (-0.615, 0.615):
    box("Foot", (0.15, 0.335, 0.032), (x, 0, 0.016), "charcoal", 0.005)
    box("SlabLeg", (0.125, 0.31, 0.37), (x, 0, 0.21), "sage", 0.008)
box("Stretcher", (1.17, 0.055, 0.06), (0, 0.02, 0.165), "metal", 0.005)
seal_asset()

asset("SM_Wall_2m", "06  /  WALL MODULE", "2 m repeatable wall with skirting")
# 巾木と壁を連続した断面にし、接触する重複面や重複エッジを作らない。
section = [(0.08, 0), (-0.08, 0), (-0.08, 0.15), (-0.045, 0.15),
           (-0.045, 0.164), (-0.04, 0.164), (-0.04, 2.8), (0.08, 2.8)]
vertices = [(x, y, z) for x in (-1, 1) for y, z in section]
faces = [tuple(range(8)), tuple(reversed(range(8, 16)))]
faces.extend((i, 8 + i, 8 + (i + 1) % 8, (i + 1) % 8) for i in range(8))
wall = custom_mesh("Panel", vertices, faces, "mineral")
wall.data.materials.append(MATERIALS["sage"])
wall.data.materials.append(MATERIALS["recess"])
for polygon in wall.data.polygons:
    if polygon.index in (2, 3, 4):
        polygon.material_index = 1
    elif polygon.index == 5:
        polygon.material_index = 2
finish_shape(wall, 0.0006)
seal_asset()

asset("SM_Floor_2m", "07  /  FLOOR MODULE", "2 m square tile, surface 0.08 m above origin")
floor = box("Slab", (2.0, 2.0, 0.08), (0, 0, 0.04), "sage", 0.0008)
floor.data.materials.append(MATERIALS["charcoal"])
for polygon in floor.data.polygons:
    polygon.material_index = 0 if polygon.normal.z > 0.5 else 1
seal_asset()

asset("SM_Lamp_Floor", "08  /  FLOOR LAMP", "Geometry-only lamp; add light in editor")
cylinder("Base", 0.21, 0.045, (0, 0, 0.0225), "metal", 40, 0.007)
cylinder("SocketBase", 0.035, 0.07, (0, 0, 0.072), "charcoal", 24, 0.004)
cylinder("Stem", 0.014, 1.395, (0, 0, 0.7625), "metal", 20, 0.0015)
cylinder("SocketTop", 0.030, 0.10, (0, 0, 1.43), "charcoal", 24, 0.002)
# シェードは内外面と縁を持つ閉じた薄肉形状にし、裏面消失を防ぐ。
vertices = []
for radius, height in ((0.205, 1.325), (0.125, 1.62), (0.193, 1.325), (0.113, 1.62)):
    vertices.extend([(radius * math.cos(i * math.tau / 40),
                      radius * math.sin(i * math.tau / 40), height) for i in range(40)])
faces = []
for i in range(40):
    j = (i + 1) % 40
    faces.extend([(i, j, 40 + j, 40 + i), (80 + j, 80 + i, 120 + i, 120 + j),
                  (j, i, 80 + i, 80 + j), (40 + i, 40 + j, 120 + j, 120 + i)])
shade = custom_mesh("Shade", vertices, faces, "sage")
shade.data.materials.append(MATERIALS["diffuser"])
for polygon in shade.data.polygons:
    polygon.material_index = 1 if polygon.index % 4 != 0 else 0
finish_shape(shade, 0.0015)
cylinder("Diffuser", 0.168, 0.009, (0, 0, 1.405), "diffuser", 40, 0.001)
seal_asset()

asset("SM_Shelf_Small", "09  /  OPEN SHELF", "Three-level freestanding shelf")
for x in (-0.374, 0.374):
    for y in (-0.125, 0.125):
        box("Post", (0.025, 0.025, 1.08), (x, y, 0.54), "metal", 0.003)
        box("Pad", (0.035, 0.035, 0.025), (x, y, 0.0125), "charcoal", 0.003)
for z in (0.135, 0.565, 1.045):
    box("Shelf", (0.82, 0.32, 0.035), (0, 0, z), "taupe", 0.005)
box("BackBrace", (0.75, 0.020, 0.10), (0, 0.13, 0.956), "sage", 0.003)
box("Divider", (0.024, 0.275, 0.394), (-0.17, 0, 0.780), "sage", 0.004)
seal_asset()


def studio_material(name, color):
    return material(name, color, 0.9)


def aim(obj, target):
    obj.rotation_euler = (Vector(target) - obj.location).to_track_quat("-Z", "Y").to_euler()


def studio_for(entry):
    scene = entry["scene"]
    bpy.context.window.scene = scene
    collection = bpy.data.collections.new(entry["id"] + "__PreviewOnly")
    scene.collection.children.link(collection)
    collection["export"] = False
    ground_mesh = bpy.data.meshes.new("Preview Ground")
    ground_mesh.from_pydata([(-200, -200, -0.006), (200, -200, -0.006),
                            (200, 200, -0.006), (-200, 200, -0.006)], [], [(0, 1, 2, 3)])
    ground = bpy.data.objects.new("Preview Ground - do not export", ground_mesh)
    collection.objects.link(ground)
    ground.data.materials.append(STUDIO_GROUND)
    width, height, depth = entry["dimensions_m"]
    center = Vector((0, 0, height / 2))
    direction = Vector((2.5, -6.5, 3.0)).normalized()
    if "Floor" in entry["id"]:
        direction = Vector((3, -5, 5.5)).normalized()
    camera_data = bpy.data.cameras.new("Preview camera")
    camera = bpy.data.objects.new("Preview camera - do not export", camera_data)
    collection.objects.link(camera)
    camera.location = center + direction * 10
    aim(camera, center)
    camera_data.type = "ORTHO"
    # 投影後の境界から構図を求め、縦長部品の上下切れを防ぐ。
    corners = [Vector((x, y, z)) for x in (-width / 2, width / 2)
               for y in (-depth / 2, depth / 2) for z in (0, height)]
    inv_rotation = camera.rotation_euler.to_matrix().transposed()
    projections = [inv_rotation @ (p - center) for p in corners]
    span_x = max(p.x for p in projections) - min(p.x for p in projections)
    span_y = max(p.y for p in projections) - min(p.y for p in projections)
    camera_data.ortho_scale = max(span_x, span_y * 1.3) * 1.27
    scene.camera = camera
    for name, location, power, size, color in (
        ("Key", (-3.5, -4.0, 6.5), 700, 5.0, (1.0, 0.94, 0.84)),
        ("Fill", (4, -1, 4), 450, 4.0, (0.83, 0.90, 1.0)),
        ("Rim", (1.5, 4, 5.0), 850, 3.0, (1.0, 0.97, 0.90)),
    ):
        data = bpy.data.lights.new("Preview " + name, "AREA")
        data.energy = power
        data.shape = "DISK"
        data.size = size
        data.color = color
        obj = bpy.data.objects.new(data.name + " - do not export", data)
        collection.objects.link(obj)
        obj.location = location
        aim(obj, center)
    world = bpy.data.worlds.new(entry["id"] + "__StudioWorld")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.42, 0.46, 0.44, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.32
    scene.world = world
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 40
    scene.cycles.use_denoising = True
    scene.cycles.max_bounces = 6
    scene.render.resolution_x = 780
    scene.render.resolution_y = 600
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.view_settings.view_transform = "AgX"
    scene.render.filepath = str(PREVIEWS / (entry["id"] + ".png"))
    bpy.ops.render.render(write_still=True)
    entry["preview"] = scene.render.filepath
    # 編集用ファイルを開いた時に補助物がモデル操作を妨げないようにする。
    for obj in collection.objects:
        obj.hide_select = True
    select_only([entry["root"]])


STUDIO_GROUND = studio_material("Preview_Only_Ground", "D7D8D2")
for entry in ASSETS:
    studio_for(entry)


def emission_material(name, rgba, image=None):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = rgba
    mat.node_tree.links.new(emission.outputs[0], output.inputs["Surface"])
    if image:
        texture = nodes.new("ShaderNodeTexImage")
        texture.image = image
        mat.node_tree.links.new(texture.outputs["Color"], emission.inputs["Color"])
    return mat


def board_rect(name, x, y, w, h, mat, z=0):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([(x, y, z), (x + w, y, z), (x + w, y + h, z), (x, y + h, z)], [], [(0, 1, 2, 3)])
    obj = bpy.data.objects.new(name, mesh)
    BOARD.collection.objects.link(obj)
    mesh.materials.append(mat)
    uv = mesh.uv_layers.new(name="UVMap")
    for i, co in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[i].uv = co
    return obj


def board_text(body, x, y, size, mat):
    curve = bpy.data.curves.new(body, "FONT")
    curve.body = body
    curve.size = size
    curve.space_character = 1.12
    curve.extrude = 0
    obj = bpy.data.objects.new(body, curve)
    BOARD.collection.objects.link(obj)
    obj.location = (x, y, 0.02)
    curve.materials.append(mat)


# 俯瞰の一括シーンにせず、各部品の形状を読み取れる同一照明の一覧を作る。
BOARD = bpy.data.scenes.new("00_ModularSet01_ContactSheet")
bpy.context.window.scene = BOARD
paper = emission_material("Preview_Paper", (*linear_hex("ECEDE7"), 1))
ink = emission_material("Preview_Ink", (*linear_hex("253D34"), 1))
muted = emission_material("Preview_Muted", (*linear_hex("737C72"), 1))
board_rect("Paper", 0, 0, 24.8, 24.0, paper, -0.01)
board_text("TD2 / MODULAR SET 01", 0.75, 22.82, 0.57, ink)
board_text("STAGE ONE   /   NINE INDEPENDENT PROPS   /   METRIC", 0.78, 22.24, 0.20, muted)
board_rect("Header rule", 0.78, 21.87, 23.24, 0.018, muted)
for index, entry in enumerate(ASSETS):
    col, row = index % 3, index // 3
    x, top = 0.7 + col * 8.05, 21.60 - row * 6.85
    image = bpy.data.images.load(entry["preview"], check_existing=True)
    image.pack()
    mat = emission_material("Preview_Tile_" + entry["id"], (1, 1, 1, 1), image)
    board_rect(entry["id"] + " thumbnail", x, top - 5.88, 7.644, 5.88, mat)
    board_text(entry["title"], x + 0.12, top - 6.25, 0.235, ink)
    width, height, depth = entry["dimensions_m"]
    board_text("%.3f W / %.3f H / %.3f D m" % (width, height, depth), x + 0.12, top - 6.58, 0.19, muted)
board_rect("Footer rule", 0.78, 0.91, 23.24, 0.016, muted)
board_text("EXPORT +Z FRONT / +Y UP / BOTTOM-CENTER PIVOTS", 0.80, 0.47, 0.19, muted)
board_text("FLAT PBR / NO TEXTURES / STUDIO LIGHTING FOR REVIEW", 12.35, 0.47, 0.19, muted)
camera_data = bpy.data.cameras.new("Contact sheet camera")
camera = bpy.data.objects.new("Contact sheet camera", camera_data)
BOARD.collection.objects.link(camera)
camera.location = (12.4, 12.0, 10)
camera_data.type = "ORTHO"
camera_data.ortho_scale = 24.8
BOARD.camera = camera
BOARD.render.engine = "CYCLES"
BOARD.cycles.samples = 1
BOARD.cycles.use_denoising = False
BOARD.render.resolution_x = 2480
BOARD.render.resolution_y = 2400
BOARD.render.resolution_percentage = 100
BOARD.view_settings.view_transform = "Standard"
BOARD.render.image_settings.file_format = "PNG"
BOARD.render.filepath = str(PREVIEWS / "ModularSet01_ContactSheet.png")
bpy.ops.render.render(write_still=True)

manifest = {
    "schema_version": 1,
    "set": "ModularSet01",
    "blender_version": bpy.app.version_string,
    "source": str(ROOT / "ModularSet01.blend"),
    "preview": str(PREVIEWS / "ModularSet01_ContactSheet.png"),
    "conventions": {"units": "meters", "gltf_up": "+Y", "gltf_front": "+Z",
                    "pivot": "bottom_center", "dimension_order": "width_height_depth",
                    "node_transforms": "identity", "screen_material": "TD2_Screen",
                    "screen_texture_uv": "glTF top-left (0,0); Blender bottom-left (0,0)",
                    "default_emission_strength": 0,
                    "textures": False, "camera_light_export": False},
    "engine_import_verified": False,
    "notes": ["Floor origin is underside; walking surface is +0.08 m.",
              "Lamp is geometry only. Add the actual light through HIKARI.",
              "TV render texture binding, facing and color require engine verification.",
              "Preview cameras, lights and ground are not included in GLB exports."],
    "assets": [],
}
for entry in ASSETS:
    triangles = 0
    for obj in entry["meshes"]:
        obj.data.calc_loop_triangles()
        triangles += len(obj.data.loop_triangles)
    manifest["assets"].append({"id": entry["id"], "purpose": entry["purpose"],
                               "file": str(entry["export"]), "preview": entry["preview"],
                               "dimensions_m": entry["dimensions_m"],
                               "source_triangle_count": triangles,
                               "mesh_names": [obj.name for obj in entry["meshes"]],
                               "materials": sorted({mat.name for obj in entry["meshes"] for mat in obj.data.materials}),
                               "screen": entry["screen"]})
(ROOT / "asset_manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / "ModularSet01.blend"))
print("TD2_MODEL_SET_COMPLETE", json.dumps({"count": len(ASSETS), "source": manifest["source"]}))
