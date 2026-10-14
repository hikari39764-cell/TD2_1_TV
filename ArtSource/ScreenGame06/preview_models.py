# -*- coding: utf-8 -*-
"""凍結した最終 FBX を読むだけの確認画像。背景部品やゲームルールは追加しない。"""
import json
import math
from pathlib import Path

import bpy
from mathutils import Vector

ROOT=Path('D:/PJ/TD2_TV')
OUT=ROOT/'Library/Screen3D06'
ART=ROOT/'ArtSource/ScreenGame06'
CONTRACT=ROOT/'Assets/TD2/Screen3D/Models/screen3d_contract.json'
data=json.loads(CONTRACT.read_text(encoding='utf-8'))
assert bpy.app.background and data['exportsFrozen']
bpy.ops.wm.read_factory_settings(use_empty=True)
collections={}
for entry in data['models']:
    scene=bpy.data.scenes.new(entry['rootName']+'__Readback')
    scene.render.fps=60
    bpy.context.window.scene=scene
    bpy.ops.import_scene.fbx(filepath=entry['file'],use_anim=True,anim_offset=0)
    collection=bpy.data.collections.new(entry['rootName']+'__Imported')
    scene.collection.children.link(collection)
    for obj in list(scene.objects):
        for old in list(obj.users_collection):old.objects.unlink(obj)
        collection.objects.link(obj)
    if entry['id']=='character_ninja':
        rig=next(o for o in collection.objects if o.type=='ARMATURE')
        bpy.context.view_layer.objects.active=rig
        rig.select_set(True)
        bpy.ops.object.mode_set(mode='EDIT')
        for bone in rig.data.edit_bones:bone.use_connect=False
        bpy.ops.object.mode_set(mode='OBJECT')
        for track in rig.animation_data.nla_tracks:track.mute=True
        action=next(a for a in bpy.data.actions if a.name.endswith('Ninja_Idle'))
        rig.animation_data.action=action
        rig.animation_data.action_slot=action.slots[0]
        scene.frame_set(0)
    collections[entry['id']]=collection

scene=bpy.data.scenes.new('ScreenGame06 / transparent game-shape proof')
bpy.context.window.scene=scene
scene.render.fps=60


def place(identifier,x,y,z=0,scale=(1,1,1),spin=0,yaw=0):
    obj=bpy.data.objects.new(identifier,None)
    obj.instance_type='COLLECTION'
    obj.instance_collection=collections[identifier]
    obj.location=(x,-z,y)
    obj.scale=(scale[0],scale[2],scale[1])
    obj.rotation_euler=(0,-math.radians(spin),math.radians(yaw))
    scene.collection.objects.link(obj)
    return obj


# 接合線は 1 セル単位で反復する。見せるために半端な地形へ伸ばさない。
for x in list(range(9))+list(range(12,16)):
    place('terrain_solid',x+.5,0)
for x in range(8,15):
    place('terrain_solid',x+.5,5)
for x in range(2,6):
    place('oneway',x+.5,3)
for x in (9,10,11):
    place('spikes',x+.5,0)
place('pressure_plate',4.5,1)
place('door',7.5,1,scale=(1,4,1))
place('lockwall',8.5,1)
place('lockwall',8.5,2)
place('checkpoint_idle',3.5,3.16)
place('checkpoint_active',14,6)
place('laser_emitter',9.2,6)
beam_start,beam_end=9.44,13.50
place('laser_beam',(beam_start+beam_end)/2,6.24-.018,scale=(beam_end-beam_start,1,1))
# Animator を持たない実行経路に合わせ、評価済みの一格高メッシュを確認する。
place('character_ninja_display',2.1,1,yaw=90)
place('character_ninja_display',12.4,6,yaw=90)
place('fx_ring',12.4,5.99,.52,scale=(1.04,1.04,1.04))
place('fx_shard',13.10,6.42,.53,scale=(.65,.65,.65),spin=-24)
place('fx_shard',11.68,6.22,.53,scale=(.45,.45,.45),spin=27)
place('fx_streak',1.28,1.16,.50,scale=(.7,.7,.7))


def aim(obj,target):
    obj.rotation_euler=(Vector(target)-obj.location).to_track_quat('-Z','Y').to_euler()


world=bpy.data.worlds.new('Neutral light only / no backdrop')
world.use_nodes=True
world.node_tree.nodes['Background'].inputs['Color'].default_value=(.13,.18,.22,1)
world.node_tree.nodes['Background'].inputs['Strength'].default_value=.8
scene.world=world
for name,pos,power,size in (
    ('Key',(-1,-5,10),1300,9),('Fill',(13,-6,7),850,8),('Top',(8,1,11),600,8)):
    light=bpy.data.lights.new(name,'AREA')
    light.energy,light.size=power,size
    obj=bpy.data.objects.new(name,light)
    scene.collection.objects.link(obj)
    obj.location=pos
    aim(obj,(8,0,4))
camera_data=bpy.data.cameras.new('Orthographic / yaw 0 / elevation 7')
camera=bpy.data.objects.new(camera_data.name,camera_data)
scene.collection.objects.link(camera)
target=Vector((8,0,3.7))
camera.location=target+24*Vector((0,-math.cos(math.radians(7)),math.sin(math.radians(7))))
aim(camera,target)
camera_data.type='ORTHO'
camera_data.ortho_scale=17.2
scene.camera=camera
scene.render.engine='CYCLES'
scene.cycles.samples=40
scene.cycles.use_denoising=True
scene.render.film_transparent=True
scene.render.image_settings.file_format='PNG'
scene.render.image_settings.color_mode='RGBA'
scene.view_settings.view_transform='AgX'
scene.frame_set(0)
for width,height,filename in ((1920,1080,'screen_game06.png'),(384,216,'screen_game06_small.png')):
    scene.render.resolution_x,scene.render.resolution_y=width,height
    scene.render.resolution_percentage=100
    scene.render.filepath=str(OUT/filename)
    bpy.ops.render.render(write_still=True)
# 撮影中に更新された統合証拠を古い値で上書きしない。
data=json.loads(CONTRACT.read_text(encoding='utf-8'))
data['previews']={'large':str(OUT/'screen_game06.png'),'small':str(OUT/'screen_game06_small.png'),
                  'background':'transparent; no background geometry',
                  'projection':'orthographic; yaw 0; elevation 7 degrees',
                  'source':'Frozen final FBX files imported into Blender; static Ninja Display and grooved terrain revision'}
for path in (ART/'manifest.json',CONTRACT):
    path.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('PREVIEW_COMPLETE',data['previews'],flush=True)
