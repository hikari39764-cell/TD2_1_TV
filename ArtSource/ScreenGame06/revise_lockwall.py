# -*- coding: utf-8 -*-
"""暗い背景でも外形と厚みが読めるよう、LockWall だけの面構成を更新する。"""
import hashlib
import json
from pathlib import Path

ROOT = Path('D:/PJ/TD2_TV')
ART = ROOT/'ArtSource/ScreenGame06'
MODELS = ROOT/'Assets/TD2/Screen3D/Models'
frozen = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in MODELS.glob('*.fbx')}

# 既存の部品生成ヘルパーだけを読み込む。一括生成部分は実行しない。
helper = (ART/'build_props.py').read_text(encoding='utf-8').split("\nbegin('terrain_solid')", 1)[0]
exec(compile(helper, str(ART/'build_props.py'), 'exec'), globals())
old_bounds = entries['lockwall']['bounds']
begin('lockwall')
outline = [(-.40,0),(.40,0),(.5,.10),(.5,.90),(.40,1),(-.40,1),(-.5,.90),(-.5,.10)]
inner = [(x*.91, .5+(z-.5)*.91) for x,z in outline]
vertices = [(x,y,z) for shape,y in ((outline,.22),(outline,-.17),(inner,-.22)) for x,z in shape]
faces, colors = [tuple(reversed(range(8))), tuple(range(16,24))], [0,0]
for i in range(8):
    j = (i+1) % 8
    faces.append((i,j,j+8,i+8))
    # 上面と上隅は象牙色を側面へ回し、正面だけの輪郭線に見せない。
    colors.append(1 if i in (3,4,5) else 0)
    faces.append((i+8,j+8,j+16,i+16))
    colors.append(1)
body = mesh('OctagonalBlock', vertices, faces, 'ink')
body.data.materials.append(MATS['ivory'])
for polygon, color in zip(body.data.polygons, colors):
    polygon.material_index = color
    polygon.use_smooth = False
# 汎用ヘルパーの加重法線を置き換え、広い正面に三角形単位の陰影を出さない。
body.data.normals_split_custom_set([polygon.normal for polygon in body.data.polygons for _ in polygon.loop_indices])

# 承認済みの錠前記号と奥行きの端点は維持し、衝突や配置寸法を変えない。
box('LockBody', (.38,.05,.28), (0,-.20,.43), 'ivory', .015)
box('LockRear', (.38,.05,.28), (0,.20,.43), 'ink', .015)
for x in (-.11,.11):
    box('LockLoop', (.055,.046,.19), (x,-.20,.655), 'ivory', .012)
box('LockLoopTop', (.275,.046,.055), (0,-.20,.7375), 'ivory', .012)
cylinder('Keyhole', .034, .004, (0,-.227,.46), 'ink', vertices=16)
box('KeyholeStem', (.029,.004,.085), (0,-.227,.419), 'ink', .001)
group('Body')
end()
entry = entries['lockwall']
for key in ('min','max'):
    assert all(abs(a-b)<1e-6 for a,b in zip(old_bounds[key],entry['bounds'][key]))
source = ART/'ScreenGame06_LockWall.blend'
bpy.ops.wm.save_as_mainfile(filepath=str(source))


def child(element,name):
    return next(e for e in element.elems if e.id==name)


# 最終 FBX の読み戻しで、幾何の境界・二色・法線・UV を確認する。
doc,_ = parse_fbx.parse(entry['file'])
fbx_objects = child(doc,b'Objects').elems
names = [e.props[1].split(b'\0')[0].decode() for e in fbx_objects if e.id==b'Material']
assert set(names)=={'SG06_Ink','SG06_Ivory'}
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=entry['file'],use_anim=False)
objects = [o for o in bpy.context.scene.objects if o.type=='MESH']
measured = bounds(objects)
error = max(abs(a-b) for key in ('min','max') for a,b in zip(old_bounds[key],measured[key]))
assert error<1e-6
for obj in objects:
    obj.data.calc_loop_triangles()
    assert all(t.area>1e-12 for t in obj.data.loop_triangles)
    assert obj.data.uv_layers.active
    assert all(math.isfinite(v) for vertex in obj.data.vertices for v in vertex.normal)
    assert all(abs(v-1)<1e-5 for v in obj.scale)
for filename,checksum in frozen.items():
    if filename!='SG06_LockWall.fbx':
        assert hashlib.sha256((MODELS/filename).read_bytes()).hexdigest()==checksum,filename

entry.update({'status':'ready','revision':'ivory_chamfer_01','source':str(source),
              'frontChamferWidth':.045,'frontChamferDepth':.05,
              'topFaces':'Ivory continues from bevel across the upper side faces',
              'boundsUnchanged':True,'fbxReadbackPassed':True,
              'fbxReadbackMaxErrorMeters':error,'fbxReadbackBounds':measured,
              'materialIndices':{name:i for i,name in enumerate(names)},
              'sha256':hashlib.sha256(Path(entry['file']).read_bytes()).hexdigest(),
              'engineImportVerified':False,'runtimeVisualVerified':False})
# 他担当が追記した検証状態を保ち、今回の一部品だけを差し替える。
latest = json.loads(CONTRACT.read_text(encoding='utf-8'))
index = next(i for i,e in enumerate(latest['models']) if e['id']=='lockwall')
latest['models'][index] = entry
latest['engineImportVerified'] = False
pending = latest.setdefault('pendingEngineReimport',[])
if 'SG06_LockWall.fbx' not in pending:
    pending.append('SG06_LockWall.fbx')
latest['exportsFrozen'] = True
latest['sourceLockWall'] = str(source)
outdated = latest.setdefault('previews',{}).setdefault('outdatedFor',[])
if 'SG06_LockWall.fbx' not in outdated:
    outdated.append('SG06_LockWall.fbx')
for path in (CONTRACT,ART/'manifest.json'):
    path.write_text(json.dumps(latest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('LOCKWALL_READY',json.dumps({'file':entry['file'],'bounds':entry['bounds'],
       'triangles':entry['triangles'],'sha256':entry['sha256'],'maxReadbackError':error,
       'otherFbxUnchanged':len(frozen)-1}),flush=True)
