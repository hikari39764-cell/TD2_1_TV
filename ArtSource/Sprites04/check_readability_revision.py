"""配色のみの修正でフレーム寸法・透過形状・配置契約が変わらないことを確認する。"""
import hashlib
import json
import shutil
import sys
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT=Path('D:/PJ/TD2_TV')
ASSET=ROOT/'Assets/TD2/Sprites04'
REVIEW=ROOT/'Library/IntegrationStage04/Art'
OUT=REVIEW/'ReadabilityRevision01'
FIELDS=('key','image','path','sizePx','rectPx','anchorPx','durationMs','role','drawPolicy','collisionReferencePx')

def read(path):return json.loads(path.read_text(encoding='utf-8'))

if sys.argv[1]=='prepare':
    # 元の成果物を削除せず、色変更の前後差分に必要な最小資料だけ保存する。
    OUT.mkdir(parents=True,exist_ok=True)
    assert not (OUT/'before-manifest.json').exists(), 'Baseline already saved; do not overwrite it.'
    shutil.copy2(ASSET/'td2_sprites04.manifest.json',OUT/'before-manifest.json')
    shutil.copy2(REVIEW/'screen-composition-384.png',OUT/'before-screen-384.png')
    (OUT/'before-frames').mkdir(exist_ok=True)
    for f in read(OUT/'before-manifest.json')['frames']:
        shutil.copy2(ROOT/f['path'],OUT/'before-frames'/Path(f['path']).name)
    print('Baseline saved')
else:
    old=read(OUT/'before-manifest.json');new=read(ASSET/'td2_sprites04.manifest.json')
    assert len(old['frames'])==len(new['frames'])==102
    changed=[]
    for a,b in zip(old['frames'],new['frames']):
        assert {k:a.get(k) for k in FIELDS}=={k:b.get(k) for k in FIELDS}, b['key']
        with Image.open(OUT/'before-frames'/Path(a['path']).name) as before, Image.open(ROOT/b['path']) as after:
            assert before.size==after.size==(64,64)
            assert ImageChops.difference(before.getchannel('A'),after.getchannel('A')).getbbox() is None,b['key']
            if before.tobytes()!=after.tobytes():changed.append(b['path'])
    # 輝度比較はsRGBを線形へ変換して算出する。実機の露出・CRT効果までは推測しない。
    def linear(hex_value):
        rgb=[int(hex_value.lstrip('#')[i:i+2],16)/255 for i in (0,2,4)]
        return [v/12.92 if v<=.04045 else ((v+.055)/1.055)**2.4 for v in rgb]
    def luminance(hex_value):return sum(v*w for v,w in zip(linear(hex_value),(.2126,.7152,.0722)))
    changes=[]
    for key,value in new['paletteSrgb'].items():
        prior=old['paletteSrgb'][key]
        if value!=prior:changes.append({'key':key,'fromSrgb':prior,'toSrgb':value,
                                       'linearLuminanceMultiplier':round(luminance(value)/luminance(prior),3)})
    assert new['paletteSrgb']['white']==old['paletteSrgb']['white']=='#F6EDD4'
    new['status']='ready_for_reimport'
    new['assetRevision']=2
    new['needsReimport']=True
    new['visualReview']['ingame3dVerified']=False
    new['visualReview']['revisionBasis']='GPU screenshot f139918efdb8bde7f8731cbec9bb6d0a.png and Stage04 world-map.png'
    (ASSET/'td2_sprites04.manifest.json').write_text(json.dumps(new,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    # 前後の小サイズ画面を並べて渡す。背景の提案値は正式Runtimeへ自動適用しない。
    board=Image.new('RGB',(824,284),'#172C34');draw=ImageDraw.Draw(board)
    font=ImageFont.truetype('C:/Windows/Fonts/seguisb.ttf',18)
    draw.text((20,15),'BEFORE',font=font,fill='#C7E4D6')
    draw.text((424,15),'BRIGHTER SPRITES / BG PROPOSAL',font=font,fill='#F6EDD4')
    board.paste(Image.open(OUT/'before-screen-384.png').convert('RGB'),(20,48))
    board.paste(Image.open(REVIEW/'screen-composition-384.png').convert('RGB'),(424,48))
    board.save(OUT/'before-after-384.png')
    report={
        'status':'ready_for_root_offline_reimport','frames':102,'changedPngCount':len(changed),
        'changedPngPaths':changed,'paletteChanges':changes,
        'unchanged':['All 102 frame paths, image dimensions, rects, anchors, durations, draw policies and alpha masks.',
                     'Warm-white player face/feet color #F6EDD4.','Runtime/Source, formal 3D scene and global environment.'],
        'screenshotsActuallyRead':['Library/EditorAI/Captures/f139918efdb8bde7f8731cbec9bb6d0a.png','Library/IntegrationStage04/world-map.png'],
        'backgroundRecommendation':{'scope':'2D region background / screen target only; root applies if desired',
                                    'srgb':'#29454D','linearRgb':[round(v,6) for v in linear('#29454D')],
                                    'note':'The preview uses this background; no runtime background file was modified by art.'},
        'presentationRecommendations':['Keep sprite tint white.','Root owns chromaPixels/phosphorMask reduction; do not add bloom to compensate.',
                                       'Recheck an actual focused A/B TV after reimport; a flat sprite preview does not prove final screen brightness.'],
        'verification':{'allAlphaMasksIdentical':True,'frameContractUnchanged':True,'warmWhiteUnchanged':True,
                        'updatedFlatPreviewsReviewed':'--reviewed' in sys.argv,'updatedGpuVerified':False}
    }
    (OUT/'delivery.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'frames':102,'changedPngs':len(changed),'paletteChanges':changes,'alphaMasksUnchanged':True}))
