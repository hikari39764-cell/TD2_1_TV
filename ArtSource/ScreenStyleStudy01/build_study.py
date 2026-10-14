"""テレビ内の横視点を検討する独立した Blender 美術サンプル。"""

import json
import math
from pathlib import Path

import bpy
from mathutils import Vector


ROOT = Path('D:/PJ/TD2_TV')
OUTPUT = ROOT / 'ArtSource/ScreenStyleStudy01'
PREVIEW = ROOT / 'Library/ScreenStyleStudy01'
OUTPUT.mkdir(parents=True, exist_ok=True)
PREVIEW.mkdir(parents=True, exist_ok=True)

# バックグラウンド専用プロセスで実行し、利用者の既存ファイルを変更しない。
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.name = 'Screen Interior | Side View Study'
scene.render.engine = 'BLENDER_EEVEE'
scene.render.resolution_x = 1920
scene.render.resolution_y = 1080
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'PNG'
scene.render.film_transparent = False
scene.view_settings.view_transform = 'AgX'
scene.view_settings.look = 'AgX - Medium High Contrast'
scene.view_settings.exposure = 0.25
scene.render.fps = 30
scene.world = bpy.data.worlds.new('Screen ambient')
scene.world.use_nodes = True
scene.world.node_tree.nodes['Background'].inputs['Color'].default_value = (0.14, 0.19, 0.19, 1)
scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.3


def collection(name):
    result = bpy.data.collections.new(name)
    scene.collection.children.link(result)
    return result


collections = {name: collection(name) for name in (
    '01 Terrain | integer cells', '02 Mechanisms | editable parts',
    '03 Characters | prototype rig', '04 Background | shallow depth',
    '05 Light and camera', '06 Signal details')}
active_collection = collections['01 Terrain | integer cells']


def move_to_collection(obj, target=None):
    target = target or active_collection
    for owner in list(obj.users_collection):
        owner.objects.unlink(obj)
    target.objects.link(obj)
    return obj


def rgb(hex_color):
    values = [int(hex_color[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    return tuple(v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in values)


def material(name, color, emission=0.0, stylized=True):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.diffuse_color = (*rgb(color), 1)
    nodes = mat.node_tree.nodes
    nodes.clear()
    links = mat.node_tree.links
    out = nodes.new('ShaderNodeOutputMaterial')
    out.location = (640, 0)
    if emission:
        surface = nodes.new('ShaderNodeEmission')
        surface.inputs['Color'].default_value = (*rgb(color), 1)
        surface.inputs['Strength'].default_value = emission
        links.new(surface.outputs[0], out.inputs['Surface'])
        return mat
    if not stylized:
        surface = nodes.new('ShaderNodeBsdfPrincipled')
        surface.inputs['Base Color'].default_value = (*rgb(color), 1)
        surface.inputs['Roughness'].default_value = 0.8
        links.new(surface.outputs[0], out.inputs['Surface'])
        return mat
    # 物理光を大きな三段へ整理する。形状の陰影は保ち、細かな反射は作らない。
    diffuse = nodes.new('ShaderNodeBsdfDiffuse')
    diffuse.location = (-620, 0)
    diffuse.inputs['Color'].default_value = (0.8, 0.8, 0.8, 1)
    diffuse.inputs['Roughness'].default_value = 0.8
    shading = nodes.new('ShaderNodeShaderToRGB')
    shading.location = (-410, 0)
    links.new(diffuse.outputs[0], shading.inputs[0])
    ramp = nodes.new('ShaderNodeValToRGB')
    ramp.name = 'Soft three tone | shadow-mid-light'
    ramp.label = 'Soft three tone'
    ramp.location = (-190, 0)
    ramp.color_ramp.interpolation = 'EASE'
    points = [(0.08, 0.21), (0.28, 0.24), (0.36, 0.55), (0.57, 0.59), (0.68, 1.0)]
    for e in list(ramp.color_ramp.elements)[1:]:
        ramp.color_ramp.elements.remove(e)
    for i, (pos, value) in enumerate(points):
        e = ramp.color_ramp.elements[0] if i == 0 else ramp.color_ramp.elements.new(pos)
        e.position = pos
        e.color = (value * 0.91, value, value * 0.98, 1)
    links.new(shading.outputs['Color'], ramp.inputs['Fac'])
    multiply = nodes.new('ShaderNodeMixRGB')
    multiply.blend_type = 'MULTIPLY'
    multiply.inputs[0].default_value = 1.0
    multiply.inputs[2].default_value = (*rgb(color), 1)
    multiply.location = (110, 0)
    links.new(ramp.outputs[0], multiply.inputs[1])
    surface = nodes.new('ShaderNodeEmission')
    surface.location = (420, 0)
    surface.inputs['Strength'].default_value = 1.0
    links.new(multiply.outputs[0], surface.inputs['Color'])
    links.new(surface.outputs[0], out.inputs['Surface'])
    return mat


PALETTE_A = {
    'tile': 'C4BDAA', 'frame': '647078', 'edge': '899A9D', 'steel': '465868',
    'black': '1A2531', 'wall': '253240', 'wall_alt': '2B3948',
    'brass': 'B39B77', 'ceramic': 'DDD5BE', 'body': 'F3EADB', 'band': 'A65343',
    'echo': 'BAD1CD', 'amber': 'E7B56C', 'red': 'C85945', 'mint': 'C6E1D5', 'lamp': 'E7E1CA',
}
PALETTE_B = {
    'tile': 'B1C4C2', 'frame': '526D6F', 'edge': '769995', 'steel': '3F5959',
    'black': '152E30', 'wall': '203C40', 'wall_alt': '26464A',
    'brass': 'A39C83', 'ceramic': 'D1DCD0', 'body': 'EDF0DF', 'band': '936251',
    'echo': 'B2D7C8', 'amber': 'E6BA70', 'red': 'CF7960', 'mint': 'CADDC8', 'lamp': 'D7E5D6',
}
mats = {}
for role, color in PALETTE_A.items():
    strength = {'amber': 2.1, 'red': 1.4, 'mint': 1.5, 'lamp': 2.2}.get(role, 0)
    mats[role] = material('Screen / ' + role, color, emission=strength)


def box(name, location, size, mat, bevel=0.018):
    bpy.ops.mesh.primitive_cube_add(size=1, location=location)
    obj = move_to_collection(bpy.context.object)
    obj.name = name
    obj.dimensions = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    obj.data.materials.append(mat)
    if bevel:
        modifier = obj.modifiers.new('Small readable bevel', 'BEVEL')
        modifier.width = min(bevel, min(size) * 0.2)
        modifier.segments = 2
    return obj


def cylinder(name, location, radius, depth, mat, axis='Y', vertices=16):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=depth, location=location)
    obj = move_to_collection(bpy.context.object)
    obj.name = name
    if axis == 'Y':
        obj.rotation_euler.x = math.pi / 2
    elif axis == 'X':
        obj.rotation_euler.y = math.pi / 2
    obj.data.materials.append(mat)
    bevel = obj.modifiers.new('Edge radius', 'BEVEL')
    bevel.width = min(0.015, depth * 0.15)
    bevel.segments = 2
    return obj


def rod(name, a, b, radius, mat):
    a, b = Vector(a), Vector(b)
    obj = cylinder(name, (a + b) / 2, radius, (b - a).length, mat, axis='Z', vertices=10)
    obj.rotation_euler = (b - a).to_track_quat('Z', 'Y').to_euler()
    return obj


def tile(x, z):
    # 前面の占有範囲は一格を維持する。溝は表層だけに作り、隣接セルを離さない。
    face = mats['frame'] if z == 8 or x in (0, 15) else mats['tile']
    obj = box(f'Tile {x:02d}-{z:02d}', (x + .5, .03, z + .5), (1, .8, 1), face, .017)
    obj['cell_x'], obj['cell_z'] = x, z
    box(f'Tile seam {x}-{z}', (x + .025, -.381, z + .5), (.014, .014, .90), mats['steel'], 0)
    box(f'Tile face cap {x}-{z}', (x + .5, -.385, z + .93), (.90, .022, .045), mats['edge'], .006)


# 正面は X-Z 平面、一格は 1 単位。奥行き Y は表示だけに使う。
for x in range(16):
    if x not in (10, 11, 12):
        tile(x, 0)
for z in range(1, 9):
    tile(0, z)
    tile(15, z)
for x in range(1, 15):
    tile(x, 8)
for x in range(8, 15):
    tile(x, 4)

active_collection = collections['04 Background | shallow depth']
box('Continuous back wall', (8, 1.62, 4.5), (16, .25, 9), mats['wall'], .01)
for x in range(1, 15, 2):
    for z in (1, 4.5):
        box(f'Wall panel {x}-{z}', (x + 1, 1.42, z + 1.6), (1.95, .08, 3.15), mats['wall_alt'], .008)
for x in (1.2, 5.8, 9.6, 14.6):
    box(f'Background column {x}', (x, 1.05, 4.5), (.15, .35, 7), mats['steel'])
box('Upper industrial lintel', (8, .98, 7.45), (13.6, .25, .24), mats['steel'])
for x in (2.0, 11.5):
    box(f'Vent recess {x}', (x, 1.20, 6.2), (1.15, .18, .85), mats['black'])
    for k in range(5):
        box(f'Vent louver {x}-{k}', (x, 1.04, 5.94 + .12 * k), (1.0, .13, .055), mats['steel'], .008)
for x in (3.6, 12.8):
    box(f'Lamp hood {x}', (x, .9, 7.35), (2.1, .4, .20), mats['black'])
    box(f'Lamp tube {x}', (x, .66, 7.29), (1.7, .05, .07), mats['lamp'], .012)

active_collection = collections['02 Mechanisms | editable parts']
# 一方向床は厚い壁と区別し、足が触れる上端を明瞭にする。
box('One-way / front beam', (3.8, -.04, 3.05), (3.6, .6, .18), mats['edge'])
box('One-way / top lip', (3.8, -.36, 3.14), (3.6, .06, .045), mats['ceramic'], .006)
for x in (2.25, 3.25, 4.25, 5.25):
    rod(f'One-way / bracket {x}', (x, .35, 3.0), (x, 1.2, 2.6), .045, mats['steel'])
    box(f'One-way / deck joint {x}', (x, -.38, 3.04), (.025, .025, .13), mats['black'], 0)

# 圧板の上下差と信号は同時に読める。表示サンプルなので判定や配線コードを持たない。
box('Pressure plate / fixed frame', (4.5, -.10, 1.07), (1, .72, .14), mats['steel'])
box('Pressure plate / moving platen', (4.5, -.16, 1.16), (.78, .59, .11), mats['brass'])
box('Pressure plate / indicator', (4.5, -.48, 1.06), (.30, .035, .047), mats['amber'], .005)
for x in (4.07, 4.93):
    box(f'Pressure plate / travel stop {x}', (x, -.16, 1.17), (.08, .62, .11), mats['edge'])

for x in (7.02, 7.98):
    box(f'Gate / rail {x}', (x, .03, 2.48), (.12, .56, 2.96), mats['steel'])
box('Gate / drive enclosure', (7.5, .0, 4.01), (1.18, .64, .32), mats['edge'])
for k in range(6):
    z = 1.24 + k * .455
    box(f'Gate / shutter panel {k}', (7.5, -.12, z), (.82, .22, .439), mats['brass'], .018)
    box(f'Gate / panel recess {k}', (7.5, -.248, z), (.58, .014, .025), mats['steel'], .001)
cylinder('Gate / power indicator', (7.5, -.346, 4.03), .07, .03, mats['amber'])

# 危険面の三角形は正面から見ても十分な幅を持つ。
box('Spikes / machinery bed', (11.5, .03, .2), (3, .78, .4), mats['steel'])
for index in range(9):
    x = 10.18 + index * .33
    verts = [(x-.12,-.30,.4),(x+.12,-.30,.4),(x,-.30,.95),
             (x-.12,.28,.4),(x+.12,.28,.4),(x,.28,.95)]
    mesh = bpy.data.meshes.new(f'Pressed tooth {index}')
    mesh.from_pydata(verts, [], [(0,1,2),(5,4,3),(0,3,4,1),(1,4,5,2),(2,5,3,0)])
    mesh.materials.append(mats['ceramic'])
    obj = bpy.data.objects.new(f'Spikes / tooth {index}', mesh)
    active_collection.objects.link(obj)
for x in (10.1, 12.9):
    box(f'Spikes / warning {x}', (x, -.39, .24), (.09, .025, .12), mats['red'], .005)

box('Checkpoint / back box', (5.05, .82, 3.78), (.57, .30, .95), mats['steel'])
box('Checkpoint / ceramic face', (5.05, .635, 3.8), (.45, .10, .78), mats['ceramic'])
cylinder('Checkpoint / indicator housing', (5.05, .535, 3.96), .145, .08, mats['black'])
cylinder('Checkpoint / opal indicator', (5.05, .484, 3.96), .112, .035, mats['mint'])
box('Checkpoint / toggle slot', (5.05, .53, 3.55), (.16, .08, .18), mats['black'])
rod('Checkpoint / toggle', (5.05,.46,3.57), (5.05,.32,3.64), .029, mats['brass'])

box('Laser / emitter case', (14.64, .01, 5.65), (.50, .65, .70), mats['steel'])
box('Laser / ivory clamp', (14.39, -.03, 5.65), (.16, .57, .49), mats['ceramic'])
cylinder('Laser / aperture', (14.285, -.08, 5.65), .11, .08, mats['black'], axis='X')
cylinder('Laser / signal lens', (14.232, -.08, 5.65), .064, .035, mats['red'], axis='X')
rod('Laser / readable beam core', (11.03,-.10,5.65), (14.21,-.10,5.65), .018, mats['red'])

# 瞬移を遮る壁は普通のタイルと別の閉じた構造を持つ。
box('Lock wall / framed panel', (1.5, .13, 5.45), (.9, .65, 1.90), mats['steel'])
for x in (1.2, 1.5, 1.8):
    box(f'Lock wall / shielding rib {x}', (x, -.24, 5.45), (.085, .16, 1.72), mats['brass'])
for z in (4.58, 6.32):
    box(f'Lock wall / closed perimeter {z}', (1.5, -.26, z), (.83, .18, .08), mats['brass'])

active_collection = collections['06 Signal details']
# 配線は背景の少数経路へまとめ、遊べる足場と混同させない。
for index, (a, b) in enumerate([
    ((4.5,.9,1.1),(4.5,.9,1.55)), ((4.5,.9,1.55),(6.7,.9,1.55)),
    ((6.7,.9,1.55),(6.7,.9,4.25)), ((6.7,.9,4.25),(7.5,.9,4.25)),
]):
    rod(f'Plate gate / conduit {index}', a, b, .035, mats['brass'])


def import_character(name, x, z, palette, clip):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(ROOT / 'ArtSource/Characters/PrototypeNinja/ninja-game.glb'))
    imported = list(set(bpy.data.objects) - before)
    # glTF が生成する骨の表示用メッシュはキャラクター本体へ移さない。
    helpers = {bone.custom_shape for obj in imported if obj.type == 'ARMATURE'
               for bone in obj.pose.bones if bone.custom_shape is not None}
    for helper in helpers:
        helper.hide_render = True
        helper.hide_set(True)
    objects = [obj for obj in imported if obj not in helpers]
    parent = bpy.data.objects.new(name, None)
    collections['03 Characters | prototype rig'].objects.link(parent)
    # 元の身長 1.67268m を現在の 0.875 格へ合わせる。ルート移動は使わない。
    parent.scale = (.523112,)*3
    parent.location = (x, -.07, z)
    parent.rotation_euler.z = math.pi / 2
    for obj in objects:
        move_to_collection(obj, collections['03 Characters | prototype rig'])
        obj.name = name + ' / ' + obj.name
        if obj.parent not in objects:
            obj.parent = parent
        if obj.type == 'MESH':
            for slot in obj.material_slots:
                old_name = slot.material.name if slot.material else ''
                slot.material = palette if palette else (mats['band'] if 'Headband' in old_name else mats['body'])
        if obj.type == 'ARMATURE':
            obj.animation_data_create()
            for track in list(obj.animation_data.nla_tracks):
                obj.animation_data.nla_tracks.remove(track)
            action = next((a for a in bpy.data.actions if a.name.startswith(clip)), None)
            if action:
                obj.animation_data.action = action
                if action.slots:
                    obj.animation_data.action_slot = action.slots[0]
    return parent


player = import_character('Player', 2.55, 1.0, None, 'Ninja_Jog')
echo = import_character('Echo', 10.7, 5.0, mats['echo'], 'Ninja_Idle')
echo.scale = tuple(v * (1 / .875) for v in echo.scale)
active_collection = collections['06 Signal details']
# 一格高の残像は上端と足元の細い実体枠で支えの範囲を示す。
for z in (5.02, 5.99):
    box(f'Echo / support boundary {z}', (10.7,-.11,z), (.625,.48,.035), mats['mint'], .004)
scene.frame_set(7)

active_collection = collections['05 Light and camera']


def area(name, location, target, power, size, color):
    data = bpy.data.lights.new(name, 'AREA')
    data.energy, data.shape, data.size = power, 'DISK', size
    data.color = rgb(color)
    obj = bpy.data.objects.new(name, data)
    active_collection.objects.link(obj)
    obj.location = location
    obj.rotation_euler = (Vector(target) - obj.location).to_track_quat('-Z','Y').to_euler()
    return obj


area('Key / cool broad', (3,-5,10), (7,0,3.5), 1300, 7, 'DEE8E2')
area('Fill / restrained warm', (12,-4,6), (8,0,4), 700, 6, 'E5D8B9')
area('Front / readability', (8,-8,4), (8,0,4), 500, 8, 'CDDCD8')
camera_data = bpy.data.cameras.new('Screen orthographic camera')
camera = bpy.data.objects.new('Screen orthographic camera', camera_data)
active_collection.objects.link(camera)
camera.location = (8,-24,4.5)
camera.rotation_euler = (Vector((8,0,4.5))-camera.location).to_track_quat('-Z','Y').to_euler()
camera.data.type = 'ORTHO'
camera.data.ortho_scale = 16.7
camera.data.lens = 50
scene.camera = camera

# 5.1 の合成ノードは専用グループへ保存する。外部の工場用設定を参照しない。
tree = bpy.data.node_groups.new('Screen only | grade and mild glow', 'CompositorNodeTree')
tree.interface.new_socket(name='Image', in_out='OUTPUT', socket_type='NodeSocketColor')
scene.compositing_node_group = tree
render = tree.nodes.new('CompositorNodeRLayers')
render.location = (-520,0)
grade = tree.nodes.new('CompositorNodeHueSat')
grade.name = 'Screen palette restraint'
grade.location = (-260,0)
grade.inputs['Saturation'].default_value = .82
grade.inputs['Value'].default_value = 1.0
tree.links.new(render.outputs['Image'],grade.inputs['Image'])
glow = tree.nodes.new('CompositorNodeGlare')
glow.name = 'Small signal halation'
glow.location = (0,0)
glow.inputs['Type'].default_value = 'Fog Glow'
glow.inputs['Quality'].default_value = 'High'
glow.inputs['Size'].default_value = .18
if 'Threshold' in glow.inputs:
    glow.inputs['Threshold'].default_value = 1.4
if 'Strength' in glow.inputs:
    glow.inputs['Strength'].default_value = .12
tree.links.new(grade.outputs['Image'],glow.inputs['Image'])
out = tree.nodes.new('NodeGroupOutput')
out.location = (260,0)
tree.links.new(glow.outputs['Image'],out.inputs['Image'])

# 開いた時に横視点をすぐ確認できるようカメラ表示で保存する。
for screen in bpy.data.screens:
    for editor in screen.areas:
        if editor.type == 'VIEW_3D':
            editor.spaces.active.region_3d.view_perspective = 'CAMERA'
            editor.spaces.active.shading.type = 'RENDERED'
            editor.spaces.active.overlay.show_overlays = False
scene['study_scope'] = 'Visual study only. Existing gameplay and factory scene unchanged.'
scene['grid'] = '16 x 9; 1 Blender unit per tile; gameplay plane X-Z; depth Y is visual only.'
scene['palette'] = 'A | warm mineral / ink blue / vermilion signal'
scene['preview_path'] = str(PREVIEW / 'screen_side_view_A.png')
scene.render.filepath = str(PREVIEW / 'screen_side_view_A.png')
bpy.ops.wm.save_as_mainfile(filepath=str(OUTPUT / 'ScreenStyleStudy01.blend'))
bpy.ops.render.render(write_still=True)

# 同じ構図・同じ照明で配色だけを比べる。主案のファイルは上書きしない。
for role, color in PALETTE_B.items():
    mat = mats[role]
    rgba = (*rgb(color), 1)
    mat.diffuse_color = rgba
    for node in mat.node_tree.nodes:
        if node.bl_idname == 'ShaderNodeMixRGB':
            node.inputs[2].default_value = rgba
        elif node.bl_idname == 'ShaderNodeEmission' and not node.inputs['Color'].is_linked:
            node.inputs['Color'].default_value = rgba
scene['palette'] = 'B | silver / deep grey teal / amber signal'
scene['preview_path'] = str(PREVIEW / 'screen_side_view_B.png')
scene.render.filepath = str(PREVIEW / 'screen_side_view_B.png')
bpy.ops.wm.save_as_mainfile(filepath=str(OUTPUT / 'ScreenStyleStudy01_PaletteB.blend'))
bpy.ops.render.render(write_still=True)
print(json.dumps({'blend_A': str(OUTPUT / 'ScreenStyleStudy01.blend'),
                  'blend_B': bpy.data.filepath, 'objects': len(scene.objects)},ensure_ascii=False))
