# -*- coding: utf-8 -*-
"""GPU を使わず、暗い同系色の背景で LockWall の輪郭と厚みを確認する。"""
import json
import math
from pathlib import Path

import bpy
from mathutils import Vector

ROOT = Path('D:/PJ/TD2_TV')
assert bpy.app.background
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=str(ROOT/'Assets/TD2/Screen3D/Models/SG06_LockWall.fbx'), use_anim=False)
collection = bpy.data.collections.new('Frozen LockWall readback')
bpy.context.scene.collection.children.link(collection)
for obj in list(bpy.context.scene.objects):
    for old in list(obj.users_collection):
        old.objects.unlink(obj)
    collection.objects.link(obj)
scene = bpy.data.scenes.new('CPU outline and depth proof')
bpy.context.window.scene = scene
for x,yaw in ((-.72,0),(.72,30)):
    instance = bpy.data.objects.new('Front' if yaw==0 else 'Three quarter',None)
    instance.instance_type = 'COLLECTION'
    instance.instance_collection = collection
    instance.location = (x,0,0)
    instance.rotation_euler.z = math.radians(yaw)
    scene.collection.objects.link(instance)


def aim(obj,target):
    obj.rotation_euler = (Vector(target)-obj.location).to_track_quat('-Z','Y').to_euler()


# 背景の色と照明を分け、青灰色の正面が背景へ近づく条件を確認する。
world = bpy.data.worlds.new('Ink camera background')
world.use_nodes = True
scene.world = world
nodes,links = world.node_tree.nodes,world.node_tree.links
nodes.clear()
output = nodes.new('ShaderNodeOutputWorld')
mix = nodes.new('ShaderNodeMixShader')
path = nodes.new('ShaderNodeLightPath')
ambient = nodes.new('ShaderNodeBackground')
ambient.inputs['Color'].default_value = (.5,.5,.5,1)
ambient.inputs['Strength'].default_value = .65
camera_background = nodes.new('ShaderNodeBackground')
camera_background.inputs['Color'].default_value = (.056,.100,.141,1)
links.new(path.outputs['Is Camera Ray'],mix.inputs[0])
links.new(ambient.outputs[0],mix.inputs[1])
links.new(camera_background.outputs[0],mix.inputs[2])
links.new(mix.outputs[0],output.inputs[0])
light = bpy.data.lights.new('Soft key','AREA')
light.energy,light.size = 280,4
lamp = bpy.data.objects.new('Soft key',light)
scene.collection.objects.link(lamp)
lamp.location = (-3,-4,5)
aim(lamp,(0,0,.5))
camera_data = bpy.data.cameras.new('Orthographic elevation 7')
camera = bpy.data.objects.new(camera_data.name,camera_data)
scene.collection.objects.link(camera)
camera.location = (0,-8,.5+8*math.tan(math.radians(7)))
aim(camera,(0,0,.5))
camera_data.type = 'ORTHO'
camera_data.ortho_scale = 2.95
scene.camera = camera
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 32
scene.cycles.use_denoising = False
scene.render.resolution_x,scene.render.resolution_y = 900,420
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'PNG'
scene.view_settings.view_transform = 'Standard'
output_path = ROOT/'Library/Screen3D06/lockwall_revision_cpu.png'
scene.render.filepath = str(output_path)
bpy.ops.render.render(write_still=True)
contract = ROOT/'Assets/TD2/Screen3D/Models/screen3d_contract.json'
data = json.loads(contract.read_text(encoding='utf-8'))
entry = next(e for e in data['models'] if e['id']=='lockwall')
entry['preview'] = {'file':str(output_path),'source':'Final FBX readback',
                    'renderer':'Blender Cycles CPU','engineVisualVerified':False}
for file in (contract,ROOT/'ArtSource/ScreenGame06/manifest.json'):
    file.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('CPU_PREVIEW_READY',str(output_path),flush=True)
