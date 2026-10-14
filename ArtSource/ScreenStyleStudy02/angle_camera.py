"""水平な横スクロール画面を維持し、俯角だけで上面を少し見せる。"""

import json
import math
from pathlib import Path

import bpy
from mathutils import Vector


ROOT = Path('D:/PJ/TD2_TV')
OUTPUT = ROOT / 'ArtSource/ScreenStyleStudy02/ScreenStyleStudy02_GameShapes_Level.blend'
PREVIEW = ROOT / 'Library/ScreenStyleStudy02'
scene = bpy.context.scene
source = scene.camera

# 正面カメラは比較用に残す。透視投影や地形変形ではなく、正投影の向きだけを変える。
camera = source.copy()
camera.data = source.data.copy()
camera.name = 'Screen orthographic camera | front 7deg above'
source.users_collection[0].objects.link(camera)
target = Vector((8,0,4.5))
# 水平方向の旋回をゼロにし、床の投影が左右へ傾かないようにする。
azimuth,elevation = 0.0,math.radians(7)
distance = 24
camera.location = target + distance*Vector((math.sin(azimuth)*math.cos(elevation),
                                           -math.cos(azimuth)*math.cos(elevation),
                                           math.sin(elevation)))
camera.rotation_euler = (target-camera.location).to_track_quat('-Z','Y').to_euler()
camera.data.type = 'ORTHO'
camera.data.ortho_scale = 17.3
camera['azimuth_degrees'] = 0
camera['elevation_degrees'] = 7
scene.camera = camera
scene['projection_study'] = 'Orthographic; zero side turn, 7 degrees above. Horizontal platforms remain horizontal.'

# 保存時の表示も描画カメラへ合わせる。利用者の別ファイルは変更しない。
for screen in bpy.data.screens:
    for editor in screen.areas:
        if editor.type == 'VIEW_3D':
            space = editor.spaces.active
            space.camera = camera
            space.use_local_camera = False
            space.region_3d.view_rotation = camera.rotation_euler.to_quaternion()
            space.region_3d.view_location = target
            space.region_3d.view_distance = distance
            space.region_3d.view_perspective = 'CAMERA'
            space.shading.type = 'RENDERED'
            space.shading.use_compositor = 'CAMERA'
            space.overlay.show_overlays = False
scene.render.resolution_x,scene.render.resolution_y = 1920,1080
scene.render.filepath = str(PREVIEW/'game_shapes_level.png')
scene['preview_path'] = scene.render.filepath
bpy.ops.wm.save_as_mainfile(filepath=str(OUTPUT))
bpy.ops.render.render(write_still=True)
scene.render.resolution_x,scene.render.resolution_y = 384,216
scene.render.filepath = str(PREVIEW/'game_shapes_level_small.png')
bpy.ops.render.render(write_still=True)
print(json.dumps({'blend':str(OUTPUT),'camera':camera.name,'type':camera.data.type,
                  'yaw':0,'elevation':7,'render':str(PREVIEW/'game_shapes_level.png')}))
