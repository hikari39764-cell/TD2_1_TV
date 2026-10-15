# -*- coding: utf-8 -*-
"""電視の中のゲームの特效（HVFX）一式を作る。どれもエンジンの VFX 工作区でそのまま開いて直せる普通の資産。

方針
- 単位は「格」。原点は各特效の基準点（瞬移は出発点の体の中心で +X が進む向き、ほかは体か装置の中心・足元）。
  ゲーム側が舞台の 1 格の大きさと向き・長さを transform で掛ける。粒子は局所座標で動かし、その倍率に従わせる。
- 「電視の中のゲーム世界」の語彙で作る：像素の欠片、走査線、ブラウン管の点く／消える動き、速度線、菱形のきらめき、
  矢印の折れ線、塵の塊。輪（リング）は使わない。像素・素描の描き方を通るので、霞より輪郭のはっきりした形にする。
- 色は ScreenKit07 の配色（主角の琥珀・残影の菫・危険の赤・象牙）に合わせ、加算の HDR 値で置く。鮮やかな色は描き方の側で
  光が足される。短く切れ味よく（0.25〜0.9 秒）。
- 演出（電視の中の画面の揺れ・閃光）の既定値も表に持たせ、ゲーム側の TD2.ScreenFx で事件ごとに調整できるようにする。
出力: Assets/TD2/Screen3D/Fx/TD2_Fx_<名前>.hvfx と meta、Source/Runtime/TD2ScreenFxAssets.h（既定の GUID と演出の表）。
実行: python build_screen_fx.py [--only 名前,名前]
  --only は指定した特効の資産とメタだけを書く（VFX 工作区で直したほかの特効を上書きしない）。表（.h）は全部を書き直す。
"""

import copy
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent
FX_REL = "Assets/TD2/Screen3D/Fx"
TEMPLATE = json.loads((ROOT / "emitter_template.hvfx").read_text(encoding="utf-8"))["emitters"][0]

AMBER = (1.0, 0.55, 0.08)
AMBER_HOT = (1.6, 1.0, 0.35)
IVORY = (1.0, 0.92, 0.78)
WHITE = (1.4, 1.35, 1.25)
VIOLET = (0.45, 0.22, 1.0)
VIOLET_HOT = (0.62, 0.22, 1.5)   # 加算でも菫の彩度が残る（描き方の「鮮やか」判定を越える）
RED = (0.95, 0.10, 0.06)
RED_HOT = (1.5, 0.25, 0.12)


def tex(name):
    return f"engine.vfx.texture.{name}"


def scalar(v):
    return {"maximum": v, "minimum": v, "mode": "Constant", "randomGroup": 0}


def scalar_range(a, b):
    return {"maximum": max(a, b), "minimum": min(a, b), "mode": "RandomRange", "randomGroup": 0}


def vector(v):
    return {"maximum": list(v), "minimum": list(v), "mode": "Constant", "randomGroup": 0}


def vector_range(a, b):
    # 各成分ごとに小さい方を minimum にする（左右反転した速度でも範囲が逆転しない）。
    low = [min(x, y) for x, y in zip(a, b)]
    high = [max(x, y) for x, y in zip(a, b)]
    return {"maximum": high, "minimum": low, "mode": "RandomRange", "randomGroup": 0}


class Layer:
    """テンプレートの図層を複製し、ID を図層名で一意にして各モジュールを書き換える。"""

    def __init__(self, fx, name, kind):
        self.id = f"{fx}.{name}"
        self.e = copy.deepcopy(TEMPLATE)
        self._rename(self.e)
        self.e["id"], self.e["name"], self.e["kind"] = self.id, name, kind
        self.m = self.e["modules"]
        self.m["basic"]["localSimulation"] = True
        rc = self.m["renderCommon"]
        rc["blendMode"], rc["colorTextureAssetGuid"] = "Additive", ""
        self.m["renderShape"]["ringSegmentCount"] = 64

    def _rename(self, node):
        if isinstance(node, dict):
            for k, v in node.items():
                if k == "id" and isinstance(v, str) and node is not self.e:
                    node[k] = f"{self.id}/{v}"
                else:
                    self._rename(v)
        elif isinstance(node, list):
            for v in node:
                self._rename(v)

    def curve(self, name, keys, interpolation="Smooth"):
        base = copy.deepcopy(self.m["position"]["value"]["curve"]["x"])
        base["interpolation"] = interpolation
        base["keys"] = [{"id": f"{self.id}/{name}.{i}", "time": float(t), "value": float(v), "inTangent": 0.0,
                         "outTangent": 0.0} for i, (t, v) in enumerate(keys)]
        return base

    def timing(self, delay, active):
        self.e["startDelay"], self.e["activeDuration"] = delay, active
        return self

    def place(self, position=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1)):
        self.m["emitterTransform"]["transform"] = {"position": list(position), "rotationEulerDegrees": list(rotation),
                                                   "scale": list(scale)}
        return self

    def color(self, rgb, alpha_keys, interpolation="Smooth"):
        value = self.m["color"]["value"]
        value["mode"] = "Curve"
        for channel, v in zip("rgb", rgb):
            value["curve"][channel] = self.curve(f"color.{channel}", [(0.0, v)])
        value["curve"]["a"] = self.curve("color.a", alpha_keys, interpolation)
        return self

    def vec_curve(self, module, prop, keys, interpolation="Smooth"):
        value = self.m[module][prop]
        value["mode"] = "Curve"
        for i, axis in enumerate("xyz"):
            value["curve"][axis] = self.curve(f"{module}.{axis}", [(t, v[i]) for t, v in keys], interpolation)
        return self

    def scale_keys(self, keys, interpolation="Smooth"):
        return self.vec_curve("scale", "value", keys, interpolation)

    def scale_fixed(self, v):
        self.m["scale"]["value"]["mode"] = "Fixed"
        self.m["scale"]["value"]["fixed"] = vector(v)
        return self

    def position_keys(self, keys):
        return self.vec_curve("position", "value", keys)

    def spin(self, low, high):
        self.m["rotation"]["eulerDegrees"]["mode"] = "Pva"
        self.m["rotation"]["eulerDegrees"]["pva"]["velocity"] = vector_range((0, 0, low), (0, 0, high))
        return self

    def look(self, shape, billboard="Fixed", texture="", depth="WorldOccluded"):
        self.m["renderShape"]["type"], self.m["renderShape"]["billboardMode"] = shape, billboard
        self.m["renderCommon"]["colorTextureAssetGuid"] = tex(texture) if texture else ""
        self.m["renderCommon"]["depthMode"] = depth
        return self


def shape(fx, name, kind, delay, active):
    layer = Layer(fx, name, "Shape").timing(delay, active)
    layer.m["spawn"]["maximumParticles"] = 1
    layer.m["position"]["radialSpeed"] = scalar(0.0)
    return layer.look(kind)


def burst(fx, name, count, lifetime, delay=0.0):
    layer = Layer(fx, name, "Particles").timing(delay, 0.035)
    spawn = layer.m["spawn"]
    spawn["burstTimeline"] = [{"id": f"{layer.id}/burst", "time": 0.0, "count": count}]
    spawn["maximumParticles"] = max(count, 16)
    layer.m["basic"]["lifetime"] = scalar_range(*lifetime)
    layer.m["shape"]["type"], layer.m["shape"]["radius"] = "Sphere", scalar(0.06)
    return layer


def radial(layer, speed, gravity=-6.0, drag=1.5, radius=None):
    layer.m["position"]["radialSpeed"] = scalar_range(*speed)
    layer.m["forces"]["gravity"], layer.m["forces"]["drag"] = [0.0, gravity, 0.0], drag
    if radius is not None:
        layer.m["shape"]["radius"] = scalar(radius)
    return layer


def velocity(layer, low, high, gravity=0.0, drag=0.0):
    value = layer.m["position"]["value"]
    value["mode"] = "Pva"
    value["pva"]["velocity"] = vector_range(low, high)
    layer.m["position"]["radialSpeed"] = scalar(0.0)
    layer.m["forces"]["gravity"], layer.m["forces"]["drag"] = [0.0, gravity, 0.0], drag
    return layer


def box_spawn(layer, half):
    layer.m["shape"]["type"], layer.m["shape"]["boxHalfExtents"] = "Box", list(half)
    return layer


def streaks(layer, length=0.42, width=0.05):
    """進む向きに伸びた短い光の線（火花）。"""
    layer.look("Sprite", "DirectionalBillboard", "soft_line")
    return layer.scale_keys([(0.0, (width, length, width)), (0.6, (width * 0.5, length * 0.4, width * 0.5)),
                             (1.0, (0.0, 0.02, 0.0))])


def bits(layer, size=0.08):
    """像素の欠片：回りながら小さくなる四角。像素の描き方では一粒ずつのドットに見える。"""
    layer.look("MeshBox").scale_keys([(0.0, (size, size, 0.03)), (0.6, (size * 0.8, size * 0.8, 0.03)),
                                      (1.0, (size * 0.2, size * 0.2, 0.03))])
    return layer.spin(-540, 540)


def puffs(layer, size=0.14):
    """塵の塊：膨らんでから縮む輪郭のはっきりした丸。"""
    layer.look("Sprite", "RotatedBillboard", "particle_hard")
    return layer.scale_keys([(0.0, (size,) * 3), (0.35, (size * 1.7,) * 3), (1.0, (size * 0.4,) * 3)])


def sparkle(fx, name, delay, active, size, color, position=(0, 0, 0.1), texture="spark_dot"):
    """菱形のきらめき：一瞬で開いて閉じる。"""
    layer = shape(fx, name, "Sprite", delay, active).look("Sprite", "RotatedBillboard", texture).place(position)
    layer.scale_keys([(0.0, (size * 0.15,) * 3), (0.25, (size,) * 3), (1.0, (0.0,) * 3)])
    return layer.color(color, [(0, 1), (0.6, 1), (1, 0)])


# ---------------------------------------------------------------- 特効の定義
def fx_dash():
    f = "dash"
    layers = []
    # 速度線：中心ほど長く太い。終点側へ縮みながら消える。
    for i, (dy, length, thick, delay) in enumerate(((-0.3, 0.5, 0.025, 0.03), (-0.15, 0.78, 0.04, 0.015),
                                                     (0.0, 1.0, 0.07, 0.0), (0.15, 0.72, 0.04, 0.02),
                                                     (0.3, 0.46, 0.025, 0.04))):
        line = shape(f, f"line{i}", "MeshBox", delay, 0.2).place((1.0 - length * 0.5, dy, 0.05))
        line.scale_keys([(0, (length, thick, 0.03)), (0.5, (length * 0.7, thick * 0.6, 0.03)), (1, (length * 0.2, 0.0, 0.03))])
        line.position_keys([(0, (0, 0, 0)), (1, (length * 0.4, 0, 0))])
        line.color(AMBER_HOT if i == 2 else AMBER, [(0, 1 if i == 2 else 0.8), (0.6, 0.7), (1, 0)])
        layers.append(line)
    layers.append(sparkle(f, "arrive", 0.02, 0.2, 0.95, AMBER_HOT, (1.0, 0, 0.1)))
    debris = bits(velocity(burst(f, "debris", 8, (0.18, 0.3), 0.02), (-4.0, -1.2, 0), (-1.5, 1.2, 0), gravity=-6, drag=2.0), 0.07)
    layers.append(debris.place((1.0, 0, 0.1)).color(AMBER, [(0, 1), (0.5, 0.9), (1, 0)]))
    return "Dash", "瞬移拖尾", 0.32, layers, 0.0, 0.0


def fx_impact():
    f = "impact"
    flash = sparkle(f, "flash", 0.0, 0.24, 1.15, AMBER_HOT)
    sparks = streaks(radial(burst(f, "sparks", 6, (0.16, 0.24)), (3.0, 5.0), gravity=-4), 0.44).color(AMBER, [(0, 1), (1, 0)])
    chips = bits(radial(burst(f, "chips", 6, (0.22, 0.32)), (1.8, 3.0), gravity=-9), 0.08).color(AMBER, [(0, 1), (1, 0)])
    return "Impact", "冲击", 0.28, [flash, sparks, chips], 0.04, 0.0


def fx_land():
    f = "land"
    layers = []
    for side, sign in (("left", -1), ("right", 1)):
        dust = puffs(velocity(burst(f, f"dust_{side}", 3, (0.3, 0.45)), (sign * 2.2, 0.1, 0), (sign * 0.8, 0.5, 0),
                              gravity=0.6, drag=5.0), 0.17)
        layers.append(dust.place((sign * 0.15, 0.06, 0.1)).color(IVORY, [(0, 0.75), (0.5, 0.55), (1, 0)]))
        pebbles = bits(velocity(burst(f, f"pebble_{side}", 3, (0.25, 0.32)), (sign * 2.2, 1.2, 0), (sign * 1.0, 2.2, 0),
                                gravity=-14), 0.05)
        layers.append(pebbles.place((sign * 0.1, 0.04, 0.1)).color(IVORY, [(0, 1), (1, 0)]))
    return "Land", "落地", 0.42, layers, 0.03, 0.0


def fx_bounce():
    f = "bounce"
    layers = []
    # 上向きの矢印「^」を二本の光の棒で組み、二段で上へ抜けていく（像素でも形が崩れない）。
    for i, (delay, y) in enumerate(((0.0, 0.1), (0.05, 0.45))):
        for side, sign in (("l", -1), ("r", 1)):
            bar = shape(f, f"chevron{i}{side}", "MeshBox", delay, 0.26).place((sign * 0.15, y, 0.08), (0, 0, -sign * 38))
            bar.scale_fixed((0.42, 0.08, 0.03)).position_keys([(0, (0, 0, 0)), (1, (0, 0.6, 0))])
            layers.append(bar.color(AMBER_HOT if i == 0 else AMBER, [(0, 1), (0.6, 0.8), (1, 0)]))
    chips = bits(radial(burst(f, "chips", 8, (0.2, 0.3)), (2.5, 4.0), gravity=-8), 0.07).color(AMBER, [(0, 1), (1, 0)])
    layers.append(chips)
    for side, sign in (("left", -1), ("right", 1)):
        dust = puffs(velocity(burst(f, f"dust_{side}", 2, (0.22, 0.32)), (sign * 1.8, 0.0, 0), (sign * 0.8, 0.4, 0),
                              gravity=0.4, drag=5.0), 0.11)
        layers.append(dust.place((sign * 0.1, -0.35, 0.1)).color(IVORY, [(0, 0.6), (1, 0)]))
    return "Bounce", "反弹", 0.36, layers, 0.06, 0.0


def fx_echo_form():
    f = "echo_form"
    # 菫の欠片が周りから中心へ集まり、走査線が上へ流れて形が「刷られる」。最後に中心がきらめく。
    gather = bits(radial(burst(f, "gather", 14, (0.28, 0.34)), (-2.4, -1.8), gravity=0.0, drag=0.0, radius=0.75), 0.07)
    layers = [gather.color(VIOLET_HOT, [(0, 0), (0.2, 1), (0.85, 1), (1, 0)])]
    for i in range(3):
        bar = shape(f, f"scan{i}", "MeshBox", 0.06 + 0.06 * i, 0.22).place((0, -0.45, 0.06))
        bar.scale_fixed((0.8, 0.035, 0.03)).position_keys([(0, (0, 0, 0)), (1, (0, 0.95, 0))])
        layers.append(bar.color(VIOLET, [(0, 0), (0.2, 0.9), (0.8, 0.6), (1, 0)]))
    layers.append(sparkle(f, "settle", 0.28, 0.2, 0.7, VIOLET_HOT))
    return "EchoForm", "残影生成", 0.52, layers, 0.0, 0.0


def fx_echo_recall():
    f = "echo_recall"
    # ブラウン管を消す時の動き：体の大きさの光が横線へ潰れ、点に縮んで消える。
    crt = shape(f, "crt_off", "MeshBox", 0.0, 0.3).place((0, 0, 0.06))
    crt.scale_keys([(0, (0.55, 0.85, 0.03)), (0.35, (0.8, 0.05, 0.03)), (0.7, (0.04, 0.04, 0.03)), (1, (0.0, 0.0, 0.03))])
    layers = [crt.color(VIOLET_HOT, [(0, 0.85), (0.7, 1), (1, 0)])]
    layers.append(sparkle(f, "dot", 0.2, 0.16, 0.45, VIOLET_HOT))
    chips = bits(radial(burst(f, "chips", 8, (0.18, 0.26), 0.05), (2.0, 3.2), gravity=-2), 0.06)
    layers.append(chips.color(VIOLET, [(0, 1), (1, 0)]))
    return "EchoRecall", "残影收回", 0.42, layers, 0.0, 0.0


def fx_echo_blocked():
    f = "echo_blocked"
    layers = []
    for name, angle in (("cross_a", 45), ("cross_b", -45)):
        bar = shape(f, name, "MeshBox", 0.0, 0.3).place((0, 0, 0.06), (0, 0, angle))
        bar.scale_keys([(0, (0.3, 0.09, 0.03)), (0.25, (1.0, 0.09, 0.03)), (1, (1.0, 0.0, 0.03))])
        layers.append(bar.color(VIOLET_HOT, [(0, 1), (0.7, 0.9), (1, 0)]))
    # 故障の帯：ずれた位置で明滅する横長の四角。
    for i, (x, y, w, h) in enumerate(((-0.25, 0.3, 0.45, 0.05), (0.2, -0.1, 0.3, 0.08), (-0.1, -0.35, 0.5, 0.04))):
        glitch = shape(f, f"glitch{i}", "MeshBox", 0.02 * i, 0.3).place((x, y, 0.07)).scale_fixed((w, h, 0.03))
        layers.append(glitch.color(VIOLET, [(0, 1), (0.2, 0), (0.4, 1), (0.6, 0), (0.8, 0.8), (1, 0)], "Step"))
    shards = bits(radial(burst(f, "shards", 6, (0.2, 0.32)), (2.0, 3.2), gravity=-6), 0.07).color(VIOLET, [(0, 1), (1, 0)])
    layers.append(shards)
    return "EchoBlocked", "残影受阻", 0.36, layers, 0.06, 0.0


def fx_death():
    f = "death"
    layers = [sparkle(f, "flash", 0.0, 0.14, 2.0, WHITE, texture="star_soft")]
    # 横一本の故障線が赤く明滅する。
    line = shape(f, "glitch_line", "MeshBox", 0.0, 0.24).place((0, 0, 0.07)).scale_fixed((2.6, 0.05, 0.03))
    layers.append(line.color(RED_HOT, [(0, 1), (0.25, 0), (0.5, 1), (0.75, 0.6), (1, 0)], "Step"))
    # 体が像素の欠片に崩れて飛び散る（赤と主角の琥珀）。
    for name, color, count in (("pixels_red", RED_HOT, 16), ("pixels_amber", AMBER, 6)):
        pixels = bits(radial(burst(f, name, count, (0.4, 0.7)), (3.5, 6.5), gravity=-12, drag=1.0, radius=0.3), 0.1)
        layers.append(pixels.color(color, [(0, 1), (0.7, 0.9), (1, 0)]))
    return "Death", "死亡", 0.72, layers, 0.18, 0.6


def fx_respawn():
    f = "respawn"
    # ブラウン管を点ける時の動き：点 → 横線 → 体の大きさの光、そして消える。
    crt = shape(f, "crt_on", "MeshBox", 0.0, 0.42).place((0, 0.45, 0.06))
    crt.scale_keys([(0, (0.0, 0.0, 0.03)), (0.15, (0.05, 0.05, 0.03)), (0.4, (0.9, 0.05, 0.03)), (0.65, (0.6, 0.9, 0.03)),
                    (1, (0.6, 0.9, 0.03))])
    layers = [crt.color(AMBER_HOT, [(0, 1), (0.65, 1), (1, 0)])]
    rise = bits(box_spawn(velocity(burst(f, "rise", 10, (0.4, 0.6), 0.12), (-0.3, 1.5, 0), (0.3, 3.0, 0)), (0.35, 0.05, 0.05)), 0.06)
    layers.append(rise.place((0, 0.1, 0.08)).color(AMBER, [(0, 1), (0.6, 0.8), (1, 0)]))
    layers.append(sparkle(f, "ready", 0.3, 0.2, 0.8, AMBER_HOT, (0, 0.5, 0.1)))
    return "Respawn", "复活", 0.65, layers, 0.0, 0.35


def fx_device():
    f = "device"
    layers = [sparkle(f, "glint", 0.0, 0.2, 0.8, IVORY, (0, 0.3, 0.1))]
    line = shape(f, "line", "MeshBox", 0.0, 0.14).place((0, 0.5, 0.06))
    line.scale_keys([(0, (0.2, 0.05, 0.03)), (0.4, (1.1, 0.05, 0.03)), (1, (1.2, 0.0, 0.03))])
    layers.append(line.color(IVORY, [(0, 1), (1, 0)]))
    pops = bits(velocity(burst(f, "pops", 6, (0.25, 0.34)), (-0.9, 1.5, 0), (0.9, 2.6, 0), gravity=-10), 0.07)
    layers.append(pops.place((0, 0.4, 0.08)).color(IVORY, [(0, 1), (1, 0)]))
    return "Device", "机关切换", 0.32, layers, 0.0, 0.0


def fx_checkpoint():
    f = "checkpoint"
    pillar = shape(f, "pillar", "MeshBox", 0.0, 0.5).place((0, 1.5, 0.05))
    pillar.scale_keys([(0, (0.4, 3.0, 0.03)), (0.3, (0.12, 3.2, 0.03)), (1, (0.0, 3.4, 0.03))])
    layers = [pillar.color(AMBER_HOT, [(0, 0), (0.08, 1), (1, 0)])]
    layers.append(sparkle(f, "glint", 0.0, 0.3, 1.4, WHITE, (0, 0.7, 0.1), texture="star_glint"))
    rise = bits(box_spawn(velocity(burst(f, "rise", 14, (0.5, 0.9), 0.04), (-0.3, 1.0, 0), (0.3, 2.5, 0)), (0.4, 0.1, 0.05)), 0.06)
    layers.append(rise.place((0, 0.1, 0.08)).color(AMBER_HOT, [(0, 1), (0.7, 0.7), (1, 0)]))
    return "Checkpoint", "存档点", 0.9, layers, 0.0, 0.45


def fx_echo_absorb():
    f = "echo_absorb"
    # 回収した主角の体：周りの菫の欠片が体へ吸い込まれ、体を囲む菫の枠が内へ縮んで消える（残影を取り込んだ合図）。
    # 素描の描き方でも形が残るよう、霞ではなく太めの棒と欠片で作る（菫は描き方で残影の横縞になる）。
    gather = bits(radial(burst(f, "gather", 12, (0.22, 0.28)), (-3.2, -2.6), gravity=0.0, drag=0.0, radius=0.95), 0.08)
    layers = [gather.color(VIOLET_HOT, [(0, 0), (0.15, 1), (0.8, 1), (1, 0)])]
    for name, pos, size in (("top", (0, 0.55, 0.07), (0.9, 0.07, 0.03)), ("bottom", (0, -0.55, 0.07), (0.9, 0.07, 0.03)),
                            ("left", (-0.42, 0, 0.07), (0.07, 1.1, 0.03)), ("right", (0.42, 0, 0.07), (0.07, 1.1, 0.03))):
        bar = shape(f, f"frame_{name}", "MeshBox", 0.04, 0.26).place(pos).scale_fixed(size)
        bar.position_keys([(0, (0, 0, 0)), (1, (-pos[0] * 0.55, -pos[1] * 0.55, 0))])
        layers.append(bar.color(VIOLET_HOT, [(0, 0), (0.15, 1), (0.7, 0.8), (1, 0)]))
    layers.append(sparkle(f, "core", 0.24, 0.18, 0.75, VIOLET_HOT))
    return "EchoAbsorb", "残影吸收", 0.46, layers, 0.0, 0.0


EFFECTS = [fx_dash, fx_impact, fx_land, fx_bounce, fx_echo_form, fx_echo_recall, fx_echo_blocked, fx_death,
           fx_respawn, fx_device, fx_checkpoint, fx_echo_absorb]


def write_meta(rel, guid, display):
    meta = {"displayName": display, "guid": guid,
            "importSettings": {"cookVfx": True, "outputFormat": "HVFXC", "sourceFormat": ".hvfx"},
            "importerId": "VfxAssetImporter", "importerVersion": 27, "metaVersion": 1, "sourcePath": rel,
            "type": "VfxEffect"}
    path = PROJECT / "ProjectSettings" / "AssetMeta" / f"{rel}.hikari.asset.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main():
    args = sys.argv[1:]
    only = set(args[args.index("--only") + 1].split(",")) if "--only" in args else None
    table = []
    for build in EFFECTS:
        name, label, duration, layers, shake, flash = build()
        guid = hashlib.md5(f"TD2/ScreenFx/{name}".encode()).hexdigest()
        rel = f"{FX_REL}/TD2_Fx_{name}.hvfx"
        if only and name not in only:
            table.append((name, label, guid, rel, shake, flash))
            continue
        for order, layer in enumerate(layers):
            layer.e["order"] = order
        asset = {"durationSeconds": duration, "emitters": [layer.e for layer in layers], "inputs": [], "loop": False,
                 "name": f"TD2 {label}", "version": 15}
        (PROJECT / rel).parent.mkdir(parents=True, exist_ok=True)
        (PROJECT / rel).write_text(json.dumps(asset, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        write_meta(rel, guid, f"TD2 屏幕特效 · {label}")
        table.append((name, label, guid, rel, shake, flash))
        print("FX", name, guid, len(layers), "layers")
    lines = ["#pragma once",
             "// build_screen_fx.py が生成する。電視の中の特効（HVFX）の既定資産と演出（画面の揺れ・閃光）の既定値。",
             "// TD2.ScreenFx が無い時もこれを使う。shake は揺れの振幅（格）、flash は画面の明るさの一時的な上乗せ。",
             "#include <array>", "#include <string_view>", "", "namespace TD2 {",
             "struct ScreenFxDefault { std::string_view name, label, guid, path; float shake, flash; };",
             f"inline constexpr std::array<ScreenFxDefault, {len(table)}> kScreenFxDefaults{{{{"]
    lines += [f'    {{"{n}", "{l}", "{g}", "{r}", {s:.2f}f, {fl:.2f}f}},' for n, l, g, r, s, fl in table]
    lines += ["}};", "} // namespace TD2", ""]
    (PROJECT / "Source/Runtime/TD2ScreenFxAssets.h").write_text("\n".join(lines), encoding="utf-8")
    print("DONE", len(table))


if __name__ == "__main__":
    main()
