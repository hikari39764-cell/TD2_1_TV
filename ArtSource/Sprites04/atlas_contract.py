"""描画側が先に接続できる固定アトラス契約。並び順と矩形は生成後も変更しない。"""
import json
from pathlib import Path

ROOT = Path('D:/PJ/TD2_TV')
ASSET = ROOT/'Assets/TD2/Sprites04'
REVIEW = ROOT/'Library/IntegrationStage04/Art'
frames, animations = [], {}

def add(key, role, duration=0, anchor=(32,32), collision=None):
    index = len(frames)
    kind=key.split('.')[0]
    draw_policy=('native_body_anchor' if kind in ('player','echo') else
                 'tile_height_bottom_anchor' if kind in ('plate','spring','checkpoint') else
                 'tile_height_top_anchor' if kind in ('platform','lift') else
                 'effect_anchor' if kind=='fx' else 'stretch_to_authored_bounds')
    frames.append({'key':key,'image':'frames/'+key.replace('.','_')+'.png',
                   'path':'Assets/TD2/Sprites04/frames/'+key.replace('.','_')+'.png','sizePx':[64,64],
                   'rectPx':[(index%12)*80+8,(index//12)*80+8,64,64],
                   'anchorPx':list(anchor),'durationMs':duration,'role':role,
                   'drawPolicy':draw_policy,'collisionReferencePx':collision})
    return key

def sequence(prefix, count, role, duration, loop=False, anchor=(32,32), collision=None):
    keys = [add(f'{prefix}.{i}',role,duration,anchor,collision) for i in range(count)]
    animations[prefix] = {'frames':keys,'frameDurationMs':duration,'loop':loop}

for facing in ('right','left'):
    for state,count,duration,loop in [('idle',2,500,True),('walk',4,110,True),
                                       ('air',1,0,False),('dash',1,140,False),('land',1,90,False)]:
        sequence(f'player.{state}.{facing}',count,'player',duration,loop,(32,64),[12,8,40,56])
    add(f'echo.idle.{facing}','echo',0,(32,64),[12,0,40,64])
    sequence(f'echo.form.{facing}',3,'echo',55,False,(32,64),[12,0,40,64])

for part in ('fill','top','bottom','left','right','top_left','top_right','bottom_left','bottom_right'):
    add('terrain.'+part,'terrain')
for part in ('left','middle','right'):
    add('platform.'+part,'platform',(0),(32,0))
for direction in ('up','right','down','left'):
    add('spikes.'+direction,'hazard')
for state in ('up','down'):
    add('plate.'+state,'device',0,(32,64))
for state in ('closed','open'):
    add('door.'+state,'device')
for state in ('idle','charge0','charge1','released'):
    add('spring.'+state,'device',0,(32,64))
for state in ('idle','active','pulse'):
    add('checkpoint.'+state,'device',0,(32,64))
for direction in ('right','down','left','up'):
    for state in ('off','on'):
        add(f'laser.{direction}.{state}','hazard')
for direction in ('horizontal','vertical'):
    add('laser.beam.'+direction,'hazard')
for state in ('idle','active'):
    add('lift.'+state,'device',(0),(32,0))
for state in ('idle','active'):
    add('hazard_mover.'+state,'hazard')
for kind in ('collectible','core','key','portal','beacon'):
    sequence(kind,2,'device',350,True)
for state in ('closed','open'):
    add('lock.'+state,'device')
add('spawn.marker','editor')
for name,count,duration,anchor in [('dash',4,35,(32,32)),('land',4,45,(32,64)),
                                    ('plate',3,40,(32,64)),('door',3,45,(32,32)),
                                    ('checkpoint',4,75,(32,32)),('death',4,55,(32,32))]:
    sequence('fx.'+name,count,'effect',duration,False,anchor)

manifest = {
 'schemaVersion':1,'status':'layout_frozen_art_pending',
 'image':'td2_sprites04.png','atlasSizePx':[1024,1024],
 'frameSizePx':[64,64],'packing':{'columns':12,'stridePx':80,'insetPx':8,'extrusionPx':2},
 'coordinateOrigin':'top-left','alphaMode':'straight','colorSpace':'sRGB',
 'recommendedSampler':{'filter':'linear','mipmaps':False,'wrap':'clamp'},
 'runtimeTexturePolicy':'Use each frame.image relative to this manifest directory. The public sprite API has no atlas UV rectangle; atlas rects are authoring/preview metadata only.',
 'logicalTilePixels':64,
 'placement':{
   'player':'Draw a 64x64 logical-pixel quad at (body.x*tile-12, body.y*tile-8) when tile=64; scale all offsets by tile/64. Collision reference is 40x56, do not stretch artwork into the body rectangle.',
   'echo':'Draw a 64x64 quad at (body.x*tile-12, body.y*tile); collision reference is 40x64. Same foot anchor as player.',
   'devices':'Default stretch_to_authored_bounds. For tile_height_bottom_anchor (plate/spring/checkpoint), draw one tile HIGH with bottom=device.y+device.h; width=device.w*tile. For tile_height_top_anchor (platform/lift), draw one tile HIGH with top=device.y; width=device.w*tile. Do not shrink an entire plate PNG into its thin sensor height. Artwork never changes collision geometry.',
   'terrain':'Select exposed-edge/corner frames. Repeat at one tile per frame; use fill for interior cells, never stretch a single bordered tile over the whole room.',
   'direction':'All left/right/up/down variants are baked; no UV mirroring needed.',
 },
 'frames':frames,'animations':animations,
 'currentRuntimeBindings':{
   'Solid':'terrain.fill','OneWay':'platform.middle','Plate':{'false':'plate.up','true':'plate.down'},
   'Door':{'false':'door.closed','true':'door.open'},
   'Laser':'laser.{direction}.{off|on}','Spikes':'spikes.{direction}',
   'Checkpoint':{'false':'checkpoint.idle','true':'checkpoint.active'},
   'LockWall':{'false':'lock.closed','true':'lock.open'}
 },
 'assetOnlyUntilRulesSupport':['spring','lift','hazard_mover','collectible','core','key','portal','beacon'],
 'readability':'Warm-white player; cyan echo; pale-jade terrain rims; amber state accents; muted coral danger. No bloom baked in, no text baked in.',
 'importedIntoEditor':False
}

if __name__ == '__main__':
    ASSET.mkdir(parents=True,exist_ok=True)
    REVIEW.mkdir(parents=True,exist_ok=True)
    (ASSET/'td2_sprites04.manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'frames':len(frames),'manifest':str(ASSET/'td2_sprites04.manifest.json'),'atlas':[1024,1024]}))
