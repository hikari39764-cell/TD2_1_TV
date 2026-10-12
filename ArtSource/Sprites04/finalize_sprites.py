"""目視済み素材の寸法・参照・アルファとハッシュを確定する。インポート成功とは区別する。"""
import hashlib
import json
from pathlib import Path
from PIL import Image

ROOT=Path('D:/PJ/TD2_TV')
ASSET=ROOT/'Assets/TD2/Sprites04'
REVIEW=ROOT/'Library/IntegrationStage04/Art'
path=ASSET/'td2_sprites04.manifest.json'
m=json.loads(path.read_text(encoding='utf-8'))
keys=set();rects=[]
for f in m['frames']:
    assert f['key'] not in keys
    keys.add(f['key'])
    image_path=ROOT/f['path']
    assert image_path.resolve().is_relative_to(ASSET.resolve())
    with Image.open(image_path) as im:
        assert im.mode=='RGBA' and list(im.size)==f['sizePx']==[64,64]
        assert im.getchannel('A').getbbox()
        f['sha256']=hashlib.sha256(image_path.read_bytes()).hexdigest()
    x,y,w,h=f['rectPx']
    assert x>=0 and y>=0 and x+w<=1024 and y+h<=1024
    assert all(x+w<=a or a+c<=x or y+h<=b or b+d<=y for a,b,c,d in rects)
    rects.append(f['rectPx'])
for a in m['animations'].values():assert all(key in keys for key in a['frames'])
effects=json.loads((ASSET/'td2_effects04.json').read_text(encoding='utf-8'))
for effect in effects['effects']:
    a=m['animations'][effect['animation']]
    assert len(a['frames'])*a['frameDurationMs']==effect['durationMs']

m['status']='ready_for_import'
m['visualReview']={'reviewed':True,'nativeFramePixels':64,'testedDisplayPixelsPerCell':[16,24,32],
                   'screenCompositionPreviewSizes':[[1024,576],[512,288],[384,216]],
                   'ingame3dVerified':False}
m['renderTint']=[1,1,1,1]
path.write_text(json.dumps(m,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
report={
 'status':'ready_for_root_import_and_presenter_integration','frameCount':len(keys),
 'manifest':'Assets/TD2/Sprites04/td2_sprites04.manifest.json',
 'effects':'Assets/TD2/Sprites04/td2_effects04.json',
 'independentPngDirectory':'Assets/TD2/Sprites04/frames',
 'svgSources':'ArtSource/Sprites04/svg',
 'previewFiles':['contact-sheet.png','readability-actual-size.png','screen-composition-512.png','screen-composition-384.png'],
 'checks':{'uniqueFrames':True,'pngRgba64x64':True,'pathsExist':True,'atlasRectsNonOverlapping':True,
           'animationReferencesAndDurations':True,'visualReview':True,'editorImportedByThisRole':False,'runtimeVerifiedByThisRole':False},
 'integrationNotes':[
    'Use frame.path independent PNG; the public UI sprite API has no atlas source rect.',
    'Sprite images already contain color. Set tint to white; do not multiply the previous DeviceColor palette into these images.',
    'Keep player/echo at the native 64px quad and apply collision offsets from the manifest.',
    'Honor drawPolicy for plate/platform: keep one tile of visual height, anchored to bottom/top; thin sensor bounds are not the sprite height.',
    'Current eight DeviceKinds have bindings; spring/lift/collectible/core/key/portal/beacon art does not imply implemented rules.',
    'Screen emission/unlit display should preserve source colors. No global GI/exposure edits requested.',
 ],
 'limits':['16px cells retain primary silhouettes but little internal detail; 24px or more is preferable for a focused playable TV.',
           'These are raster composition/scale previews, not screenshots proving engine rendering or rule behavior.'],
}
(REVIEW/'delivery.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'frames':len(keys),'status':m['status'],'runtimeVerified':False}))
