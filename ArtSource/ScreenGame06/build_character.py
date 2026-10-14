# -*- coding: utf-8 -*-
"""承認済みの忍者を二色へ統一し、既存の骨と四つの動作を FBX に保つ。"""
import json
from pathlib import Path

import bpy
from mathutils import Vector
from io_scene_fbx import parse_fbx

ROOT = Path('D:/PJ/TD2_TV')
ART = ROOT/'ArtSource/ScreenGame06'
CONTRACT = ROOT/'Assets/TD2/Screen3D/Models/screen3d_contract.json'
contract = json.loads(CONTRACT.read_text(encoding='utf-8'))
entry = next(e for e in contract['models'] if e['id']=='character_ninja')
assert bpy.app.background
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.filepaths.save_version = 0
scene = bpy.context.scene
scene.name = 'SG06_Ninja'
scene.render.fps = 60
scene.unit_settings.system = 'METRIC'
scene.unit_settings.scale_length = 1
bpy.ops.import_scene.gltf(filepath=entry['source'])
rig = bpy.data.objects['SoftHuman_Rig']
meshes = [o for o in scene.objects if o.type=='MESH' and o.parent==rig]
assert len(meshes)==3
bone_names = sorted(b.name for b in rig.data.bones)
actions = {a.name:a for a in bpy.data.actions}


def material(name, color):
    values=[int(color[i:i+2],16)/255 for i in (0,2,4)]
    rgba=tuple(v/12.92 if v<=.04045 else ((v+.055)/1.055)**2.4 for v in values)+(1,)
    mat=bpy.data.materials.new(name)
    mat.use_nodes=True
    mat.diffuse_color=rgba
    bsdf=mat.node_tree.nodes.get('Principled BSDF')
    bsdf.inputs['Base Color'].default_value=rgba
    bsdf.inputs['Metallic'].default_value=0
    bsdf.inputs['Roughness'].default_value=.86
    return mat


ivory=material('SG06_Ivory','DDD5C1')
ink=material('SG06_Ink','435969')
for obj in meshes:
    obj.data.materials.clear()
    obj.data.materials.append(ink if obj.name=='Ninja_Headband' else ivory)
    for polygon in obj.data.polygons:
        polygon.material_index=0

collection=bpy.data.collections.new('SG06_Ninja')
scene.collection.children.link(collection)
for obj in [rig,*meshes]:
    for previous in list(obj.users_collection):
        previous.objects.unlink(obj)
    collection.objects.link(obj)
root=bpy.data.objects.new('SG06_Ninja',None)
collection.objects.link(root)
rig.parent=root


def bounds():
    bpy.context.view_layer.update()
    dg=bpy.context.evaluated_depsgraph_get()
    points=[obj.evaluated_get(dg).matrix_world@v.co for obj in meshes
            for v in obj.evaluated_get(dg).data.vertices]
    low=[min(p[i] for p in points) for i in range(3)]
    high=[max(p[i] for p in points) for i in range(3)]
    return {'min':[low[0],low[2],-high[1]],'max':[high[0],high[2],-low[1]],
            'size':[high[0]-low[0],high[2]-low[2],high[1]-low[1]]}


for track in rig.animation_data.nla_tracks:
    track.mute=True
rig.data.pose_position='REST'
scene.frame_set(0)
original=bounds()
# FBX のスキンと NLA の二重変換を避け、キャラクターだけは元の足元リグ原点を保持する。
# 必要な描画位置補正は契約へ出し、ルールのプレイヤー位置と独立に適用できるようにする。
root.location=(0,0,0)
rest=bounds()
height=rest['size'][1]
entry['bounds']=rest
entry['sourceBoundsBeforePivotRebase']=original
entry['naturalRestHeight']=height
entry['uniformScaleForOneCellHeight']=1/height
entry['assetRootTranslation']=[root.location.x,root.location.z,-root.location.y]
entry['pivot']='original_rig_feet_origin'
entry['bottomCenterRenderOffset']=[-(original['min'][0]+original['max'][0])/2,
                                   -original['min'][1],-(original['min'][2]+original['max'][2])/2]
entry['sideFacingRightYawDegrees']=90
entry['sourceBoneCount']=len(bone_names)

rig.data.pose_position='POSE'
signatures={}
for name,action in actions.items():
    rig.animation_data.action=action
    rig.animation_data.action_slot=action.slots[0]
    duration=(action.frame_range[1]-action.frame_range[0])/scene.render.fps
    signatures[name]=[]
    for seconds in (0,duration/2,duration):
        frame=seconds*scene.render.fps
        scene.frame_set(int(frame),subframe=frame-int(frame))
        signatures[name].append(bounds())

# glTF の NLA ストリップを個別 Take として出し、動作名と秒数を変えない。
rig.animation_data.action=actions['Ninja_Idle']
rig.animation_data.action_slot=actions['Ninja_Idle'].slots[0]
for track in rig.animation_data.nla_tracks:
    track.mute=True
    for strip in track.strips:
        strip.frame_start=0
        strip.action_frame_start=0
        strip.action_frame_end=strip.action.frame_range[1]
        strip.frame_end=strip.action.frame_range[1]
        strip.extrapolation='NOTHING'
        strip.blend_type='REPLACE'
bpy.ops.object.select_all(action='DESELECT')
for obj in [root,rig,*meshes]:
    obj.select_set(True)
bpy.context.view_layer.objects.active=rig
# バインド評価を Idle の先頭へ戻し、直前の動作サンプルの姿勢を出力へ持ち越さない。
scene.frame_set(0)
bpy.context.view_layer.update()
bpy.ops.export_scene.fbx(filepath=entry['file'],use_selection=True,object_types={'EMPTY','ARMATURE','MESH'},
    global_scale=1,apply_unit_scale=True,apply_scale_options='FBX_SCALE_UNITS',
    axis_forward='-Z',axis_up='Y',use_space_transform=True,bake_space_transform=False,
    use_mesh_modifiers=True,mesh_smooth_type='OFF',use_triangles=True,add_leaf_bones=False,
    armature_nodetype='NULL',use_armature_deform_only=False,bake_anim=True,
    bake_anim_use_all_bones=True,bake_anim_use_nla_strips=False,bake_anim_use_all_actions=True,
    bake_anim_step=1,bake_anim_simplify_factor=0,path_mode='AUTO',embed_textures=False)

for track in rig.animation_data.nla_tracks:
    track.mute=True
rig.animation_data.action=actions['Ninja_Idle']
rig.animation_data.action_slot=actions['Ninja_Idle'].slots[0]
scene.frame_start,scene.frame_end=0,60
scene.frame_set(0)
# インポータの骨表示用 Icosphere は製品メッシュではないため、保存時も非表示にする。
for obj in scene.objects:
    if obj.type=='MESH' and obj not in meshes:
        obj.hide_render=True
        obj.hide_viewport=True
bpy.ops.wm.save_as_mainfile(filepath=str(ART/'ScreenGame06_Ninja.blend'))


def child(element,name):
    return next(e for e in element.elems if e.id==name)


doc,_=parse_fbx.parse(entry['file'])
objects=child(doc,b'Objects').elems
clips=[]
for stack in (e for e in objects if e.id==b'AnimationStack'):
    name=stack.props[1].split(b'\0')[0].decode()
    props={p.props[0]:p.props[-1] for p in child(stack,b'Properties70').elems}
    # FBX のゼロ時刻は既定値なので、プロパティ自体が省略される場合がある。
    start=props.get(b'LocalStart',0)
    clips.append({'name':name,'sourceName':name.rsplit('|',1)[-1],'durationSeconds':(props[b'LocalStop']-start)/46186158000,
                  'startSeconds':start/46186158000})
assert sorted(c['sourceName'] for c in clips)==sorted(signatures), clips
names=[e.props[1].split(b'\0')[0].decode() for e in objects if e.id==b'Material']
entry['materialIndices']={name:i for i,name in enumerate(names)}
entry['animationClips']=clips

# 最終ファイルを実際に回読し、各動作の始点・中間・終点の変形境界を照合する。
bpy.ops.wm.read_factory_settings(use_empty=True)
scene=bpy.context.scene
scene.render.fps=60
bpy.ops.import_scene.fbx(filepath=entry['file'],use_anim=True,anim_offset=0)
rig=next(o for o in scene.objects if o.type=='ARMATURE')
meshes=[o for o in scene.objects if o.type=='MESH' and o.name in ('Body_Base','Head_Base','Ninja_Headband')]
assert len(meshes)==3
assert set(bone_names).issubset({b.name for b in rig.data.bones})
# Blender の FBX 読込は一致する骨端を自動接続し、Hips の移動を拘束する。
# FBX の移動チャンネル自体を検査するため、検証用リグだけで接続拘束を解除する。
connected=[b.name for b in rig.data.bones if b.use_connect]
bpy.context.view_layer.objects.active=rig
rig.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
for bone in rig.data.edit_bones:
    bone.use_connect=False
bpy.ops.object.mode_set(mode='OBJECT')
for track in rig.animation_data.nla_tracks:
    track.mute=True
max_error=0
origin_delta=None
for clip in clips:
    action=next(a for a in bpy.data.actions if a.name.endswith(clip['name']))
    rig.animation_data.action=action
    rig.animation_data.action_slot=action.slots[0]
    for index,seconds in enumerate((0,clip['durationSeconds']/2,clip['durationSeconds'])):
        frame=(clip['startSeconds']+seconds)*60
        scene.frame_set(int(frame),subframe=frame-int(frame))
        measured=bounds()
        expected=signatures[clip['sourceName']][index]
        if origin_delta is None:
            origin_delta=[(measured['min'][i]+measured['max'][i]-expected['min'][i]-expected['max'][i])/2 for i in range(3)]
        # FBX 回読で生じる一定の原点差を分離し、動作自体の形状・移動差を検査する。
        error=max(abs(measured[key][i]-origin_delta[i]-expected[key][i]) for key in ('min','max') for i in range(3))
        max_error=max(max_error,error)
        print('POSE_BOUNDS_ERROR',clip['name'],seconds,error,flush=True)
        assert error<.004, (clip['name'],seconds,error,measured,expected)
for obj in meshes:
    assert obj.data.uv_layers.active and len(obj.data.materials)==1
    obj.data.calc_loop_triangles()
entry['triangles']=sum(len(o.data.loop_triangles) for o in meshes)
entry['animations']=[c['name'] for c in clips]
entry['fbxReadbackPassed']=True
entry['animationPoseBoundsMaxErrorMeters']=max_error
entry['fbxReadbackOriginDelta']=origin_delta
entry['fbxReadbackFeetCorrection']=[-v for v in origin_delta]
entry['readbackNote']='Blender automatic bone-connect constraints disabled during QA so exported bone translation channels can evaluate.'
entry['readbackConnectedBonesReleased']=connected
entry['readbackBoneCount']=len(rig.data.bones)
entry['status']='ready'
contract['sourceCharacter']=str(ART/'ScreenGame06_Ninja.blend')
contract['status']='models_ready'
for path in (ART/'manifest.json',CONTRACT):
    path.write_text(json.dumps(contract,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('NINJA_READY',json.dumps({'height':height,'scale':1/height,'clips':clips,'error':max_error}),flush=True)
