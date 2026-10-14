"""建築再現よりゲームの輪郭を優先する、背景なしの表示比較。"""

import json
from pathlib import Path

import bpy


ROOT = Path('D:/PJ/TD2_TV')
OUTPUT = ROOT / 'ArtSource/ScreenStyleStudy02/ScreenStyleStudy02_GameShapes.blend'
PREVIEW = ROOT / 'Library/ScreenStyleStudy02'
scene = bpy.context.scene
scene.name = 'Screen Interior | Clear game shapes'

# 壁・配管・換気口は役割の読み取りを妨げるため、この案から外す。
background = bpy.data.collections['04 Architecture | quiet background']
for obj in list(background.objects):
    bpy.data.objects.remove(obj, do_unlink=True)
bpy.data.collections.remove(background)
unneeded = ('Catwalk / wall plate', 'Catwalk / cantilever', 'Switch / mounting plate',
            'Switch / supply', 'Scale to gate /', 'Gate / motor body',
            'Gate / chain pulley', 'Gate / shaft coupling', 'Optical head / supply')
for obj in list(scene.objects):
    if obj.name.startswith(unneeded):
        bpy.data.objects.remove(obj, do_unlink=True)


def box(name, center, size, material, bevel=0):
    bpy.ops.mesh.primitive_cube_add(size=1, location=center)
    obj = bpy.context.object
    obj.name = name
    for coll in list(obj.users_collection):
        coll.objects.unlink(obj)
    bpy.data.collections['02 Machinery | installed assemblies'].objects.link(obj)
    obj.dimensions = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    obj.data.materials.append(bpy.data.materials['Screen / '+material])
    if bevel:
        mod = obj.modifiers.new('Readable edge', 'BEVEL')
        mod.width, mod.segments = bevel, 1
    return obj


# スイッチは短い台座だけで足場との関係を示す。背景の壁を必要としない。
box('Switch / short pedestal', (5.02,1.52,3.40), (.14,.23,.48), 'steel', .01)
box('Switch / foot', (5.02,1.52,3.17), (.40,.37,.055), 'steel', .006)
for name in ('Switch / short pedestal','Switch / foot','Switch / enamel enclosure',
             'Switch / rotary socket','Switch / rotary grip','Switch / ivory status window'):
    scene.objects[name].location.y -= 1.60

# 細かい設備表現を減らし、門は大きい一枚板、レーザーは短い発射口として読む。
drive = scene.objects['Gate / drive lintel']
drive.dimensions.x = 1.14
drive.location.x = 7.5
box('Gate / state window', (7.5,-.345,4.13), (.21,.026,.055), 'signal ivory')

# 背景は基調色の低い明度だけ。粒子や模様の追加は視認性を確認してから行う。
ink = bpy.data.materials['Screen / wall'].diffuse_color[:3]
world = scene.world.node_tree.nodes['Background']
world.inputs['Color'].default_value = tuple(c*.28 for c in ink)+(1,)
world.inputs['Strength'].default_value = 1

# 大きな照度の輪が画面を分断しないよう、地形の明暗変化を緩やかにする。
# 人物と残像の既存マテリアルは維持する。
for mat in bpy.data.materials:
    if not mat.name.startswith('Screen / ') or mat.name in (
        'Screen / body','Screen / band','Screen / echo'):
        continue
    if not mat.use_nodes:
        continue
    for node in mat.node_tree.nodes:
        if node.bl_idname == 'ShaderNodeValToRGB':
            ramp = node.color_ramp
            points = [(.0,.30),(.24,.42),(.48,.60),(.74,.82),(1.0,1.0)]
            for element,(position,value) in zip(ramp.elements,points):
                element.position = position
                element.color = (value,value,value,1)
            ramp.interpolation = 'LINEAR'

# 危険ヘッドの面取りで実体を読み取る。色の意味は赤＝危険のまま変えない。
for obj in scene.objects:
    if obj.name.startswith('Punch / cutting head'):
        mod = obj.modifiers.new('Cutting face bevel','BEVEL')
        mod.width,mod.segments = .025,1

scene['design_intent'] = 'Game readability first. Empty dark background; solid grid terrain, simple devices; red only for damage.'
scene['palette'] = 'Ink blue-grey / warm ivory / damage vermilion. Tonal values within each family.'
scene['study_scope'] = 'Blender visual study only; no engine, gameplay or user factory changes.'
scene.render.resolution_x,scene.render.resolution_y = 1920,1080
scene.render.filepath = str(PREVIEW / 'game_shapes.png')
scene['preview_path'] = scene.render.filepath
bpy.ops.wm.save_as_mainfile(filepath=str(OUTPUT))
bpy.ops.render.render(write_still=True)
scene.render.resolution_x,scene.render.resolution_y = 384,216
scene.render.filepath = str(PREVIEW / 'game_shapes_small.png')
bpy.ops.render.render(write_still=True)
print(json.dumps({'blend':str(OUTPUT),'objects':len(scene.objects),'camera':scene.camera.data.type,
                  'renders':['game_shapes.png','game_shapes_small.png']}))
