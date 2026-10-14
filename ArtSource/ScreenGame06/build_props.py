# -*- coding: utf-8 -*-
"""画面内ゲーム用の部品を生成する。衝突とルールはモデルから独立させる。"""
import json
import math
from pathlib import Path

import bmesh
import bpy
from mathutils import Matrix, Vector
from io_scene_fbx import parse_fbx

ROOT = Path('D:/PJ/TD2_TV')
ART = ROOT / 'ArtSource/ScreenGame06'
MODELS = ROOT / 'Assets/TD2/Screen3D/Models'
CONTRACT = MODELS / 'screen3d_contract.json'
assert bpy.app.background
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.filepaths.save_version = 0
contract = json.loads(CONTRACT.read_text(encoding='utf-8'))
entries = {e['id']: e for e in contract['models']}


def linear(value):
    values = [int(value[i:i+2], 16)/255 for i in (0, 2, 4)]
    return tuple(v/12.92 if v <= .04045 else ((v+.055)/1.055)**2.4 for v in values)


def material(name, color, glow=0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    rgba = (*linear(color), 1)
    mat.diffuse_color = rgba
    shader = mat.node_tree.nodes.get('Principled BSDF')
    shader.inputs['Base Color'].default_value = rgba
    shader.inputs['Roughness'].default_value = .86
    shader.inputs['Metallic'].default_value = 0
    shader.inputs['Specular IOR Level'].default_value = .12
    shader.inputs['Emission Color'].default_value = rgba
    shader.inputs['Emission Strength'].default_value = glow
    mat.use_backface_culling = True
    return mat


# 色相は承認済みの三色だけ。信号用も同色で、強度だけを変える。
MATS = {'ink': material('SG06_Ink', '435969'),
        'ivory': material('SG06_Ivory', 'DDD5C1'),
        'danger': material('SG06_Danger', 'C34737'),
        'signal': material('SG06_IvorySignal', 'DDD5C1', .30),
        'beam': material('SG06_DangerSignal', 'C34737', 1.0),
        'core': material('SG06_IvoryCore', 'DDD5C1', 1.8)}
BUILT, CURRENT, GROUP = [], None, []


def select(objects):
    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]


def begin(identifier):
    global CURRENT, GROUP
    entry = entries[identifier]
    scene = bpy.data.scenes.new(entry['rootName'])
    scene.unit_settings.system = 'METRIC'
    scene.unit_settings.scale_length = 1
    bpy.context.window.scene = scene
    collection = bpy.data.collections.new(entry['rootName'])
    scene.collection.children.link(collection)
    root = bpy.data.objects.new(entry['rootName'], None)
    collection.objects.link(root)
    root.empty_display_size = .10
    CURRENT = {'entry': entry, 'scene': scene, 'collection': collection, 'root': root, 'meshes': []}
    GROUP = []
    BUILT.append(CURRENT)


def register(obj, name, mat):
    for collection in list(obj.users_collection):
        collection.objects.unlink(obj)
    CURRENT['collection'].objects.link(obj)
    obj.name = CURRENT['entry']['rootName'] + '__' + name
    obj.data.materials.append(MATS[mat])
    GROUP.append(obj)
    return obj


def finish(obj, bevel=0, segments=1):
    select([obj])
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    if bevel:
        mod = obj.modifiers.new('Readable edge', 'BEVEL')
        mod.width, mod.segments = bevel, segments
        mod.harden_normals = True
        bpy.ops.object.modifier_apply(modifier=mod.name)
    for face in obj.data.polygons:
        face.use_smooth = True
    mod = obj.modifiers.new('Stable face normals', 'WEIGHTED_NORMAL')
    mod.keep_sharp = True
    bpy.ops.object.modifier_apply(modifier=mod.name)
    return obj


def box(name, size, center, mat, bevel=.008):
    bpy.ops.mesh.primitive_cube_add(size=1, location=center)
    obj = register(bpy.context.object, name, mat)
    obj.dimensions = size
    return finish(obj, min(bevel, min(size)*.23))


def mesh(name, points, faces, mat, bevel=0):
    data = bpy.data.meshes.new(name)
    data.from_pydata(points, [], faces)
    data.update()
    bm = bmesh.new()
    bm.from_mesh(data)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(data)
    bm.free()
    obj = register(bpy.data.objects.new(name, data), name, mat)
    return finish(obj, bevel)


def prism(name, outline, depth, mat, front_center=0, bevel=.008):
    # XY の遊び面は Blender XZ。奥行きは -Y 側が画面手前となる。
    n = len(outline)
    vertices = [(x, y, z) for y in (-depth/2-front_center, depth/2-front_center) for x, z in outline]
    faces = [tuple(reversed(range(n))), tuple(range(n, 2*n))]
    faces.extend((i, (i+1)%n, (i+1)%n+n, i+n) for i in range(n))
    return mesh(name, vertices, faces, mat, bevel)


def cylinder(name, radius, depth, center, mat, axis='Y', vertices=20):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=depth, location=center)
    obj = register(bpy.context.object, name, mat)
    if axis == 'Y':
        obj.rotation_euler.x = math.pi/2
    elif axis == 'X':
        obj.rotation_euler.y = math.pi/2
    return finish(obj, min(.003, depth*.23, radius*.23))


def diamond_ring(name, center_y, radius, inner, depth, mat):
    outer = [(0, center_y-radius), (radius, center_y), (0, center_y+radius), (-radius, center_y)]
    inside = [(0, center_y-inner), (inner, center_y), (0, center_y+inner), (-inner, center_y)]
    points = [(x, y, z) for y, shape in ((-depth/2, outer), (-depth/2, inside),
                                       (depth/2, outer), (depth/2, inside)) for x, z in shape]
    faces = []
    for i in range(4):
        j = (i+1)%4
        faces.extend([(i,j,j+4,i+4), (i+8,i+12,j+12,j+8), (i,i+8,j+8,j), (i+4,j+4,j+12,i+12)])
    return mesh(name, points, faces, mat, .004)


def group(name, pivot=(0, 0, 0)):
    global GROUP
    select(GROUP)
    if len(GROUP) > 1:
        bpy.ops.object.join()
    obj = bpy.context.object
    obj.name = CURRENT['entry']['rootName'] + '__' + name
    obj.data.name = obj.name
    # 子部品は独自の底面原点を持ち、組立位置はノードの平行移動だけで表す。
    obj.data.transform(Matrix.Translation(-Vector(pivot)) @ obj.matrix_world)
    obj.matrix_world = Matrix.Translation(Vector(pivot))
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=1.15, island_margin=.025)
    bpy.ops.object.mode_set(mode='OBJECT')
    obj.data.calc_loop_triangles()
    assert len(obj.data.materials) and all(obj.data.materials)
    assert all(0 <= f.material_index < len(obj.data.materials) for f in obj.data.polygons)
    assert all(t.area > 1e-12 for t in obj.data.loop_triangles), obj.name
    assert all(-1e-5 <= value <= 1.00001 for loop in obj.data.uv_layers.active.data for value in loop.uv)
    CURRENT['meshes'].append(obj)
    GROUP = []
    return obj


def export(path, objects):
    select(objects)
    bpy.ops.export_scene.fbx(filepath=str(path), use_selection=True, object_types={'MESH'},
        global_scale=1, apply_unit_scale=True, apply_scale_options='FBX_SCALE_UNITS',
        axis_forward='-Z', axis_up='Y', use_space_transform=True, bake_space_transform=True,
        use_mesh_modifiers=True, use_triangles=True, mesh_smooth_type='OFF',
        bake_anim=False, add_leaf_bones=False, use_custom_props=True)


def xyz(point):
    return [round(point.x, 7), round(point.z, 7), round(-point.y, 7)]


def bounds(objects, world=True):
    points = [obj.matrix_world@v.co if world else v.co for obj in objects for v in obj.data.vertices]
    coords = [xyz(p) for p in points]
    low = [min(p[i] for p in coords) for i in range(3)]
    high = [max(p[i] for p in coords) for i in range(3)]
    return {'min': low, 'max': high, 'size': [round(b-a, 7) for a,b in zip(low,high)]}


EXPORTED, EXPECTED_BOUNDS = set(), {}
def end():
    entry, objects = CURRENT['entry'], CURRENT['meshes']
    bpy.context.view_layer.update()
    initial = bounds(objects)
    cx = (initial['min'][0]+initial['max'][0])*.5
    cz = (initial['min'][2]+initial['max'][2])*.5
    bottom = initial['min'][1]
    # 前面記号の薄い厚みも含めて、組立全体の底面中央を原点に統一する。
    for obj in objects:
        obj.location += Vector((-cx,cz,-bottom))
    bpy.context.view_layer.update()
    entry['bounds'] = bounds(objects)
    entry['triangles'] = sum(len(o.data.loop_triangles) for o in objects)
    entry['animations'] = []
    entry['nodes'] = [{'name':o.name, 'restTranslation':xyz(o.location),
                       'boundsLocal':bounds([o], False),
                       'materials':[m.name for m in o.data.materials]} for o in objects]
    export(entry['file'], objects)
    EXPECTED_BOUNDS[entry['file']] = entry['bounds']
    EXPORTED.add(entry['file'])
    for name, part in entry.get('parts', {}).items():
        obj = next(o for o in objects if o.name.endswith('__'+name))
        part['restTranslation'] = xyz(obj.location)
        part['bounds'] = bounds([obj], False)
        part['node'] = obj.name
        if part['file'] not in EXPORTED:
            position = obj.location.copy()
            obj.location = (0,0,0)
            export(part['file'], [obj])
            EXPECTED_BOUNDS[part['file']] = part['bounds']
            obj.location = position
            EXPORTED.add(part['file'])
    for obj in objects:
        obj.parent = CURRENT['root']
    entry['status'] = 'exported'


begin('terrain_solid')
box('Solid', (1,.44,1), (0,0,.5), 'ivory', .004)
group('Body'); end()

begin('oneway')
box('Underside', (1,.40,.10), (0,0,.05), 'ink', .004)
box('Surface', (1,.44,.06), (0,0,.13), 'ivory', .004)
group('Body'); end()

begin('pressure_plate')
box('Base', (1,.48,.04), (0,0,.02), 'ink', .004)
for x in (-.46,.46):
    box('EndGuide', (.08,.48,.14), (x,0,.07), 'ink', .005)
group('Base')
box('MovingPlate', (.80,.40,.055), (0,0,.1025), 'ivory', .006)
group('Plate', (0,0,.075)); end()

begin('door')
for x in (-.465,.465):
    box('Upright', (.07,.40,1), (x,0,.5), 'ink', .006)
box('Lintel', (1,.40,.08), (0,0,.96), 'ink', .006)
box('Sill', (1,.40,.025), (0,0,.0125), 'ink', .003)
group('Frame')
box('Leaf', (.84,.28,.88), (0,0,.475), 'ivory', .012)
group('Leaf', (0,0,.035)); end()

begin('laser_emitter')
box('Foot', (.48,.44,.04), (0,0,.02), 'ink', .004)
box('Head', (.40,.40,.40), (-.04,0,.24), 'ivory', .016)
cylinder('Nozzle', .075, .08, (.20,0,.24), 'ink', 'X')
cylinder('Aperture', .039, .009, (.2355,0,.24), 'beam', 'X')
prism('DirectionArrow', [(-.12,.18),(-.12,.30),(.02,.24)], .008, 'danger', .212, .002)
group('Body'); end()

begin('laser_beam')
# 光束は 36 三角形の効果用部品。長さだけを X 方向へ伸ばして使う。
box('Envelope', (1,.040,.036), (0,0,.018), 'beam', 0)
for y in (-.021,.021):
    box('BrightCore', (1,.004,.009), (0,y,.018), 'core', 0)
group('Body'); end()
entries['laser_beam']['centerAnchor'] = [0,.018,0]

begin('spikes')
box('Base', (1,.40,.08), (0,0,.04), 'ink', .004)
for x in (-.32,0,.32):
    prism('Tooth', [(x-.14,.08),(x+.14,.08),(x,.65)], .32, 'danger', bevel=.013)
group('Body'); end()

for state in ('idle','active'):
    begin('checkpoint_'+state)
    box('Foot', (.55,.40,.08), (0,0,.04), 'ink', .006)
    box('Stem', (.10,.18,.37), (0,0,.235), 'ink', .006)
    group('Base')
    diamond_ring('MarkerRing', .60, .30, .205, .12, 'ivory' if state=='idle' else 'signal')
    if state=='active':
        prism('FilledMarker', [(-.176,.60),(0,.776),(.176,.60),(0,.424)], .10, 'signal', bevel=.006)
    group('Marker',(0,0,.30)); end()

begin('lockwall')
prism('OctagonalBlock', [(-.40,0),(.40,0),(.5,.10),(.5,.90),(.40,1),(-.40,1),(-.5,.90),(-.5,.10)],
      .44, 'ink', bevel=.006)
box('LockBody', (.38,.05,.28), (0,-.20,.43), 'ivory', .015)
box('LockRear', (.38,.05,.28), (0,.20,.43), 'ink', .015)
# 大きい錠前記号は立体で表し、縮小表示でも地形と区別する。
for x in (-.11,.11):
    box('LockLoop', (.055,.046,.19), (x,-.20,.655), 'ivory', .012)
box('LockLoopTop', (.275,.046,.055), (0,-.20,.7375), 'ivory', .012)
cylinder('Keyhole', .034, .004, (0,-.227,.46), 'ink', vertices=16)
box('KeyholeStem', (.029,.004,.085), (0,-.227,.419), 'ink', .001)
group('Body'); end()

begin('fx_ring')
bpy.ops.mesh.primitive_torus_add(major_segments=32, minor_segments=6, major_radius=.465, minor_radius=.035,
                                location=(0,0,.5), rotation=(math.pi/2,0,0))
finish(register(bpy.context.object,'Ring','signal'))
group('Body'); end()

begin('fx_shard')
mesh('Shard', [(0,0,0),(0,0,.45),(-.10,0,.18),(.10,0,.18),(0,-.07,.18),(0,.07,.18)],
     [(0,2,4),(0,4,3),(0,3,5),(0,5,2),(1,4,2),(1,3,4),(1,5,3),(1,2,5)], 'signal')
group('Body'); end()

begin('fx_streak')
prism('Streak', [(-.5,.04),(-.36,0),(.40,.025),(.5,.06),(.40,.095),(-.36,.12)], .05, 'signal', bevel=0)
group('Body'); end()
entries['fx_streak']['centerAnchor'] = [0,.06,0]

# 編集用の集合だけを保存し、背景や撮影装置は導入モデルへ含めない。
bpy.context.window.scene = BUILT[0]['scene']
bpy.ops.wm.save_as_mainfile(filepath=str(ART/'ScreenGame06_Props.blend'))


def child(element,name):
    return next(e for e in element.elems if e.id==name)


file_checks = {}
for path in sorted(EXPORTED):
    document,_ = parse_fbx.parse(path)
    objects = child(document,b'Objects').elems
    materials = [e for e in objects if e.id==b'Material']
    names = [e.props[1].split(b'\0')[0].decode() for e in materials]
    links = child(document,b'Connections').elems
    for geometry in (e for e in objects if e.id==b'Geometry'):
        model_id = next(e.props[2] for e in links if e.props[1]==geometry.props[0])
        slots = [e.props[1] for e in links if e.props[2]==model_id and e.props[1] in {m.props[0] for m in materials}]
        indices = child(child(geometry,b'LayerElementMaterial'),b'Materials').props[0]
        assert slots and all(0 <= index < len(slots) for index in indices)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=path,use_anim=False)
    objects = [o for o in bpy.context.scene.objects if o.type=='MESH']
    measured = bounds(objects)
    for bound in ('min','max'):
        assert all(abs(a-b)<1e-5 for a,b in zip(measured[bound],EXPECTED_BOUNDS[path][bound])), (path,measured)
    for obj in objects:
        obj.data.calc_loop_triangles()
        assert all(t.area > 1e-12 for t in obj.data.loop_triangles)
        assert all(math.isfinite(v) for vert in obj.data.vertices for v in vert.normal)
        assert all(abs(v-1) < 1e-5 for v in obj.scale)
        assert obj.data.uv_layers.active
        assert all(-1e-5 <= v <= 1.00001 for uv in obj.data.uv_layers.active.data for v in uv.uv)
    file_checks[path] = {'materialIndices':{name:i for i,name in enumerate(names)},
                         'fbxReadbackPassed':True,
                         'triangles':sum(len(o.data.loop_triangles) for o in objects)}
    print('PROP_READY', Path(path).name, file_checks[path]['triangles'], flush=True)

for entry in contract['models']:
    if entry['id']=='character_ninja':
        continue
    entry.update(file_checks[entry['file']])
    entry['status']='ready'
    for part in entry.get('parts',{}).values():
        part.update(file_checks[part['file']])
contract['sourceProps']=str(ART/'ScreenGame06_Props.blend')
for path in (ART/'manifest.json',CONTRACT):
    path.write_text(json.dumps(contract,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('PROPS_COMPLETE',len(EXPORTED),flush=True)
