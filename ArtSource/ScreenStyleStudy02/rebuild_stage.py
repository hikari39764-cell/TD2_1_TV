"""テレビ内の試作を、部品の陳列ではなく一つの保守区画として組み直す。"""

import json
import math
from pathlib import Path

import bpy
from mathutils import Vector


ROOT = Path('D:/PJ/TD2_TV')
OUTPUT = ROOT / 'ArtSource/ScreenStyleStudy02/ScreenStyleStudy02_Maintenance.blend'
PREVIEW = ROOT / 'Library/ScreenStyleStudy02'
scene = bpy.context.scene
scene.name = 'Screen Interior | Maintenance passage'

# 別プロセスで旧サンプルを開く。人物・配色は引き継ぎ、旧ファイルは保存し直さない。
kept = {'03 Characters | prototype rig', '05 Light and camera'}
for coll in list(bpy.data.collections):
    if coll.name.startswith(('01 ', '02 ', '04 ', '06 ')):
        for obj in list(coll.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        bpy.data.collections.remove(coll)


def collection(name):
    coll = bpy.data.collections.new(name)
    scene.collection.children.link(coll)
    return coll


terrain = collection('01 Concrete | integer seams')
mechanisms = collection('02 Machinery | installed assemblies')
backdrop = collection('04 Architecture | quiet background')
signals = collection('06 Signals | ivory states, red danger')
active = terrain
mats = {role: bpy.data.materials['Screen / ' + role] for role in (
    'tile', 'frame', 'edge', 'steel', 'black', 'wall', 'wall_alt', 'brass',
    'ceramic', 'body', 'band', 'echo', 'amber', 'red', 'mint', 'lamp', 'damage surface')}


def variation(role, source, strength):
    mat = mats[source].copy()
    mat.name = 'Screen / ' + role
    for node in mat.node_tree.nodes:
        if node.bl_idname == 'ShaderNodeEmission':
            node.inputs['Strength'].default_value = strength
    mats[role] = mat
    return mat


variation('concrete', 'tile', .70)
variation('concrete joint', 'tile', .42)
variation('contact edge', 'tile', .99)
variation('background concrete', 'wall', .51)
variation('background ribs', 'wall', .68)
variation('wall joint', 'wall', .26)
variation('machine enamel', 'tile', .61)
variation('dark iron', 'steel', .38)
variation('signal ivory', 'mint', .90)


def place(obj, name, material):
    obj.name = name
    for owner in list(obj.users_collection):
        owner.objects.unlink(obj)
    active.objects.link(obj)
    if material:
        obj.data.materials.append(mats[material])
    return obj


def box(name, center, size, material, bevel=0):
    bpy.ops.mesh.primitive_cube_add(size=1, location=center)
    obj = place(bpy.context.object, name, material)
    obj.dimensions = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bevel:
        mod = obj.modifiers.new('Manufactured edge', 'BEVEL')
        mod.width = min(bevel, min(size) * .24)
        mod.segments = 1
    return obj


def cylinder(name, center, radius, depth, material, axis='Y', vertices=24):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=depth, location=center)
    obj = place(bpy.context.object, name, material)
    if axis == 'Y':
        obj.rotation_euler.x = math.pi / 2
    elif axis == 'X':
        obj.rotation_euler.y = math.pi / 2
    return obj


def rod(name, a, b, radius, material):
    a, b = Vector(a), Vector(b)
    obj = cylinder(name, (a+b)/2, radius, (b-a).length, material, 'Z', 12)
    obj.rotation_euler = (b-a).to_track_quat('Z', 'Y').to_euler()
    return obj


def prism(name, outline, front, back, material):
    # 正面輪郭を押し出す。奥行きの装飾で占有セルを変えない。
    count = len(outline)
    vertices = [(x, y, z) for y in (front, back) for x, z in outline]
    faces = [tuple(reversed(range(count))), tuple(range(count, count*2))]
    faces.extend((i, (i+1) % count, (i+1) % count+count, i+count) for i in range(count))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    active.objects.link(obj)
    mesh.materials.append(mats[material])
    return obj


def tube_path(name, points, radius, material):
    # 配管は必要な設備間だけを結ぶ。背景全面を配線模様で埋めない。
    curve = bpy.data.curves.new(name, 'CURVE')
    curve.dimensions = '3D'
    curve.bevel_depth = radius
    curve.resolution_u = 2
    curve.bevel_resolution = 1
    spline = curve.splines.new('POLY')
    spline.points.add(len(points)-1)
    for point, position in zip(spline.points, points):
        point.co = (*position, 1)
    obj = bpy.data.objects.new(name, curve)
    active.objects.link(obj)
    curve.materials.append(mats[material])
    return obj


def concrete_run(name, start, end, bottom, top, material='concrete', face_y=-.36):
    obj = box(name, ((start+end)/2, .03, (bottom+top)/2),
              (end-start, .78, top-bottom), material, .005)
    obj['grid_bounds'] = [start, end, bottom, top]
    obj['role'] = 'Solid terrain, same front silhouette as integer collision cells'
    # 連続した面の浅い目地だけで一格を読む。全セルを額縁で囲まない。
    for x in range(math.ceil(start)+1, math.ceil(end)):
        box(name+f' / joint {x}', (x, face_y-.003, (bottom+top)/2),
            (.012, .012, top-bottom-.07), 'concrete joint')
    box(name+' / upper arris', ((start+end)/2, face_y-.006, top-.025),
        (end-start, .026, .05), 'contact edge')
    return obj


# 建築は長い塊で構成する。床や梁は再配置しやすい数セル単位の部材を保つ。
for start, end in ((0, 4), (4, 7), (7, 10), (13, 16)):
    concrete_run(f'Floor / {start}-{end}', start, end, 0, 1)
for start, end in ((8, 11), (11, 15)):
    concrete_run(f'Upper gallery / {start}-{end}', start, end, 4, 5)

for x in (.5, 15.5):
    box(f'Perimeter / pier {x}', (x, .15, 4.5), (1, 1.02, 7), 'frame', .005)
    for z in range(2, 8):
        box(f'Perimeter / lift joint {x}-{z}', (x, -.367, z), (1, .009, .010), 'dark iron')
box('Perimeter / concrete soffit', (8, .15, 8.5), (16, 1.02, 1), 'frame', .004)
for x in range(1, 16):
    box(f'Perimeter / soffit joint {x}', (x, -.368, 8.5), (.010, .008, .95), 'dark iron')
box('Gallery / underside steel bearer', (11.5, .07, 4.07), (7, .83, .14), 'dark iron')

active = backdrop
# 右側は高いサービスシャフト、左側は低い保守通路。左右対称の壁紙構成を避ける。
box('Architecture / back concrete', (8, 2.16, 4.5), (16, .24, 9), 'background concrete')
box('Architecture / deep service shaft', (12.05, 1.92, 6.4), (5.9, .18, 3.2), 'wall')
box('Architecture / lower shadow recess', (11.55, 1.90, 2.48), (6.75, .22, 2.94), 'black')
box('Architecture / shaft jamb', (9.12, 1.40, 6.42), (.27, .90, 3.16), 'background ribs')
box('Architecture / high lintel', (11.98, 1.52, 7.73), (5.72, .60, .42), 'background ribs')
for x in (3.1, 6.2):
    box(f'Architecture / casting joint {x}', (x, 2.025, 4.5), (.018, .014, 7), 'wall joint')
box('Architecture / casting lift', (4.55, 2.025, 5.45), (7.1, .014, .018), 'wall joint')

# 大きな排気ダクトを一箇所だけ配置し、丸と直線の対比を建物の用途から作る。
cylinder('Ventilation / throat shadow', (3.25, 1.92, 6.25), 1.03, .22, 'black', vertices=64)
bpy.ops.mesh.primitive_torus_add(major_segments=64, minor_segments=6, location=(3.25,1.73,6.25),
                               rotation=(math.pi/2, 0, 0), major_radius=1.035, minor_radius=.065)
place(bpy.context.object, 'Ventilation / rolled rim', 'background ribs')
for offset in (-.65, -.32, 0, .32, .65):
    half = math.sqrt(.97**2-offset**2)
    box(f'Ventilation / internal louver {offset}', (3.25,1.73,6.25+offset),
        (half*2,.17,.07), 'dark iron')
box('Ventilation / lower sill', (3.25,1.76,5.17), (2.26,.33,.10), 'background ribs')

# 下層は壁に沿う一本の太い管だけとし、動線の背後に余白を残す。
tube_path('Services / return pipe', [(1.03,1.55,2.30),(1.6,1.55,2.30),(1.8,1.55,2.12),
    (1.8,1.55,1.92),(5.8,1.55,1.92),(6.05,1.55,2.17),(6.05,1.55,7.98)], .075, 'dark iron')
for x in (2.5, 5.2):
    box(f'Services / return pipe clip {x}', (x,1.52,1.92), (.06,.23,.24), 'steel')
box('Services / cable tray below upper deck', (11.5,1.05,3.60), (6.8,.38,.12), 'dark iron')
for x in (9.2, 12.8, 14.4):
    box(f'Services / hanger {x}', (x,1.06,3.82), (.035,.07,.45), 'dark iron')

active = mechanisms
# 一方向床は壁へ片持ちで固定した鋼製歩廊。厚いコンクリートとは輪郭を分ける。
box('Catwalk / channel web', (3.8,-.035,3.065), (3.6,.62,.16), 'steel', .003)
box('Catwalk / upper flange', (3.8,-.09,3.14), (3.6,.72,.045), 'contact edge')
box('Catwalk / lower flange', (3.8,-.06,2.98), (3.6,.68,.035), 'dark iron')
for x in (2.3, 5.15):
    box(f'Catwalk / wall plate {x}', (x,1.72,2.91), (.20,.18,.68), 'steel')
    rod(f'Catwalk / cantilever {x}', (x,1.68,2.61), (x,-.03,3.02), .045, 'steel')
for x in (3,4,5):
    box(f'Catwalk / metre joint {x}', (x,-.458,3.14), (.018,.015,.045), 'concrete joint')

# 圧板は床埋込の秤。二つの端部受けと沈む板でストロークを読む。
box('Scale / recessed pocket', (4.5,-.015,1.024), (1,.78,.05), 'black')
box('Scale / moving load plate', (4.5,-.03,1.065), (.84,.70,.09), 'machine enamel', .003)
for x in (4.04,4.96):
    box(f'Scale / fixed bearing {x}', (x,-.015,1.07), (.08,.78,.14), 'steel', .002)
box('Scale / state slit', (4.5,-.427,1.012), (.24,.02,.028), 'signal ivory')

# 閉鎖機は厚い一枚の鋼板を二本のレールで上下させる。引き出し状の反復を使わない。
for x in (7.025,7.975):
    box(f'Gate / guide channel {x}', (x,.02,2.5), (.13,.64,3), 'dark iron')
    box(f'Gate / guide wearing edge {x}', (x,-.315,2.5), (.035,.035,2.99), 'edge')
prism('Gate / steel closure leaf', [(7.11,1.01),(7.89,1.01),(7.89,3.91),(7.11,3.91)],
      -.20,.12,'machine enamel')
box('Gate / folded centre rib', (7.5,-.237,2.44), (.055,.075,2.66), 'concrete')
for z in (1.10,3.76):
    box(f'Gate / transverse stiffener {z}', (7.5,-.28,z), (.78,.12,.075), 'steel')
box('Gate / drive lintel', (7.47,.04,4.13), (1.40,.74,.36), 'steel', .025)
cylinder('Gate / motor body', (6.92,-.02,4.13), .23,.42,'dark iron',axis='X')
cylinder('Gate / chain pulley', (7.43,-.35,4.13), .16,.08,'machine enamel')
box('Gate / shaft coupling', (7.43,-.405,4.13), (.045,.04,.22), 'dark iron')
box('Gate / travel switch', (7.84,-.33,3.58), (.06,.08,.18), 'signal ivory')

# 機能のある配線だけ見せる。明るい配管で床全体を囲む表現にはしない。
tube_path('Scale to gate / conduit', [(4.5,1.25,1.0),(4.5,1.25,1.38),(6.66,1.25,1.38),
    (6.76,1.25,1.48),(6.76,1.25,4.16),(6.95,1.25,4.35),(7.45,1.25,4.35)], .025,'edge')
box('Scale to gate / junction', (6.75,1.23,2.38), (.18,.14,.26), 'steel', .01)

# 壁付け操作器は古い機械式切替器の構造に絞り、発光ボタンの集合を避ける。
box('Switch / mounting plate', (5.02,1.79,3.86), (.66,.12,.96), 'dark iron')
box('Switch / enamel enclosure', (5.02,1.52,3.83), (.52,.43,.77), 'machine enamel', .025)
cylinder('Switch / rotary socket', (5.02,1.28,3.80), .155,.08,'dark iron')
handle = box('Switch / rotary grip', (5.02,1.20,3.80), (.085,.10,.26), 'ceramic', .015)
handle.rotation_euler.y = -.45
box('Switch / ivory status window', (5.02,1.295,4.08), (.21,.025,.048), 'signal ivory')
tube_path('Switch / supply conduit', [(5.02,1.79,4.35),(5.02,1.79,4.66),(5.2,1.79,4.84),
    (6.04,1.79,4.84)], .027,'dark iron')

# 危険床は溝の中の鋼製ラム。三角アイコンではなく厚みのある切削ヘッドを作る。
box('Hazard trench / bed', (11.5,.03,.14), (3,.84,.28), 'dark iron')
box('Hazard trench / front retaining beam', (11.5,-.415,.25), (3,.08,.24), 'steel')
for index in range(6):
    x = 10.25 + .5*index
    box(f'Punch / piston {index}', (x,.0,.41), (.11,.44,.40), 'steel')
    prism(f'Punch / cutting head {index}', [(x-.19,.51),(x+.19,.51),(x+.045,.96),(x-.015,.96)],
          -.28,.32,'damage surface')
    box(f'Punch / guide shoe {index}', (x,-.02,.36), (.33,.67,.13), 'dark iron', .008)

# レーザーは右壁の据付装置。明るい赤はビームと開口だけに限定する。
box('Optical head / wall backplate', (14.93,.05,5.65), (.14,.78,.98), 'dark iron')
box('Optical head / horizontal cradle', (14.58,.06,5.39), (.73,.63,.10), 'steel')
cylinder('Optical head / barrel', (14.57,.03,5.68), .21,.61,'machine enamel',axis='X')
cylinder('Optical head / dark collar', (14.28,.03,5.68), .224,.12,'dark iron',axis='X')
cylinder('Optical head / red aperture', (14.21,.03,5.68), .076,.035,'red',axis='X')
tube_path('Optical head / supply', [(14.82,.18,5.65),(14.73,.4,5.3),(14.8,.55,5.14),
    (14.98,.55,5.14)], .034,'dark iron')
active = signals
rod('Damage / laser beam', (11.03,-.10,5.68), (14.20,-.10,5.68), .015,'red')
for z in (5.02,5.99):
    box(f'Echo / support boundary {z}', (10.7,-.11,z), (.625,.48,.025),'mint')

active = backdrop
# 照明器具も壁と一体の少数箇所に限定し、均一な展示照明から離れる。
box('Task light / mounting arm', (5.13,1.72,5.25), (.06,.40,.55), 'dark iron')
prism('Task light / bent sheet hood', [(4.52,4.95),(5.61,4.95),(5.54,5.10),(4.59,5.10)],
      .80,1.62,'dark iron')
box('Task light / opal strip', (5.065,.82,4.962), (.85,.06,.025),'lamp')
box('Shaft light / recess', (13.84,1.25,7.26), (1.19,.40,.12),'dark iron')
box('Shaft light / narrow strip', (13.84,1.04,7.235), (.94,.025,.032),'lamp')

# 主役の接地面と機械の輪郭を明確にし、背景は一段落とす。照明色は中立色。
for obj in list(scene.objects):
    if obj.type == 'LIGHT':
        bpy.data.objects.remove(obj, do_unlink=True)
active = bpy.data.collections['05 Light and camera']


def area(name, location, target, energy, size):
    data = bpy.data.lights.new(name,'AREA')
    data.energy, data.shape, data.size = energy,'DISK',size
    data.color = (1,1,1)
    obj = bpy.data.objects.new(name,data)
    active.objects.link(obj)
    obj.location = location
    obj.rotation_euler = (Vector(target)-obj.location).to_track_quat('-Z','Y').to_euler()


area('Light / maintenance pool', (4.1,-3.2,6.8), (4,.2,1.5), 900,4)
area('Light / high shaft', (13,-2,8.1), (11.4,.2,5.3), 650,3)
area('Light / quiet frontal fill', (8,-8,4), (8,0,4), 280,9)
scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = .20
scene.camera.location = (8,-24,4.5)
scene.camera.data.ortho_scale = 16
scene.view_settings.exposure = .18
scene['study_scope'] = 'Independent 3D visual study. Gameplay and user factory unchanged.'
scene['design_intent'] = 'Continuous cast concrete, cantilever walkway, installed industrial machinery. Grid seams remain integer.'
scene['grid'] = '16 x 9; one unit per cell; orthographic X-Z view; Y is visual depth only.'
scene.render.resolution_x,scene.render.resolution_y = 1920,1080
scene.render.resolution_percentage = 100
scene.render.filepath = str(PREVIEW / 'maintenance_side_view.png')
scene['preview_path'] = scene.render.filepath
for screen in bpy.data.screens:
    for editor in screen.areas:
        if editor.type == 'VIEW_3D':
            space = editor.spaces.active
            space.camera = scene.camera
            space.use_local_camera = False
            space.region_3d.view_perspective = 'CAMERA'
            space.shading.type = 'RENDERED'
            space.shading.use_compositor = 'CAMERA'
            space.overlay.show_overlays = False
bpy.ops.wm.save_as_mainfile(filepath=str(OUTPUT))
bpy.ops.render.render(write_still=True)

# テレビ表示相当の縮小画像を同じ場面から直接描画して視認性を確認する。
scene.render.resolution_x,scene.render.resolution_y = 384,216
scene.render.filepath = str(PREVIEW / 'maintenance_small.png')
bpy.ops.render.render(write_still=True)
print(json.dumps({'blend':str(OUTPUT),'objects':len(scene.objects),'camera':scene.camera.data.type,
    'large':str(PREVIEW/'maintenance_side_view.png'),'small':scene.render.filepath}))
