"""少数のベクトル形状から独立PNG・SVG・アトラスを生成し、縮小時の可読性を確認する。"""
import copy
import html
import json
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageOps
from atlas_contract import ROOT, ASSET, REVIEW, frames, manifest

SOURCE = ROOT/'ArtSource/Sprites04'
S = 4
# 実機の縮小・CRT処理でも面が消えない中明度へ。暖白の人物と輪郭の暗色は維持する。
P = {'ink':'#18343C','dark':'#557E82','body':'#91BDB5','jade':'#B6E4D1',
     'pale':'#DCEEDF','white':'#F6EDD4','amber':'#EFC46F','danger':'#DF8272',
     'echo':'#8CE0D4','bg':'#29454D','muted':'#A0CDBF'}

def rgba(color, alpha=255):
    if isinstance(color,tuple): return color
    color=P.get(color,color).lstrip('#')
    return tuple(int(color[i:i+2],16) for i in (0,2,4))+(alpha,)

class Canvas:
    def __init__(self):
        self.im=Image.new('RGBA',(64*S,64*S),(0,0,0,0))
        self.d=ImageDraw.Draw(self.im)
        self.svg=[]
    def style(self,fill,stroke=None,width=2,alpha=255):
        color=P.get(fill,fill) if fill else 'none'
        return f'fill="{color}" opacity="{alpha/255:.4f}"'+(f' stroke="{P.get(stroke,stroke)}" stroke-width="{width}" stroke-linejoin="round"' if stroke else '')
    def rect(self,box,fill,stroke=None,width=2,radius=0,alpha=255):
        b=tuple(round(v*S) for v in box)
        args={'fill':rgba(fill,alpha) if fill else None,'outline':rgba(stroke,alpha) if stroke else None,'width':max(1,round(width*S))}
        if radius:self.d.rounded_rectangle(b,radius=radius*S,**args)
        else:self.d.rectangle(b,**args)
        x,y,x2,y2=box
        self.svg.append(f'<rect x="{x}" y="{y}" width="{x2-x}" height="{y2-y}" rx="{radius}" {self.style(fill,stroke,width,alpha)}/>')
    def poly(self,points,fill,stroke=None,width=2,alpha=255):
        q=[(round(x*S),round(y*S)) for x,y in points]
        self.d.polygon(q,fill=rgba(fill,alpha) if fill else None)
        if stroke:self.d.line(q+[q[0]],fill=rgba(stroke,alpha),width=round(width*S),joint='curve')
        self.svg.append(f'<polygon points="{" ".join(f"{x},{y}" for x,y in points)}" {self.style(fill,stroke,width,alpha)}/>')
    def ellipse(self,box,fill,stroke=None,width=2,alpha=255):
        b=tuple(round(v*S) for v in box)
        self.d.ellipse(b,fill=rgba(fill,alpha) if fill else None,outline=rgba(stroke,alpha) if stroke else None,width=max(1,round(width*S)))
        x,y,x2,y2=box
        self.svg.append(f'<ellipse cx="{(x+x2)/2}" cy="{(y+y2)/2}" rx="{(x2-x)/2}" ry="{(y2-y)/2}" {self.style(fill,stroke,width,alpha)}/>')
    def line(self,points,color,width=2,alpha=255):
        self.d.line([(round(x*S),round(y*S)) for x,y in points],fill=rgba(color,alpha),width=round(width*S),joint='curve')
        self.svg.append(f'<polyline points="{" ".join(f"{x},{y}" for x,y in points)}" fill="none" stroke="{P.get(color,color)}" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round" opacity="{alpha/255:.4f}"/>')
    def transform(self,mirror=False,rotation=0):
        if mirror:
            self.im=ImageOps.mirror(self.im)
            self.svg=['<g transform="translate(64 0) scale(-1 1)">'+''.join(self.svg)+'</g>']
        if rotation:
            self.im=self.im.rotate(-rotation,resample=Image.Resampling.NEAREST)
            self.svg=[f'<g transform="rotate({rotation} 32 32)">'+''.join(self.svg)+'</g>']
        self.d=ImageDraw.Draw(self.im)
    def finish(self):return self.im.resize((64,64),Image.Resampling.LANCZOS)
    def vector(self):return '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">'+''.join(self.svg)+'</svg>\n'

def character(c,state,frame,echo=False):
    # 暖白の頭、作業着、別れた両脚、前へ出たバイザーで小表示でも人と向きを読む。
    bob=1 if state=='idle' and frame%2 else 0
    compress=5 if state=='land' else 0
    lean=5 if state=='dash' else 0
    top=8+bob+compress+(5 if state=='dash' else 0)
    skin='echo' if echo else 'white'
    jacket='echo' if echo else 'jade'
    edge='echo' if echo else 'ink'
    fill_alpha=210 if echo else 255
    # 残影は実体と同じ輪郭。頭の上端だけを上げて一格高の判定を見失わせない。
    if echo:top=0
    c.rect([13+lean,top+22,23+lean,50],'dark',edge,2,3,fill_alpha)
    c.poly([(24+lean,top+21),(42+lean,top+21),(47,49),(40,54),(22,53),(21,36)],jacket,edge,2,fill_alpha)
    c.rect([23+lean,top,43+lean,top+22],skin,edge,2,5,fill_alpha)
    c.poly([(22+lean,top+2),(38+lean,top),(45+lean,top+5),(45+lean,top+9),(22+lean,top+9)],'body',edge,1,fill_alpha)
    c.rect([34+lean,top+10,48+lean,top+15],'dark',None,1,2,fill_alpha)
    c.rect([42+lean,top+11,48+lean,top+14],skin,None,1,0,fill_alpha)
    c.line([(24+lean,top+23),(32+lean,top+26),(41+lean,top+23)],'dark',3,fill_alpha)
    c.rect([23,45,45,49],'dark',None,1,0,fill_alpha)
    c.rect([31,45,36,49],'amber' if not echo else 'pale',None,1,0,fill_alpha)
    stride=[0,5,0,-5][frame%4] if state=='walk' else 0
    if state=='dash':stride=7
    if state=='air':stride=-3
    c.poly([(24,49),(33,49),(30-stride,61),(18-stride/2,61),(20-stride/2,57)],'body',edge,2,fill_alpha)
    c.poly([(35,49),(43,49),(46+stride/2,59),(50+stride/2,61),(34+stride/2,61)],'body',edge,2,fill_alpha)
    c.rect([17-stride/2,59,30-stride/2,64],skin,edge,1,2,fill_alpha)
    c.rect([34+stride/2,59,51+stride/2,64],skin,edge,1,2,fill_alpha)
    c.line([(43+lean,top+26),(48,43),(45,47)],skin,4,fill_alpha)
    if echo:
        c.line([(20,31),(45,31)],'pale',2,190)
        c.line([(22,38),(45,38)],'echo',2,200)

def terrain(c,part):
    c.rect([0,0,64,64],'body')
    c.rect([3,5,61,61],'dark')
    c.rect([6,7,58,58],'body')
    c.line([(14,19),(14,13),(29,13)],'muted',2)
    c.line([(49,45),(49,51),(37,51)],'dark',3)
    edges={'top_left':['top','left'],'top_right':['top','right'],
           'bottom_left':['bottom','left'],'bottom_right':['bottom','right']}.get(part,[part])
    for edge in edges:
        if edge=='top':c.rect([0,0,64,7],'jade');c.line([(0,8),(64,8)],'ink',2)
        if edge=='bottom':c.rect([0,57,64,64],'muted');c.line([(0,56),(64,56)],'ink',2)
        if edge=='left':c.rect([0,0,6,64],'jade');c.line([(7,0),(7,64)],'ink',2)
        if edge=='right':c.rect([58,0,64,64],'muted');c.line([(57,0),(57,64)],'ink',2)

def platform(c,part,active=False):
    c.rect([0,0,64,9],'pale' if active else 'jade','ink',1)
    c.rect([0,9,64,15],'body')
    c.poly([(9,15),(16,15),(23,26),(15,26)],'dark')
    c.poly([(42,15),(49,15),(57,26),(49,26)],'dark')
    if part=='left':c.rect([0,0,6,22],'amber')
    if part=='right':c.rect([58,0,64,22],'amber')

def spikes(c,direction):
    c.rect([0,55,64,64],'dark','ink',2)
    for x in (1,22,43):
        c.poly([(x,56),(x+10,2),(x+20,56)],'danger','ink',2)
        c.poly([(x+10,5),(x+10,48),(x+17,53)],'white',None,1,210)
    c.transform(rotation={'up':0,'right':90,'down':180,'left':270}[direction])

def plate(c,down):
    c.rect([3,55,61,64],'dark','ink',2,2)
    top=53 if down else 43
    if not down:
        c.rect([10,top+8,18,57],'body');c.rect([46,top+8,54,57],'body')
    c.poly([(6,top),(58,top),(62,top+8),(2,top+8)],'amber' if down else 'pale','ink',2)
    c.rect([26,top+2,38,top+5],'white' if down else 'jade')

def door(c,opened,locked=False):
    c.rect([3,0,61,64],'dark','ink',2)
    c.rect([11,5,53,64],'ink')
    if not opened:
        c.rect([13,8,51,64],'body')
        for x in (20,32,44):c.line([(x,12),(x,60)],'jade',3)
        c.rect([10,31,54,38],'amber')
    else:
        # 中央は透明に戻し、開いた扉を通行不能な塗り板に見せない。
        c.d.rectangle((12*S,12*S,52*S,64*S),fill=(0,0,0,0))
        c.svg=[]
        c.rect([3,0,11,64],'body','ink',2);c.rect([53,0,61,64],'body','ink',2)
        c.rect([3,0,61,11],'jade','ink',2)
        c.rect([14,4,50,8],'amber')
    if locked and not opened:
        c.ellipse([24,21,40,38],None,'white',3)
        c.rect([21,30,43,47],'amber','ink',2,3)
        c.ellipse([29,34,35,40],'ink');c.rect([31,38,33,43],'ink')

def spring(c,state):
    top={'idle':28,'charge0':36,'charge1':45,'released':9}[state]
    c.rect([7,56,57,64],'dark','ink',2)
    n=3;step=(54-(top+8))/n
    points=[(18,54)]
    for i in range(n):points.extend([(46,54-step*(i+.5)),(18,54-step*(i+1))])
    c.line(points,'amber',4)
    c.rect([5,top,59,top+9],'white' if state=='released' else 'jade','ink',2,2)
    if state=='charge1':c.rect([25,top+2,39,top+6],'amber')

def checkpoint(c,state):
    active=state!='idle'
    c.poly([(9,59),(15,52),(49,52),(55,59),(55,64),(9,64)],'body','ink',2)
    c.rect([29,23,35,55],'jade','ink',2)
    c.poly([(32,4),(49,21),(32,38),(15,21)],'amber' if active else 'jade','ink',3)
    c.poly([(32,12),(41,21),(32,30),(23,21)],'white' if active else 'dark')
    if state=='pulse':c.line([(10,22),(4,22)],'pale',3);c.line([(54,22),(60,22)],'pale',3)

def laser(c,direction,on):
    c.rect([6,9,37,55],'body','ink',3,4)
    c.rect([2,4,10,60],'jade','ink',2)
    c.poly([(35,17),(52,23),(57,32),(52,41),(35,47)],'dark','ink',2)
    c.rect([49,23,58,41],'danger' if on else 'muted','ink',2,2)
    c.rect([54,27,61,37],'white' if on else 'ink')
    c.ellipse([16,23,29,36],'amber' if on else 'dark','ink',2)
    c.transform(rotation={'right':0,'down':90,'left':180,'up':270}[direction])

def hazard_mover(c,active):
    for i in range(4):
        angle=math.radians(i*90)
        def p(x,y):return (32+x*math.cos(angle)-y*math.sin(angle),32+x*math.sin(angle)+y*math.cos(angle))
        c.poly([p(-10,-20),p(0,-31),p(10,-20)],'danger','ink',2)
    c.rect([12,12,52,52],'dark','ink',3,4)
    c.rect([21,21,43,43],'danger' if active else 'body','jade',2,2)
    c.line([(25,32),(39,32)],'white',3)

def icon(c,kind,phase):
    accent='white' if phase else 'amber'
    if kind=='collectible':
        c.poly([(32,12),(49,32),(32,52),(15,32)],accent,'ink',3)
        c.poly([(32,18),(32,42),(22,32)],'amber' if phase else 'white')
    elif kind=='core':
        c.poly([(22,7),(42,7),(57,22),(57,42),(42,57),(22,57),(7,42),(7,22)],'dark','jade',3)
        c.line([(17,25),(43,25),(38,19)],accent,4)
        c.line([(47,39),(21,39),(26,45)],accent,4)
    elif kind=='key':
        c.ellipse([7,15,33,41],'amber','ink',3)
        c.ellipse([14,22,25,33],'dark')
        c.rect([29,25,57,32],accent,'ink',2)
        c.rect([45,30,51,41],'amber','ink',2)
        c.rect([53,30,59,37],'amber','ink',2)
    elif kind=='portal':
        c.ellipse([8,4,56,60],None,'dark',7)
        c.ellipse([12,7,52,57],None,'jade',4)
        c.ellipse([20,15,44,49],None,accent,3)
        c.rect([7,29,18,35],'amber');c.rect([46,29,57,35],'amber')
    elif kind=='beacon':
        c.rect([17,55,47,64],'body','ink',2)
        c.line([(32,55),(32,30)],'jade',5)
        c.poly([(32,3),(49,21),(32,40),(15,21)],accent,'ink',3)
        c.poly([(32,12),(40,21),(32,30),(24,21)],'jade')

def effect(c,kind,index):
    t=index/3;alpha=max(35,int(230*(1-t*.8)))
    if kind=='dash':
        for j,(y,length) in enumerate([(19,40),(31,54),(43,35)]):
            start=4+index*4
            c.poly([(start,y),(min(63,start+length),y-3),(min(63,start+length-5),y+3),(start+7,y+2)],'echo',None,1,alpha-j*20)
    elif kind=='land':
        for j in range(5):
            x=32+(j-2)*(5+index*3);y=60-index*(3 if j%2 else 2)
            r=max(1,4-index*.7)
            c.rect([x-r,y-r,x+r,y+r],'pale',None,1,1,alpha)
    elif kind=='plate':
        for side in (-1,1):
            x=32+side*(18+index*4)
            c.line([(x,50-index*3),(x+side*5,45-index*3)],'amber',3,alpha)
    elif kind=='door':
        for side in (-1,1):
            x=32+side*(9+index*7)
            c.line([(x,14),(x,50)],'amber',3,alpha)
            c.poly([(x,28),(x+side*6,32),(x,36)],'white',None,1,alpha)
    elif kind=='checkpoint':
        r=11+index*6
        c.poly([(32,32-r),(32+r,32),(32,32+r),(32-r,32)],None,'jade',3,alpha)
    elif kind=='death':
        for j in range(7):
            a=j*math.tau/7
            x=32+math.cos(a)*(6+index*7);y=32+math.sin(a)*(6+index*6)+index*index
            r=4-index*.5
            c.poly([(x-r,y),(x,y-r),(x+r,y),(x,y+r)],'white' if j%3 else 'amber',None,1,alpha)

def paint(key):
    c=Canvas();p=key.split('.')
    if p[0]=='player':
        character(c,p[1],int(p[3]));c.transform(mirror=p[2]=='left')
    elif p[0]=='echo':
        frame=int(p[3]) if p[1]=='form' else 0
        character(c,'idle',0,True)
        if p[1]=='form':
            # 形成前の見た目だけ。判定の出現タイミングはルール側に従う。
            alpha=c.im.getchannel('A').point(lambda a:round(a*(.35+.30*frame)))
            c.im.putalpha(alpha)
            c.svg=[f'<g opacity="{.35+.30*frame}">'+''.join(c.svg)+'</g>']
        c.transform(mirror=p[2]=='left')
    elif p[0]=='terrain':terrain(c,p[1])
    elif p[0]=='platform':platform(c,p[1])
    elif p[0]=='spikes':spikes(c,p[1])
    elif p[0]=='plate':plate(c,p[1]=='down')
    elif p[0]=='door':door(c,p[1]=='open')
    elif p[0]=='spring':spring(c,p[1])
    elif p[0]=='checkpoint':checkpoint(c,p[1])
    elif p[0]=='laser':
        if p[1]=='beam':
            c.rect([0,0,64,64],'danger',None,1,0,90)
            c.rect([0,19,64,45],'danger',None,1,0,210)
            c.rect([0,28,64,36],'white',None,1,0,240)
            c.transform(rotation=90 if p[2]=='vertical' else 0)
        else:laser(c,p[1],p[2]=='on')
    elif p[0]=='lift':
        platform(c,'middle',p[1]=='active')
        c.line([(32,21),(32,51)],'amber',4)
        c.poly([(22,30),(32,18),(42,30)],'amber')
        c.poly([(22,42),(32,54),(42,42)],'amber')
    elif p[0]=='hazard_mover':hazard_mover(c,p[1]=='active')
    elif p[0] in ('collectible','core','key','portal','beacon'):icon(c,p[0],int(p[1]))
    elif p[0]=='lock':door(c,p[1]=='open',True)
    elif p[0]=='spawn':
        c.ellipse([10,10,54,54],None,'jade',3)
        c.line([(32,19),(32,45)],'white',3);c.line([(19,32),(45,32)],'white',3)
    elif p[0]=='fx':effect(c,p[1],int(p[2]))
    else:raise ValueError(key)
    return c

def font(size,bold=False):
    filename='C:/Windows/Fonts/seguisb.ttf' if bold else 'C:/Windows/Fonts/segoeui.ttf'
    return ImageFont.truetype(filename,size)

def generate():
    ASSET.mkdir(parents=True,exist_ok=True);(ASSET/'frames').mkdir(exist_ok=True)
    (SOURCE/'svg').mkdir(parents=True,exist_ok=True);REVIEW.mkdir(parents=True,exist_ok=True)
    atlas=Image.new('RGBA',(1024,1024),(0,0,0,0));images={};report=[]
    for f in frames:
        c=paint(f['key']);im=c.finish();images[f['key']]=im
        path=ASSET/f['image'];im.save(path)
        (SOURCE/'svg'/(f['key'].replace('.','_')+'.svg')).write_text(c.vector(),encoding='utf-8')
        x,y,w,h=f['rectPx'];atlas.paste(im,(x,y))
        # 不透明な地形の縁は2px押し出し、隣接フレームの色混入を防ぐ。
        atlas.paste(im.crop((0,0,1,64)).resize((2,64)),(x-2,y))
        atlas.paste(im.crop((63,0,64,64)).resize((2,64)),(x+64,y))
        atlas.paste(im.crop((0,0,64,1)).resize((64,2)),(x,y-2))
        atlas.paste(im.crop((0,63,64,64)).resize((64,2)),(x,y+64))
        bounds=im.getchannel('A').getbbox()
        assert bounds is not None and im.mode=='RGBA' and im.size==(64,64)
        f['artBoundsPx']=list(bounds)
        f['projectPath']='Assets/TD2/Sprites04/'+f['image']
        report.append({'key':f['key'],'size':[64,64],'visibleBounds':list(bounds)})
    atlas.save(ASSET/'td2_sprites04.png')
    final=copy.deepcopy(manifest);final.update(status='generated_pending_visual_review',frames=frames)
    final['paletteSrgb']=P
    (ASSET/'td2_sprites04.manifest.json').write_text(json.dumps(final,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

    # ラベル付き一覧は納品アセットとは分離し、背景を付けて透過輪郭を検査する。
    cols=8;cw=166;ch=130;rows=math.ceil(len(frames)/cols)
    sheet=Image.new('RGB',(cols*cw,rows*ch+82),P['bg']);d=ImageDraw.Draw(sheet)
    d.text((22,15),'TD2 / SPRITES 04',font=font(25,True),fill=P['white'])
    d.text((22,49),'64 px | warm white / pale jade / amber / coral | independent PNG + SVG',font=font(16),fill=P['jade'])
    for i,f in enumerate(frames):
        x=(i%cols)*cw;y=(i//cols)*ch+82
        d.rounded_rectangle((x+5,y+3,x+cw-5,y+ch-4),radius=6,fill='#203941')
        sheet.paste(images[f['key']],(x+(cw-64)//2,y+10),images[f['key']])
        words=f['key'].rsplit('.',1)
        d.text((x+10,y+85),words[0],font=font(12),fill=P['pale'])
        d.text((x+10,y+103),words[-1],font=font(12),fill=P['muted'])
    sheet.save(REVIEW/'contact-sheet.png')

    # 実際のテレビ撮影では1格16～32pxまで縮む。整数倍拡大だけで誤魔化さない。
    selected=['player.idle.right.0','player.walk.left.1','echo.idle.right','terrain.top_left',
              'spikes.up','plate.up','plate.down','door.closed','door.open','checkpoint.active',
              'laser.right.on','lock.closed','spring.idle','core.0','key.0','portal.0']
    small=Image.new('RGB',(1080,610),P['bg']);sd=ImageDraw.Draw(small)
    sd.text((22,16),'ACTUAL DISPLAY SIZE / 16 - 24 - 32 px per cell',font=font(24,True),fill=P['white'])
    for row,size in enumerate((16,24,32)):
        y=85+row*150
        sd.text((22,y),str(size)+' px',font=font(18,True),fill=P['amber'])
        for i,key in enumerate(selected):
            x=100+i*59;im=images[key].resize((size,size),Image.Resampling.LANCZOS)
            small.paste(im,(x,y+18),im)
            sd.text((x,y+size+28),str(i+1),font=font(12),fill=P['muted'])
    sd.text((22,557),'1 Player >  2 Player <  3 Echo  4 Solid  5 Spike  6/7 Plate  8/9 Door  10 Save  11 Laser  12 Lock',font=font(14),fill=P['pale'])
    small.save(REVIEW/'readability-actual-size.png')

    # 16×9格の実寸画面も付ける。これはルールを動かした証拠ではなく絵の視認テスト。
    room=Image.new('RGBA',(1024,576),rgba('bg'))
    def stamp(key,x,y):room.alpha_composite(images[key],(x*64,y*64))
    for x in range(16):stamp('terrain.top',x,8);stamp('terrain.bottom',x,0)
    for y in range(1,8):stamp('terrain.right',0,y);stamp('terrain.left',15,y)
    for x in range(5,9):stamp('platform.middle',x,5)
    stamp('player.idle.right.0',2,7);stamp('echo.idle.left',4,7)
    stamp('plate.up',6,7)
    room.alpha_composite(images['door.closed'].resize((64,128),Image.Resampling.LANCZOS),(10*64,6*64))
    stamp('checkpoint.active',13,7);stamp('spikes.up',12,3);stamp('terrain.top',12,4)
    stamp('laser.left.on',14,2);stamp('core.0',7,4)
    room.save(REVIEW/'screen-composition-1024.png')
    room.resize((512,288),Image.Resampling.LANCZOS).save(REVIEW/'screen-composition-512.png')
    room.resize((384,216),Image.Resampling.LANCZOS).save(REVIEW/'screen-composition-384.png')
    (REVIEW/'generation-check.json').write_text(json.dumps({'frameCount':len(frames),'checks':report,'visualReviewPending':True},indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'frames':len(frames),'atlas':str(ASSET/'td2_sprites04.png'),'preview':str(REVIEW/'contact-sheet.png')}))

if __name__=='__main__':generate()
