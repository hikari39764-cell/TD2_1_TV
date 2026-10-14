"""既存の横視点サンプルを二つの基調色と危険色へ整理する。"""

import json
from pathlib import Path

import bpy


ROOT = Path('D:/PJ/TD2_TV')
OUTPUT = ROOT / 'ArtSource/ScreenStyleStudy01/ScreenStyleStudy01_ThreeColor.blend'
PREVIEW = ROOT / 'Library/ScreenStyleStudy01'
scene = bpy.context.scene


def linear_color(hex_color):
    channels = [int(hex_color[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    return tuple(v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4 for v in channels)


COLORS = {
    'ink': linear_color('435969'),
    'ivory': linear_color('DDD5C1'),
    'damage': linear_color('C34737'),
}

# 明度差はシェーディングと出力強度に持たせ、役割ごとに別の色相を追加しない。
ROLES = {
    'tile': ('ivory', .83), 'frame': ('ink', .88), 'edge': ('ink', 1.22),
    'steel': ('ink', .68), 'black': ('ink', .20), 'wall': ('ink', .30),
    'wall_alt': ('ink', .36), 'brass': ('ivory', .68), 'ceramic': ('ivory', 1.0),
    'body': ('ivory', 1.20), 'band': ('ink', .40), 'echo': ('ivory', 1.0),
    'amber': ('ivory', 1.45), 'red': ('damage', 1.1),
    'mint': ('ivory', 1.3), 'lamp': ('ivory', 1.8),
}


def recolor(mat, family, strength):
    color = COLORS[family]
    mat.diffuse_color = (*color, 1)
    mat['palette_family'] = family
    mat['base_color_srgb'] = {'ink': '#435969', 'ivory': '#DDD5C1', 'damage': '#C34737'}[family]
    for node in mat.node_tree.nodes:
        if node.bl_idname == 'ShaderNodeMixRGB':
            node.inputs[2].default_value = (*color, 1)
        elif node.bl_idname == 'ShaderNodeValToRGB':
            # 旧サンプルの緑寄りの陰影を無彩色へ戻す。
            for point in node.color_ramp.elements:
                shade = point.color[1]
                point.color = (shade, shade, shade, 1)
        elif node.bl_idname == 'ShaderNodeEmission':
            node.inputs['Strength'].default_value = strength
            if not node.inputs['Color'].is_linked:
                node.inputs['Color'].default_value = (*color, 1)


for role, (family, strength) in ROLES.items():
    recolor(bpy.data.materials['Screen / ' + role], family, strength)

# 赤は実際の危険部にだけ使う。筐体や通常のスイッチは基調色のままにする。
damage_surface = bpy.data.materials['Screen / tile'].copy()
damage_surface.name = 'Screen / damage surface'
recolor(damage_surface, 'damage', .95)
red_objects = []
for obj in scene.objects:
    if obj.type != 'MESH':
        continue
    if obj.name.startswith('Spikes / tooth'):
        obj.data.materials.clear()
        obj.data.materials.append(damage_surface)
    if any(slot.material and slot.material.get('palette_family') == 'damage' for slot in obj.material_slots):
        red_objects.append(obj.name)

# 残像は不透明な実体のまま、二色の粗い断層模様で本体と区別する。
echo = bpy.data.materials['Screen / echo']
nodes, links = echo.node_tree.nodes, echo.node_tree.links
geometry = nodes.new('ShaderNodeNewGeometry')
geometry.location = (-650, -280)
separate = nodes.new('ShaderNodeSeparateXYZ')
separate.location = (-470, -280)
links.new(geometry.outputs['Position'], separate.inputs[0])
frequency = nodes.new('ShaderNodeMath')
frequency.operation = 'MULTIPLY'
frequency.inputs[1].default_value = 5.0
frequency.location = (-310, -280)
links.new(separate.outputs['Z'], frequency.inputs[0])
phase = nodes.new('ShaderNodeMath')
phase.operation = 'FRACT'
phase.location = (-140, -280)
links.new(frequency.outputs[0], phase.inputs[0])
line = nodes.new('ShaderNodeMath')
line.operation = 'LESS_THAN'
line.inputs[1].default_value = .18
line.location = (20, -280)
links.new(phase.outputs[0], line.inputs[0])
mix = nodes.new('ShaderNodeMixRGB')
mix.label = 'Solid echo / two-color strata'
mix.location = (230, -160)
mix.inputs[2].default_value = (*COLORS['ink'], 1)
surface = next(n for n in nodes if n.bl_idname == 'ShaderNodeEmission')
original = surface.inputs['Color'].links[0].from_socket
links.new(original, mix.inputs[1])
links.new(line.outputs[0], mix.inputs[0])
links.new(mix.outputs[0], surface.inputs['Color'])
surface.location = (490, 0)

# ライトは中立色にそろえる。色数の判断に別の照明色を混ぜない。
for obj in scene.objects:
    if obj.type == 'LIGHT':
        obj.data.color = (1, 1, 1)
scene.world.node_tree.nodes['Background'].inputs['Color'].default_value = (.17, .17, .17, 1)
scene.view_settings.exposure = .25
scene.compositing_node_group.nodes['Screen palette restraint'].inputs['Saturation'].default_value = 1.0
scene.compositing_node_group.nodes['Small signal halation'].inputs['Strength'].default_value = .075
scene['palette'] = 'Three color | ink blue-grey / warm ivory / damage vermilion'
scene['palette_constraint'] = 'Two base color families with tonal shading; red reserved for damaging parts.'
scene['damage_red_objects'] = json.dumps(sorted(red_objects))
scene.render.resolution_x, scene.render.resolution_y = 1920, 1080
scene.render.resolution_percentage = 100
scene.render.filepath = str(PREVIEW / 'screen_three_color.png')
scene['preview_path'] = scene.render.filepath
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            space = area.spaces.active
            space.camera = scene.camera
            space.use_local_camera = False
            space.region_3d.view_perspective = 'CAMERA'
            space.shading.type = 'RENDERED'
            space.shading.use_compositor = 'CAMERA'
            space.overlay.show_overlays = False
bpy.ops.wm.save_as_mainfile(filepath=str(OUTPUT))
bpy.ops.render.render(write_still=True)

# 小さなテレビでの視認性も同一シーンから直接レンダーする。
scene.render.resolution_x, scene.render.resolution_y = 384, 216
scene.render.filepath = str(PREVIEW / 'screen_three_color_small.png')
bpy.ops.render.render(write_still=True)
print(json.dumps({'blend': str(OUTPUT), 'red_objects': sorted(red_objects),
                  'renders': ['screen_three_color.png', 'screen_three_color_small.png']}, ensure_ascii=False))
