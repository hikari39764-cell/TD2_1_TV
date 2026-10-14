# -*- coding: utf-8 -*-
"""FactorySet01 の配置表を主場景 TD2_Rules01 に反映する（編集器なしで実行できる）。

- 追加物は ID 5000 以降に置き、再実行時はそこだけ作り直す。玩法の物体（区域・カメラ・演出）は消さない。
- 旧布景は削除せず enabled=false にする（--revert で戻す）。区域のテレビは新モデルへ差し替えて移動し、
  画面材質の索引は取り込み報告から求める（未取り込みなら -1 のまま、取り込み後に再実行する）。
- 夜の工場用の環境プロファイルと描画スタイルを新規作成し、場景に割り当てる。調整値は TUNE にまとめる。
実行: python apply_layout_to_scene.py [--view V4_gameplay] [--tune key=value ...] [--revert]
"""

import argparse
import hashlib
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent
SCENE_PATH = PROJECT / "Assets" / "Scenes" / "TD2_Rules01.scene.json"
STAGE02 = PROJECT / "Assets" / "Environment" / "TD2_Stage02.henvironment"
STYLE_PATH = PROJECT / "Assets" / "RenderStyles" / "RenderStyle.hrenderstyle"
# 新規の環境資産は編集器の資産登録が要るため、場景が使っている既存資産をその場で書き換える（原本は Temp に退避）。
ENV_BACKUP = ROOT / "Temp" / "TD2_Stage02.original.henvironment"
STYLE_BACKUP = ROOT / "Temp" / "RenderStyle.original.hrenderstyle"
# 使われていなかった全画面 CRT のポスト資産を、撮影レンズ（周辺減光・色収差・粒子）の設定に置き換える。
POST_PATH = PROJECT / "Assets" / "PostProfiles" / "PostProfile.hpostprofile"
POST_BACKUP = ROOT / "Temp" / "PostProfile.original.hpostprofile"
META_ROOT = PROJECT / "ProjectSettings" / "AssetMeta"
FIRST_ID = 5000
STATE = ROOT / "Temp" / "applied_state.json"

# 調整値：ヘッドレスの画面を見ながら詰める。光束は Blender の W に掛ける係数。
TUNE = {
    "ev100": 4.0,              # 手動露出（小さいほど明るい）
    # エンジンのスポット光束は全球に配る扱いなので、工場の高天井灯は実物並み（約 4 万 lm）にする。
    "hall_lm_per_w": 48.0,     # 吊り灯・天車灯（Blender W → lm）
    "desk_lm_per_w": 30.0,     # 卓上灯・作業灯
    "spill_lm": 150.0,         # 画面のこぼれ光（1 m² あたりではなく 1 台あたり）。台とベゼルを青緑に染める主光
    # 月光は左（-X）の高窓から低く差し込ませ、霧に光の筋、床に窓枠の形の光を落とす。
    "moon_lux": 80.0,          # 窓から差す月光の照度
    "moon_azimuth": 250.0,     # エンジンの太陽方位（度）。東 +X・北 -Z
    "moon_elevation": 26.0,
    "fog_anisotropy": 0.6,     # 前方散乱。光源の方を向くと筋が強く光る
    "fog_density": 0.04,
    "fog_start": 3.0,          # 画面がかすまないよう、カメラから画面までより遠く
    "sky_light": 0.015,
    "sky_tint": 3.5,           # 屋根の破れ目から見える夜空の明るさ。画面より目立たせない
    "game_fov": 26.0,
    "shot_dist": 2.3,          # 画面からカメラまで（m）。隣のテレビが両脇に少し入る距離
    "shot_up": 0.06,
    "shot_pullback": 0.5,      # 切り替えの中間で引く量。隣り合う二台を同時に見せる
    "shot_seconds": 1.0,       # 切り替え演出の長さ（入力は止まるので短く）
    # 絵の締まり：トーンの対比、明部だけのにじみ、レンズの癖。どれも「気づかれない程度」に留める。
    "contrast": 1.2,
    "bloom": 0.1,
    "bloom_threshold": 0.85,
    "vignette": 0.42,          # 隅での減光の割合
    "aberration": 0.6,         # 隅での色ずれ（px）
    "grain": 0.045,
    # 游戏機位の被写界深度：画面と隣のテレビ（同じ奥行き）は合焦、背後の棚と窓だけを柔らかくぼかす。
    "dof_range": 0.5,
    "dof_far_transition": 2.5,
    "dof_far_px": 12.0,
}

SEQ_AB = PROJECT / "Assets" / "Sequences" / "TD2_TV_RegionAB.hsequence"
SEQ_AB_BACKUP = ROOT / "Temp" / "TD2_TV_RegionAB.original.hsequence"
SEQ_TV2A_REL = "Assets/Sequences/TD2_TV_Region_TV2_A.hsequence"
REGION_IDS = {"B": "td2.region.25", "TV2": "td2.region.26", "A": "td2.region.rules01"}


def guid_for(key: str) -> str:
    return hashlib.md5(key.encode("utf-8")).hexdigest()


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path, data):
    # 既存場景と同じ書式（キー順・2 空白インデント・CRLF・BOM なし）で保存する。
    text = json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    path.write_bytes(text.replace("\n", "\r\n").encode("utf-8"))


def write_meta(source_rel, guid, importer, kind, display, settings=None, importer_version=1):
    meta_path = META_ROOT / (source_rel + ".hikari.asset.json")
    if meta_path.exists():
        return load_json(meta_path)["guid"]
    meta_path.parent.mkdir(parents=True, exist_ok=True)
    meta = {"displayName": display, "guid": guid, "importSettings": settings or {}, "importerId": importer,
            "importerVersion": importer_version, "metaVersion": 1, "sourcePath": source_rel, "type": kind}
    meta_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return guid


def rot_from_dir(d):
    """前方 +Z、回転は Ry(yaw)·Rx(pitch) の規約で、向き d を向く [pitch, yaw, 0] を返す。"""
    x, y, z = d
    n = math.sqrt(x * x + y * y + z * z) or 1.0
    x, y, z = x / n, y / n, z / n
    return [round(math.degrees(math.asin(max(-1.0, min(1.0, -y)))), 4), round(math.degrees(math.atan2(x, z)), 4), 0.0]


def yaw_rotate(v, yaw_deg):
    a = math.radians(yaw_deg)
    return [v[0] * math.cos(a) + v[2] * math.sin(a), v[1], -v[0] * math.sin(a) + v[2] * math.cos(a)]


def screen_index(guid, part=None, manifest=None):
    """取り込み報告があればその材質順、なければ FBX 内の出現順から TD2_Screen の位置を求める。"""
    report = PROJECT / "Library" / "Imported" / guid / "import_report.json"
    if report.exists():
        names = [m.get("name") for m in load_json(report)["diagnostics"]["materials"]]
    elif part and manifest:
        names = manifest[part].get("material_order", [])
    else:
        return -1
    return names.index("TD2_Screen") if "TD2_Screen" in names else -1


def light_obj(oid, name, pos, rot, props):
    base = {"angularFalloff": 1.0, "castsShadows": True, "color": [1.0, 1.0, 1.0], "cookie": "", "enabled": True,
            "height": 1.0, "iesProfile": "", "innerConeAngleDegrees": 25.0, "intensity": 100.0,
            "intensityUnit": "Lumens", "length": 1.0, "outerConeAngleDegrees": 35.0, "priority": 0, "range": 10.0,
            "sceneRelativeIntensity": 1.0, "sourceRadius": 0.0, "type": "Spot"}
    base.update(props)
    return {"components": [{"id": 1, "properties": base, "type": "LightComponent"}], "editorLocked": False,
            "editorVisible": True, "enabled": True, "id": oid, "name": name, "parent": None, "sourcePrefabId": "",
            "transform": {"position": pos, "rotationEulerDeg": rot, "scale": [1.0, 1.0, 1.0]}}


def model_obj(oid, name, guid, pos, rot, scale, cast=True):
    return {"components": [{"id": 1, "properties": {"assetId": guid, "castShadow": cast, "receiveShadow": True,
                                                    "visible": True}, "type": "ModelComponent"}],
            "editorLocked": False, "editorVisible": True, "enabled": True, "id": oid, "name": name, "parent": None,
            "sourcePrefabId": "", "transform": {"position": pos, "rotationEulerDeg": rot, "scale": scale}}


GAMEPLAY_TYPES = {"CameraComponent", "TD2.Region", "TD2.RegionSequence", "TD2.Passage", "TD2.CameraPerformance"}


def build_environment():
    """Stage02 を土台に、夜の工場用の環境を作る。空と太陽を抑え、月光・霧・手動露出で絵を決める。"""
    if not ENV_BACKUP.exists():
        ENV_BACKUP.write_bytes(STAGE02.read_bytes())
    env = load_json(ENV_BACKUP)
    t = TUNE["sky_tint"]
    env["sky"].update({"lightIntensity": TUNE["sky_light"], "reflectionIntensity": TUNE["sky_light"] * 1.5,
                       "tint": [round(1.0 * t, 3), round(0.86 * t, 3), round(0.62 * t, 3)]})  # 青の飽和を抑えて冷たい灰に
    env["sun"].update({"illuminance": TUNE["moon_lux"], "color": [0.62, 0.74, 1.0], "showDisk": False, "mode": "Fixed",
                       "fixedAzimuthDegrees": TUNE["moon_azimuth"], "fixedElevationDegrees": TUNE["moon_elevation"]})
    env["moon"]["enabled"] = False
    env["exposure"].update({"mode": "Manual", "manualEv100": TUNE["ev100"]})
    env["fog"].update({"enabled": True, "volumetric": True, "albedo": [0.6, 0.63, 0.66], "density": TUNE["fog_density"],
                       "heightFalloff": 0.14, "baseHeight": 0.0, "startDistance": TUNE["fog_start"],
                       "scatteringIntensity": 1.0, "ambientScatteringStrength": 0.05, "anisotropy": TUNE["fog_anisotropy"],
                       "sunShaftStrength": 1.0, "noise": 0.35, "noiseScale": 6.0})
    env["bloom"].update({"enabled": True, "threshold": TUNE["bloom_threshold"], "intensity": TUNE["bloom"]})
    # 影は冷たく、明部は暖かく分ける。画面の青緑・机の暖色・月の青の三色に絞る。
    env["colorGrading"].update({"midtonesTint": [1.0, 1.0, 1.0], "shadowsTint": [0.95, 0.99, 1.05],
                                "highlightsTint": [1.04, 1.0, 0.95]})
    env["toneMapping"].update({"curve": "AgX", "contrast": TUNE["contrast"], "saturation": 1.0,
                               "whiteBalanceTemperature": 6500.0, "whiteBalanceTint": 0.0})
    env["shadows"].update({"contactShadows": True, "contactShadowLength": 0.25})
    env["ambientOcclusion"].update({"enabled": True, "radius": 0.8, "strength": 1.2})
    env["clouds"]["enabled"] = False
    env["stars"]["enabled"] = False
    write_json(STAGE02, env)


def build_render_style():
    """全画面の CRT を外した物理シェーディングの描画スタイル。影の色は中立。ポストは撮影レンズの癖だけ。"""
    if not STYLE_BACKUP.exists():
        STYLE_BACKUP.write_bytes(STYLE_PATH.read_bytes())
    if not POST_BACKUP.exists():
        POST_BACKUP.write_bytes(POST_PATH.read_bytes())
    lens = {"vignette": TUNE["vignette"], "vignetteRoundness": 0.55, "chromaticAberration": TUNE["aberration"],
            "grain": TUNE["grain"], "grainSize": 1.6, "grainShadowBias": 0.6}
    write_json(POST_PATH, {"effect": "engine.post.effect.lens", "formatVersion": 1, "name": "TD2_Lens",
                           "values": {k: [v, 0.0, 0.0, 0.0] for k, v in lens.items()}})
    post_guid = load_json(META_ROOT / "Assets" / "PostProfiles" / "PostProfile.hpostprofile.hikari.asset.json")["guid"]
    style = {"formatVersion": 1, "name": "Realistic", "post": [{"enabled": True, "profile": post_guid}],
             "shading": {"bands": 1, "model": "Physical", "rim": 0.0, "shadowTint": [1.0, 1.0, 1.0],
                         "softness": 0.03, "specular": 1.0, "threshold": 0.3}}
    write_json(STYLE_PATH, style)


def screen_center(item, manifest):
    s = manifest[item["part"]]["screen"]
    sc = item["scale"][0]
    local = [s["center_x_m"] * sc, (s["bottom_m"] + s["height_m"] / 2) * sc, s["front_z_m"] * sc]
    off = yaw_rotate(local, item["rotation_deg"][1])
    return [item["position"][i] + off[i] for i in range(3)]


def shot_for(center):
    """画面の正面に立つ游戏机位。テレビは +Z を向いているので、+Z 側へ下がって画面中心を見る。"""
    pos = [round(center[0], 4), round(center[1] + TUNE["shot_up"], 4), round(center[2] + TUNE["shot_dist"], 4)]
    return pos, rot_from_dir([center[i] - pos[i] for i in range(3)])


def sequence_json(guid, name, shot_a, shot_b):
    """区域 A の機位から B の機位へ。中間で少し引いて、接している二台を同時に見せる（入力は止まるので短く）。"""
    (pa, ra), (pb, rb) = shot_a, shot_b
    mid = [round((pa[i] + pb[i]) / 2, 4) for i in range(3)]
    mid[1] = round(mid[1] + 0.04, 4)
    mid[2] = round(mid[2] + TUNE["shot_pullback"], 4)
    mid_rot = [round((ra[i] + rb[i]) / 2, 4) for i in range(3)]
    dur = TUNE["shot_seconds"]
    keys = [(0.0, pa, ra), (round(dur * 0.5, 4), mid, mid_rot), (dur, pb, rb)]
    lens = [{"id": i + 1, "timeSeconds": t, "verticalFovDegrees": TUNE["game_fov"], "nearClip": 0.05, "farClip": 500,
             "interpolation": "smooth"} for i, t in enumerate((0.0, dur))]
    return {"assetType": "Sequence", "formatVersion": 3, "guid": guid, "displayName": name, "sequence": {
        "id": 1, "name": name, "durationSeconds": dur,
        "bindings": [{"id": 1, "name": "游戏摄像机", "targetKind": "slot", "slotName": "Camera", "required": True}],
        "tracks": [
            {"id": 1, "type": "cameraCut", "enabled": True, "clips": [{
                "id": 1, "cameraBindingId": 1, "camera": {"owned": False}, "startTimeSeconds": 0, "durationSeconds": dur,
                "transition": {"mode": "cut", "durationSeconds": 0}, "effects": []}]},
            {"id": 2, "type": "cameraTransform", "enabled": True, "channels": [{"cameraBindingId": 1, "keyframes": [
                {"id": i + 1, "timeSeconds": t, "position": p, "rotationEulerDeg": r, "interpolation": "smooth"}
                for i, (t, p, r) in enumerate(keys)]}]},
            {"id": 3, "type": "cameraLens", "enabled": True, "channels": [{"cameraBindingId": 1, "keyframes": lens}]}],
        "screenEffects": []}}


def write_sequences(shots):
    """B↔TV2 は既存の演出資産を書き換え、TV2↔A は新しい演出資産を作る。原本は Temp に退避する。"""
    if not SEQ_AB_BACKUP.exists():
        SEQ_AB_BACKUP.write_bytes(SEQ_AB.read_bytes())
    ab_guid = load_json(SEQ_AB)["guid"]
    write_json(SEQ_AB, sequence_json(ab_guid, "B ↔ TV2", shots["B"], shots["TV2"]))
    tv2a_guid = guid_for("TD2/Sequence/TV2_A")
    write_json(PROJECT / SEQ_TV2A_REL, sequence_json(tv2a_guid, "TV2 ↔ A", shots["TV2"], shots["A"]))
    write_meta(SEQ_TV2A_REL, tv2a_guid, "SequenceAssetImporter", "Sequence", "TV2 ↔ A", importer_version=3)
    return {("B", "TV2"): ab_guid, ("TV2", "A"): tv2a_guid}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--view", default="V4_gameplay")
    ap.add_argument("--tune", nargs="*", default=[])
    ap.add_argument("--revert", action="store_true")
    args = ap.parse_args()
    for kv in args.tune:
        k, v = kv.split("=")
        TUNE[k] = float(v)

    scene = load_json(SCENE_PATH)
    objects = scene["objects"]
    state = load_json(STATE) if STATE.exists() else {}
    backup = ROOT / "Temp" / "scene_before_factory.json"
    if not backup.exists():
        backup.write_bytes(SCENE_PATH.read_bytes())

    if args.revert:
        for o in objects:
            if o["id"] in state.get("disabled", []):
                o["enabled"] = True
        scene["objects"] = [o for o in objects if o["id"] < FIRST_ID]
        for key, saved in state.get("originals", {}).items():
            o = next(x for x in scene["objects"] if x["id"] == int(key))
            o["transform"], o["components"] = saved["transform"], saved["components"]
        if "environment" in state:
            scene["environment"] = state["environment"]
        write_json(SCENE_PATH, scene)
        for backup_file, target in ((ENV_BACKUP, STAGE02), (STYLE_BACKUP, STYLE_PATH), (POST_BACKUP, POST_PATH),
                                    (SEQ_AB_BACKUP, SEQ_AB)):
            if backup_file.exists():
                target.write_bytes(backup_file.read_bytes())
                backup_file.unlink()
        (PROJECT / SEQ_TV2A_REL).unlink(missing_ok=True)
        (META_ROOT / (SEQ_TV2A_REL + ".hikari.asset.json")).unlink(missing_ok=True)
        STATE.unlink(missing_ok=True)
        print("reverted")
        return

    layout = load_json(ROOT / "factory_set01_layout.json")
    manifest = {p["id"]: p for p in load_json(ROOT / "factory_set01_manifest.json")["parts"]}
    guids = load_json(ROOT / "Temp" / "installed_guids.json")

    objects = [o for o in objects if o["id"] < FIRST_ID]
    by_id = {o["id"]: o for o in objects}
    originals = state.get("originals", {})
    disabled = set(state.get("disabled", []))
    region_ids = {}
    for o in objects:
        types = {c["type"] for c in o["components"]}
        if "TD2.Region" in types:
            props = next(c for c in o["components"] if c["type"] == "TD2.Region")["properties"]
            region_ids[{"td2.region.25": "B", "td2.region.26": "TV2", "td2.region.rules01": "A"}.get(props["regionId"], "?")] = o["id"]
        if types & GAMEPLAY_TYPES:
            continue
        if types & {"ModelComponent", "LightComponent", "ProceduralMeshComponent"} and o["id"] not in disabled:
            # 実行時は物体の enabled を見ないため、モデルの visible と灯の enabled も落とす（元の値は退避）。
            originals.setdefault(str(o["id"]), json.loads(json.dumps({"transform": o["transform"], "components": o["components"]})))
            o["enabled"] = False
            for c in o["components"]:
                if c["type"] == "ModelComponent":
                    c["properties"]["visible"] = False
                    c["properties"]["castShadow"] = False
                elif c["type"] == "LightComponent":
                    c["properties"]["enabled"] = False
            disabled.add(o["id"])
    if "environment" not in state:
        state["environment"] = json.loads(json.dumps(scene["environment"]))

    next_id = FIRST_ID
    added = []
    counts = {}
    for item in layout["objects"]:
        guid = guids[item["part"]]
        if "region" in item:
            oid = region_ids[item["region"]]
            o = by_id[oid]
            originals.setdefault(str(oid), json.loads(json.dumps({"transform": o["transform"], "components": o["components"]})))
            o["transform"] = {"position": item["position"], "rotationEulerDeg": item["rotation_deg"], "scale": item["scale"]}
            for c in o["components"]:
                if c["type"] == "ModelComponent":
                    c["properties"] = {k: v for k, v in c["properties"].items() if not k.startswith("materialOverrides")}
                    c["properties"]["assetId"] = guid
                elif c["type"] == "TD2.Region":
                    c["properties"]["screenModelAssetGuid"] = guid
                    c["properties"]["screenMaterialIndex"] = screen_index(guid, item["part"], manifest)
                    c["properties"]["screenSlot"] = "TD2_Screen"
            # 画面のこぼれ光：画面と同じ大きさの面光源を画面の少し手前に置く。
            s = manifest[item["part"]]["screen"]
            sc = item["scale"][0]
            local = [s["center_x_m"] * sc, (s["bottom_m"] + s["height_m"] / 2) * sc, (s["front_z_m"] + 0.04) * sc]
            off = yaw_rotate(local, item["rotation_deg"][1])
            pos = [round(item["position"][i] + off[i], 4) for i in range(3)]
            next_id += 1
            added.append(light_obj(next_id, f"F01_Spill_{item['region']}", pos, [0.0, item["rotation_deg"][1], 0.0], {
                "type": "Rect", "castsShadows": False, "color": [0.62, 0.84, 0.76], "intensity": TUNE["spill_lm"],
                "length": round(s["width_m"] * sc, 4), "height": round(s["height_m"] * sc, 4), "range": 2.5}))
            continue
        counts[item["label"]] = counts.get(item["label"], 0) + 1
        next_id += 1
        name = f"F01_{item['label'].replace('SM_F_', '')}_{counts[item['label']]:02d}"
        added.append(model_obj(next_id, name, guid, item["position"], item["rotation_deg"], item["scale"],
                               item.get("cast_shadow", True)))

    for lt in layout["lights"]:
        if lt["type"] != "SPOT" or not lt["alive"]:
            continue
        next_id += 1
        cone = lt["cone_deg"] / 2
        added.append(light_obj(next_id, f"F01_Light_{lt['name']}_{next_id}", lt["position"], rot_from_dir(lt["direction"]), {
            "type": "Spot", "color": lt["color"], "intensity": round(lt["blender_power_w"] * TUNE["hall_lm_per_w" if lt["tag"] == "hall" else "desk_lm_per_w"], 2),
            "outerConeAngleDegrees": round(cone, 2), "innerConeAngleDegrees": round(cone * (1 - lt["blend"] * 0.8), 2),
            "range": 14.0 if lt["tag"] == "hall" else 6.0, "sourceRadius": 0.05 if lt["tag"] == "hall" else 0.02,
            "castsShadows": True}))

    # 仮の起点ルート（並べ替えや一括選択の目印。変換は単位）
    added.insert(0, {"components": [], "editorLocked": False, "editorVisible": True, "enabled": True, "id": FIRST_ID,
                     "name": "FactorySet01", "parent": None, "sourcePrefabId": "",
                     "transform": {"position": [0.0, 0.0, 0.0], "rotationEulerDeg": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]}})

    # 游戏机位と区域切替の演出：世界地図で接する B | TV2 | A の順に、隣同士だけを結ぶ。
    shots = {item["region"]: shot_for(screen_center(item, manifest)) for item in layout["objects"] if "region" in item}
    sequences = write_sequences(shots)
    cam_obj = by_id[scene["camera"]["defaultCameraObjectId"]]
    originals.setdefault(str(cam_obj["id"]), json.loads(json.dumps({"transform": cam_obj["transform"], "components": cam_obj["components"]})))
    seq_holder = next(o for o in objects if o["id"] != cam_obj["id"] and any(c["type"] == "TD2.RegionSequence" for c in o["components"]))
    originals.setdefault(str(seq_holder["id"]), json.loads(json.dumps({"transform": seq_holder["transform"], "components": seq_holder["components"]})))
    for holder, (a, b) in ((seq_holder, ("B", "TV2")), (cam_obj, ("TV2", "A"))):
        for c in holder["components"]:
            if c["type"] == "TD2.RegionSequence":
                c["properties"].update({"regionA": REGION_IDS[a], "regionB": REGION_IDS[b],
                                        "sequenceAssetGuid": sequences[(a, b)], "sequenceSlotName": "Camera"})
    if args.view == "V4_gameplay":
        cam_pos, cam_rot, fov = shots["TV2"][0], shots["TV2"][1], TUNE["game_fov"]  # 開始区域 TV2 の機位
        focus = math.dist(cam_pos, screen_center(next(i for i in layout["objects"] if i.get("region") == "TV2"), manifest))
        far_px = TUNE["dof_far_px"]
    else:
        cam = next(c for c in layout["cameras"] if c["name"] == args.view)
        cam_pos, fov = cam["position"], cam["fov_y_deg"]
        cam_rot = rot_from_dir([cam["target"][i] - cam["position"][i] for i in range(3)])
        focus = math.dist(cam_pos, cam["target"])
        far_px = TUNE["dof_far_px"] if args.view == "V1_menu" else 0.0  # 全景は深くピントを置く
    cam_obj["transform"]["position"] = cam_pos
    cam_obj["transform"]["rotationEulerDeg"] = cam_rot
    for c in cam_obj["components"]:
        if c["type"] == "CameraComponent":
            c["properties"]["verticalFovDegrees"] = fov
            # 合焦帯は画面の奥行きだけ。近景（手前の台の縁）は少し、遠景（棚・窓）はやや強くぼかす。
            c["properties"]["depthOfField"] = {
                "enabled": far_px > 0, "focusDistance": round(focus, 3), "focusRange": TUNE["dof_range"],
                "nearTransition": 0.6, "farTransition": TUNE["dof_far_transition"],
                "nearBlurRadius1080pPixels": 8.0, "farBlurRadius1080pPixels": far_px}

    build_environment()
    build_render_style()
    scene["objects"] = objects + added
    write_json(SCENE_PATH, scene)
    STATE.write_text(json.dumps({"disabled": sorted(disabled), "originals": originals,
                                 "environment": state["environment"]}, ensure_ascii=False), encoding="utf-8")
    print(f"applied: {len(added)} objects, disabled {len(disabled)}, view {args.view}, regions {region_ids}")
    print("screen index:", {k: screen_index(guids[p], p, manifest) for k, p in (("A", "SM_F_TV_Wide"), ("B", "SM_F_TV_Portable"), ("TV2", "SM_F_TV_Broadcast"))})


if __name__ == "__main__":
    main()
