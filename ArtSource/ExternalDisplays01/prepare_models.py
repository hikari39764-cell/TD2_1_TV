# -*- coding: utf-8 -*-
"""原形を保ち、整数マス比率・共通原点・独立した画面槽を持つ FBX を生成する。"""
import bpy, math, json, shutil, hashlib
from pathlib import Path
from mathutils import Matrix, Vector
from io_scene_fbx import parse_fbx

assert bpy.app.background, 'Use a separate background Blender process'
SOURCE = Path('D:/GAME PJ/TD2——1/model')
ROOT = Path(__file__).resolve().parent
WORK = Path('D:/PJ/TD2_TV/Library/ModelPrep_20261015')
for folder in ['Exports', 'Blender', 'Textures', 'Previews']:
    (ROOT/folder).mkdir(parents=True, exist_ok=True)
IDENTITY = Matrix.Identity(4)
CASES = [
    dict(id='SM_TD2_TV_FlatStand', source=SOURCE/'TV001.blend', object='TV_.001', faces=[0], rotation=Matrix.Rotation(math.pi,4,'Z'), grid=[23,14], texture=SOURCE/'TV_001.png'),
    dict(id='SM_TD2_TV_Antenna', source=SOURCE/'TV_002.blend', object='TV_', faces=[0], rotation=Matrix.Rotation(math.pi,4,'Z'), grid=[21,16], texture=SOURCE/'TV_002.png'),
    dict(id='SM_TD2_Display_Portrait', source=SOURCE/'monitor.blend', object='monitor', faces=[93], rotation=Matrix.Rotation(math.pi,4,'Z'), grid=[11,24], texture=SOURCE/'monitor.png'),
    dict(id='SM_TD2_Handheld_Classic', source=SOURCE/'GB.blend', object='円柱.001', faces=[130], rotation=Matrix.Rotation(math.pi/2,4,'X'), grid=[23,20], texture=SOURCE/'texture.png'),
    dict(id='SM_TD2_Handheld_Wide', source=WORK/'zip/switch/switch.obj', object='body', faces=[229,1105], rotation=Matrix(((0,-1,0,0),(0,0,-1,0),(1,0,0,0),(0,0,0,1))), grid=[26,12], texture=WORK/'zip/switch/texture.png'),
]

def bounds(points):
    return Vector([min(p[i] for p in points) for i in range(3)]), Vector([max(p[i] for p in points) for i in range(3)])

def pbr(name, image=None, screen=False):
    mat=bpy.data.materials.new(name);mat.use_nodes=True
    shader=mat.node_tree.nodes.get('Principled BSDF')
    shader.inputs['Base Color'].default_value=(.004,.006,.006,1) if screen else (.8,.8,.8,1)
    shader.inputs['Roughness'].default_value=.07 if screen else .5
    shader.inputs['Metallic'].default_value=0
    shader.inputs['Emission Strength'].default_value=0
    mat.diffuse_color=shader.inputs['Base Color'].default_value
    if image:
        tex=mat.node_tree.nodes.new('ShaderNodeTexImage');tex.image=image
        mat.node_tree.links.new(tex.outputs['Color'],shader.inputs['Base Color'])
        mat.node_tree.nodes.active=tex
    return mat

def mesh_object(name, vertices, faces, uvs, material, smoothing=None):
    mesh=bpy.data.meshes.new(name);mesh.from_pydata(vertices,[],faces);mesh.update()
    mesh.materials.append(material);layer=mesh.uv_layers.new(name='UVMap')
    for p,face_uv in zip(mesh.polygons,uvs):
        if smoothing:p.use_smooth=smoothing[p.index]
        for loop,uv in zip(p.loop_indices,face_uv):layer.data[loop].uv=uv
    obj=bpy.data.objects.new(name,mesh);bpy.context.scene.collection.objects.link(obj)
    obj.matrix_world=IDENTITY
    return obj

def stage_camera(scene, objects, path):
    scene.render.engine='BLENDER_WORKBENCH'
    scene.render.resolution_x=960;scene.render.resolution_y=800;scene.render.resolution_percentage=100
    scene.world=scene.world or bpy.data.worlds.new('PreviewWorld');scene.world.color=(.085,.085,.085)
    shading=scene.display.shading;shading.light='STUDIO';shading.color_type='TEXTURE';shading.show_shadows=True
    shading.show_cavity=True;shading.cavity_type='BOTH';shading.background_type='WORLD'
    points=[o.matrix_world@v.co for o in objects for v in o.data.vertices];lo,hi=bounds(points);center=(lo+hi)*.5;span=max(hi-lo)
    data=bpy.data.cameras.new('PreviewOnly');cam=bpy.data.objects.new('PreviewOnly',data);scene.collection.objects.link(cam)
    data.type='ORTHO';data.ortho_scale=span*1.28;scene.camera=cam
    cam.location=center+Vector((.8,-2,.65)).normalized()*span*3
    cam.rotation_euler=(center-cam.location).to_track_quat('-Z','Y').to_euler()
    scene.render.filepath=str(path);bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam,do_unlink=True)

def child(element,name):return next(x for x in element.elems if x.id==name)

manifest={'set':'ExternalDisplays01','conventions':{'units':'meters','up':'+Y','front':'+Z','pivot':'bottom_center','node_transforms':'identity','screen_material':'TD2_Screen','screen_mesh':'<asset>__Screen','screen_uv':'full 0..1; Blender bottom-left (0,0)','tile_pixels':64,'dimensions_order':'width_height_depth','engine_imported':False},'parts':[],'excluded_references':['TV01_Test.fbx (existing SM_TV_PortableCRT baseline)','TV_002.blend: reference PortableCRT and duplicate TV_.001','obj.zip/GB (duplicate of GB.blend)']}
for case in CASES:
    name=case['id'];path=case['source']
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if path.suffix=='.blend':bpy.ops.wm.open_mainfile(filepath=str(path),load_ui=False,use_scripts=False)
    else:bpy.ops.wm.obj_import(filepath=str(path))
    original=bpy.data.objects[case['object']]
    evaluated=original.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=evaluated.to_mesh()
    world=original.matrix_world.copy();initial=case['rotation']@world
    selected=[mesh.polygons[i] for i in case['faces']]
    # 負のオブジェクト縮尺を頂点へ焼き込む時は巻き順も反転し、表裏を維持する。
    reflected=initial.to_3x3().determinant()<0
    normal=(initial.to_3x3().inverted().transposed()@selected[0].normal).normalized()
    correction=normal.rotation_difference(Vector((0,-1,0))).to_matrix().to_4x4()
    transform=correction@initial
    points=[transform@v.co for v in mesh.vertices]
    screen_indices=sorted({i for p in selected for i in p.vertices})
    screen_points=[points[i] for i in screen_indices];lo,hi=bounds(screen_points)
    original_ratio=(hi.x-lo.x)/(hi.z-lo.z);cols,rows=case['grid'];target_ratio=cols/rows
    # サイズ基準を押し付けず、元の高さを保って横比だけを整数マスへ微調整する。
    ratio_scale=target_ratio/original_ratio
    assert abs(ratio_scale-1)<.01,(name,ratio_scale)
    for p in points:p.x*=ratio_scale
    all_lo,all_hi=bounds(points);pivot=Vector(((all_lo.x+all_hi.x)*.5,(all_lo.y+all_hi.y)*.5,all_lo.z))
    points=[p-pivot for p in points]
    screen_points=[points[i] for i in screen_indices];lo,hi=bounds(screen_points)
    assert hi.y-lo.y<1e-5,(name,'non-planar screen')
    plane_y=sum(p.y for p in screen_points)/len(screen_points)
    face_ids=set(case['faces']);body_faces=[];body_uv=[];smoothing=[]
    uv=mesh.uv_layers.active
    for p in mesh.polygons:
        if p.index in face_ids:continue
        ids=list(p.vertices);coords=[tuple(uv.data[i].uv) for i in p.loop_indices] if uv else [(0,0)]*len(ids)
        if reflected:ids.reverse();coords.reverse()
        body_faces.append(ids);body_uv.append(coords);smoothing.append(p.use_smooth)
    evaluated.to_mesh_clear()
    # 背景中の参照テレビ、カメラ、照明は成果物へ混ぜない。
    for obj in list(bpy.data.objects):bpy.data.objects.remove(obj,do_unlink=True)
    for mat in list(bpy.data.materials):bpy.data.materials.remove(mat)
    for img in list(bpy.data.images):
        if img.name!='Render Result':bpy.data.images.remove(img)
    texture_path=ROOT/'Textures'/f'T_{name.removeprefix("SM_TD2_")}_BaseColor.png'
    shutil.copy2(case['texture'],texture_path)
    image=bpy.data.images.load(str(texture_path),check_existing=False);image.colorspace_settings.name='sRGB';image.pack()
    body=mesh_object(name+'__Body',[tuple(p) for p in points],body_faces,body_uv,pbr('M_'+name.removeprefix('SM_')+'_Body',image),smoothing)
    # 表示面は既存の画面 polygon を置換する。重ね貼りせず、同一平面の二重面を残さない。
    rectangle=[(lo.x,plane_y,lo.z),(hi.x,plane_y,lo.z),(hi.x,plane_y,hi.z),(lo.x,plane_y,hi.z)]
    screen=mesh_object(name+'__Screen',rectangle,[(0,1,2,3)],[[(0,0),(1,0),(1,1),(0,1)]],pbr('TD2_Screen',screen=True))
    body['role']='Body';screen['role']='Screen';screen['grid_cols']=cols;screen['grid_rows']=rows;screen['tile_pixels']=64
    scene=bpy.context.scene;scene.name=name;scene.unit_settings.system='METRIC';scene.unit_settings.scale_length=1
    # 原本の使いかけの Scene や collection も別ファイルへ持ち出さない。
    for other in list(bpy.data.scenes):
        if other!=scene:bpy.data.scenes.remove(other)
    for coll in list(bpy.data.collections):
        if len(coll.objects)==0:bpy.data.collections.remove(coll)
    bpy.ops.object.select_all(action='DESELECT');body.select_set(True);screen.select_set(True);bpy.context.view_layer.objects.active=body
    image.filepath='//../Textures/'+texture_path.name
    blend_path=ROOT/'Blender'/f'{name}.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path),check_existing=False)
    image.filepath=str(texture_path)
    fbx_path=ROOT/'Exports'/f'{name}.fbx'
    bpy.ops.export_scene.fbx(filepath=str(fbx_path),use_selection=True,object_types={'MESH'},global_scale=1,apply_unit_scale=True,apply_scale_options='FBX_SCALE_UNITS',axis_forward='-Z',axis_up='Y',use_space_transform=True,bake_space_transform=True,use_mesh_modifiers=True,mesh_smooth_type='OFF',use_triangles=True,use_tspace=False,bake_anim=False,add_leaf_bones=False,path_mode='COPY',embed_textures=True,use_custom_props=True)
    size=all_hi-all_lo
    entry={'id':name,'source':str(path),'source_object':case['object'],'file':f'Exports/{name}.fbx','blend':f'Blender/{name}.blend','texture':f'Textures/{texture_path.name}','dimensions_m':[size.x,size.z,size.y],'original_screen_ratio':original_ratio,'width_adjustment_percent':(ratio_scale-1)*100,'grid':{'cols':cols,'rows':rows,'tilePixels':64,'outputWidth':cols*64,'outputHeight':rows*64},'screen':{'width_m':hi.x-lo.x,'height_m':hi.z-lo.z,'center_x_m':(hi.x+lo.x)*.5,'bottom_m':lo.z,'center_y_m':(hi.z+lo.z)*.5,'front_z_m':-plane_y,'material':'TD2_Screen'},'engine_properties':{'cols':cols,'rows':rows,'tilePixels':64,'outputWidth':cols*64,'outputHeight':rows*64,'screenSlot':'TD2_Screen','screenCenterX':(hi.x+lo.x)*.5,'screenCenterY':(hi.z+lo.z)*.5,'screenCenterZ':-plane_y,'screenWidth':hi.x-lo.x,'screenHeight':hi.z-lo.z}}
    # FBX 生データの軸・単位・原点と表示面の法線を検証する。
    doc,_=parse_fbx.parse(str(fbx_path));global_props={p.props[0]:p.props[-1] for p in child(child(doc,b'GlobalSettings'),b'Properties70').elems}
    assert global_props[b'UnitScaleFactor']==100 and global_props[b'UpAxis']==1 and global_props[b'UpAxisSign']==1
    raw=[]
    for geo in [g for g in child(doc,b'Objects').elems if g.id==b'Geometry']:
        values=child(geo,b'Vertices').props[0];coords=list(zip(values[0::3],values[1::3],values[2::3]));raw+=coords
        if b'__Screen' in geo.props[1]:
            assert all(abs(p[2]+plane_y)<1e-5 for p in coords)
            normals=child(child(geo,b'LayerElementNormal'),b'Normals').props[0]
            assert all(abs(z-1)<1e-5 for z in normals[2::3])
    assert abs(min(p[1] for p in raw))<1e-5
    for model in (x for x in child(doc,b'Objects').elems if x.id==b'Model'):
        props={p.props[0]:p.props[-3:] for p in child(model,b'Properties70').elems}
        assert all(abs(v)<1e-5 for v in props.get(b'Lcl Translation',[0,0,0])+props.get(b'Lcl Rotation',[0,0,0]))
        assert all(abs(v-1)<1e-5 for v in props.get(b'Lcl Scaling',[1,1,1]))
    bpy.ops.wm.read_factory_settings(use_empty=True);bpy.ops.import_scene.fbx(filepath=str(fbx_path),use_anim=False)
    objects=[o for o in bpy.context.scene.objects if o.type=='MESH'];assert len(objects)==2
    checked_screen=next(o for o in objects if o.name.endswith('__Screen'));checked_body=next(o for o in objects if o.name.endswith('__Body'))
    assert [m.name for m in checked_screen.data.materials]==['TD2_Screen']
    assert all(m.name!='TD2_Screen' for m in checked_body.data.materials)
    for loop in checked_screen.data.loops:
        p=checked_screen.matrix_world@checked_screen.data.vertices[loop.vertex_index].co
        mapped=checked_screen.data.uv_layers.active.data[loop.index].uv
        assert abs(mapped.x-(p.x-lo.x)/(hi.x-lo.x))<1e-5 and abs(mapped.y-(p.z-lo.z)/(hi.z-lo.z))<1e-5
    world_points=[o.matrix_world@v.co for o in objects for v in o.data.vertices];rlo,rhi=bounds(world_points);rsize=rhi-rlo
    assert max(abs(a-b) for a,b in zip([rsize.x,rsize.z,rsize.y],entry['dimensions_m']))<1e-5
    assert abs(entry['screen']['width_m']/cols-entry['screen']['height_m']/rows)<1e-6
    entry['triangles']=sum(len(o.data.polygons) for o in objects)
    entry['fbx_sha256']=hashlib.sha256(fbx_path.read_bytes()).hexdigest();entry['verified']=['raw_fbx_axes_units_origin','identity_fbx_nodes','screen_positive_Z_normal','body_screen_separation','fbx_roundtrip_size_uv','integer_grid_square_cells','embedded_texture']
    # チェッカーは回読結果の確認用だけ。FBX と編集用 blend には保存しない。
    grid=bpy.data.images.new('UV verification only',width=1024,height=1024);grid.generated_type='COLOR_GRID'
    mat=checked_screen.data.materials[0];mat.use_nodes=True;tex=mat.node_tree.nodes.new('ShaderNodeTexImage');tex.image=grid
    mat.node_tree.links.new(tex.outputs['Color'],mat.node_tree.nodes.get('Principled BSDF').inputs['Base Color']);mat.node_tree.nodes.active=tex
    stage_camera(bpy.context.scene,objects,ROOT/'Previews'/f'{name}.png')
    manifest['parts'].append(entry)
    print('READY',name,'grid',cols,rows,'width correction',round(entry['width_adjustment_percent'],4),flush=True)
(ROOT/'display_models_manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('ALL_READY',len(manifest['parts']),flush=True)
