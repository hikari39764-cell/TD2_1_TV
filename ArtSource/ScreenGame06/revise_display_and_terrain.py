# -*- coding: utf-8 -*-
"""骨なし描画用の Idle と、格子を読める地形だけを差し替える。"""
import hashlib
import json
import math
from pathlib import Path

import bmesh
import bpy
from mathutils import Matrix, Vector
from io_scene_fbx import parse_fbx

ROOT = Path('D:/PJ/TD2_TV')
ART = ROOT/'ArtSource/ScreenGame06'
MODELS = ROOT/'Assets/TD2/Screen3D/Models'
CONTRACT = MODELS/'screen3d_contract.json'
contract = json.loads(CONTRACT.read_text(encoding='utf-8'))
assert bpy.app.background
frozen = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in MODELS.glob('*.fbx')}
expected = {}
entries = {}
bpy.context.preferences.filepaths.save_version = 0


def select(objects):
    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]


def bounds(objects):
    bpy.context.view_layer.update()
    points = [obj.matrix_world @ v.co for obj in objects for v in obj.data.vertices]
    points = [(p.x, p.z, -p.y) for p in points]
    low = [min(p[i] for p in points) for i in range(3)]
    high = [max(p[i] for p in points) for i in range(3)]
    return {'min': low, 'max': high, 'size': [b-a for a, b in zip(low, high)]}


def export(identifier, filename, meshes, source, extra):
    # メッシュ座標へ変換を焼き込み、ノードの変換は単位行列に限定する。
    for obj in meshes:
        obj.data.calc_loop_triangles()
        assert all(t.area > 1e-12 for t in obj.data.loop_triangles)
        assert obj.data.uv_layers.active
        assert all(math.isfinite(x) for v in obj.data.vertices for x in v.co)
    measured = bounds(meshes)
    path = MODELS/filename
    select(meshes)
    bpy.ops.export_scene.fbx(filepath=str(path), use_selection=True, object_types={'MESH'},
        global_scale=1, apply_unit_scale=True, apply_scale_options='FBX_SCALE_UNITS',
        axis_forward='-Z', axis_up='Y', use_space_transform=True, bake_space_transform=True,
        use_mesh_modifiers=True, use_triangles=True, mesh_smooth_type='OFF',
        bake_anim=False, add_leaf_bones=False)
    bpy.ops.wm.save_as_mainfile(filepath=str(source))
    entry = next((e for e in contract['models'] if e['id'] == identifier), None)
    if entry is None:
        entry = {'id': identifier}
        contract['models'].append(entry)
    entry.update({'file': str(path), 'rootName': path.stem, 'pivot': 'bottom_center',
                  'status': 'ready', 'source': str(source), 'bounds': measured,
                  'animations': [], 'hasArmature': False,
                  'triangles': sum(len(o.data.loop_triangles) for o in meshes),
                  'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                  'engineImportVerified': False, 'runtimeVisualVerified': False,
                  'nodes': [{'name': o.name, 'restTranslation': [0, 0, 0],
                             'boundsLocal': bounds([o]),
                             'materials': [m.name for m in o.data.materials]} for o in meshes]})
    entry.update(extra)
    expected[filename] = measured
    entries[filename] = entry


# 元の編集用リグから評価済み頂点を取得する。FBX 再インポートの骨拘束を介さない。
bpy.ops.wm.open_mainfile(filepath=str(ART/'ScreenGame06_Ninja.blend'))
bpy.context.preferences.filepaths.save_version = 0
rig = bpy.data.objects['SoftHuman_Rig']
rig.data.pose_position = 'POSE'
for track in rig.animation_data.nla_tracks:
    track.mute = True
action = bpy.data.actions['Ninja_Idle']
rig.animation_data.action = action
rig.animation_data.action_slot = action.slots[0]
source_scene = bpy.context.scene
source_scene.frame_set(0)
bpy.context.view_layer.update()
depsgraph = bpy.context.evaluated_depsgraph_get()
objects = [o for o in source_scene.objects if o.type == 'MESH' and o.parent == rig]
assert len(objects) == 3
display_scene = bpy.data.scenes.new('SG06_Ninja_Display')
display_scene.unit_settings.system = 'METRIC'
display_scene.unit_settings.scale_length = 1
display = []
for original in objects:
    evaluated = original.evaluated_get(depsgraph)
    data = bpy.data.meshes.new_from_object(evaluated, preserve_all_data_layers=True, depsgraph=depsgraph)
    data.transform(evaluated.matrix_world)
    obj = bpy.data.objects.new('SG06_Ninja_Display__'+original.name, data)
    display_scene.collection.objects.link(obj)
    display.append(obj)
bpy.context.window.scene = display_scene
natural = bounds(display)
height = natural['size'][1]
offset = [-(natural['min'][0]+natural['max'][0])/2, -natural['min'][1],
          -(natural['min'][2]+natural['max'][2])/2]
normalization = Matrix.Scale(1/height, 4) @ Matrix.Translation(Vector((offset[0], -offset[2], offset[1])))
for obj in display:
    obj.data.transform(normalization)
assert abs(bounds(display)['size'][1]-1) < 1e-6
export('character_ninja_display', 'SG06_Ninja_Display.fbx', display,
       ART/'ScreenGame06_Ninja_Display.blend',
       {'sourceRig': str(ART/'ScreenGame06_Ninja.blend'), 'evaluatedAction': 'Ninja_Idle',
        'evaluatedFrame': 0, 'evaluatedSourceBounds': natural, 'sourcePoseHeight': height,
        'bakedUniformScale': 1/height, 'bakedTranslationBeforeScale': offset,
        'renderScaleForOneCellHeight': 1, 'renderOffset': [0, 0, 0],
        'sideFacingRightYawDegrees': 90,
        'coordinates': {'up': '+Y', 'right': '+X', 'front': '+Z'},
        'usage': 'Static evaluated Idle mesh for render-only paths without Animator; original animated FBX is unchanged.'})


# 溝は実体の面で作る。全面ポリゴンを重ねず、深度競合と隣接セルの隙間を防ぐ。
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.filepaths.save_version = 0
scene = bpy.context.scene
scene.name = 'SG06_Terrain_Solid'
scene.unit_settings.system = 'METRIC'
scene.unit_settings.scale_length = 1


def material(name, color):
    values = [int(color[i:i+2], 16)/255 for i in (0, 2, 4)]
    rgba = tuple(v/12.92 if v <= .04045 else ((v+.055)/1.055)**2.4 for v in values)+(1,)
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.diffuse_color = rgba
    shader = mat.node_tree.nodes.get('Principled BSDF')
    shader.inputs['Base Color'].default_value = rgba
    shader.inputs['Roughness'].default_value = .86
    shader.inputs['Specular IOR Level'].default_value = .12
    mat.use_backface_culling = True
    return mat


vertices, faces, material_ids = [], [], []


def loop(inset, depth):
    start = len(vertices)
    vertices.extend([(-.5+inset, depth, inset), (.5-inset, depth, inset),
                     (.5-inset, depth, 1-inset), (-.5+inset, depth, 1-inset)])
    return list(range(start, start+4))


def bridge(a, b, material_index):
    for i in range(4):
        j = (i+1) % 4
        faces.append((a[i], a[j], b[j], b[i]))
        material_ids.append(material_index)


outer = loop(0, -.22)
lip = loop(.010, -.22)
groove_outer = loop(.010, -.216)
groove_inner = loop(.035, -.216)
panel = loop(.035, -.22)
back = loop(0, .22)
bridge(outer, lip, 0)
bridge(lip, groove_outer, 1)
bridge(groove_outer, groove_inner, 1)
bridge(groove_inner, panel, 1)
bridge(outer, back, 0)
faces.extend([tuple(panel), tuple(reversed(back))])
material_ids.extend([0, 0])
data = bpy.data.meshes.new('SG06_Terrain_Solid__Body')
data.from_pydata(vertices, [], faces)
data.materials.append(material('SG06_Ivory', 'DDD5C1'))
data.materials.append(material('SG06_Ink', '435969'))
for polygon, material_id in zip(data.polygons, material_ids):
    polygon.material_index = material_id
bm = bmesh.new()
bm.from_mesh(data)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
bm.to_mesh(data)
bm.free()
obj = bpy.data.objects.new('SG06_Terrain_Solid__Body', data)
scene.collection.objects.link(obj)
select([obj])
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(angle_limit=1.15, island_margin=.025)
bpy.ops.object.mode_set(mode='OBJECT')
export('terrain_solid', 'SG06_Terrain_Solid.fbx', [obj], ART/'ScreenGame06_Terrain.blend',
       {'revision': 'front_geometric_inlay_01', 'frontGrooveWidth': .025,
        'frontGrooveDepth': .004, 'frontGrooveOuterInset': .010,
        'logicalCells': [1, 1], 'tileGap': 0})


def child(element, name):
    return next(e for e in element.elems if e.id == name)


# 完成した二つの FBX を回読し、実際の軸・境界・材質と骨の不在を確認する。
for filename, wanted in expected.items():
    path = MODELS/filename
    doc, _ = parse_fbx.parse(str(path))
    elements = child(doc, b'Objects').elems
    assert not any(e.id in (b'Deformer', b'AnimationStack', b'AnimationCurve') for e in elements)
    assert not any(e.id == b'Model' and e.props[-1] == b'LimbNode' for e in elements)
    settings = {e.props[0].decode(): e.props[-1] for e in child(child(doc, b'GlobalSettings'), b'Properties70').elems}
    materials = [e.props[1].split(b'\0')[0].decode() for e in elements if e.id == b'Material']
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(path), use_anim=False)
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    assert len(bpy.context.scene.objects) == len(meshes)
    measured = bounds(meshes)
    max_error = max(abs(a-b) for key in ('min', 'max') for a, b in zip(measured[key], wanted[key]))
    assert max_error < 1e-5, (filename, measured, wanted)
    for obj in meshes:
        assert all(abs(x-1) < 1e-5 for x in obj.scale)
        assert obj.data.uv_layers.active
        assert all(math.isfinite(x) for v in obj.data.vertices for x in v.normal)
        obj.data.calc_loop_triangles()
        assert all(t.area > 1e-12 for t in obj.data.loop_triangles)
    entries[filename].update({'fbxReadbackPassed': True, 'fbxReadbackBounds': measured,
                             'fbxReadbackMaxErrorMeters': max_error,
                             'materialIndices': {name: i for i, name in enumerate(materials)},
                             'fbxAxes': {k: settings[k] for k in ('UpAxis', 'UpAxisSign', 'FrontAxis', 'FrontAxisSign', 'CoordAxis', 'CoordAxisSign', 'UnitScaleFactor')}})
    print('TARGET_READY', filename, json.dumps(measured), 'error', max_error, flush=True)

for name, checksum in frozen.items():
    if name != 'SG06_Terrain_Solid.fbx':
        assert hashlib.sha256((MODELS/name).read_bytes()).hexdigest() == checksum, name
contract['engineImportVerified'] = False
contract['engineImportEvidence']['scope'] = 'Root imported the previous 21-file revision successfully; terrain revision and new static Display await targeted reimport.'
contract['pendingEngineReimport'] = ['SG06_Terrain_Solid.fbx', 'SG06_Ninja_Display.fbx']
contract['runtimeVisualVerified'] = False
contract['runtimeVerificationStatus'] = 'Root must verify static Ninja Display and readable terrain grid after targeted reimport.'
contract['exportsFrozen'] = True
contract['status'] = 'ready'
contract['sourceTerrain'] = str(ART/'ScreenGame06_Terrain.blend')
contract['sourceCharacterDisplay'] = str(ART/'ScreenGame06_Ninja_Display.blend')
contract['previews']['outdatedFor'] = ['SG06_Terrain_Solid.fbx', 'SG06_Ninja_Display.fbx']
for path in (CONTRACT, ART/'manifest.json'):
    path.write_text(json.dumps(contract, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print('TARGETED_REVISION_COMPLETE; 20 previous FBX unchanged; 22 total FBX', flush=True)
