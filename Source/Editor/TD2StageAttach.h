#pragma once
// 3D の布景で、テレビを地図の隣のテレビに合わせて置く。地図の行・列のずれは、画面のマスの大きさで 3D のずれに直す。
#include "TD2CameraLayout.h"
#include "TD2MachineEditor.h"
#include <optional>

namespace TD2::Editor {
// テレビの寸法と姿勢（縮尺前の寸法と、物体の姿勢）。
struct ScreenBox {
    Pose pose;
    double body[3]{1.1, .86, .7};
    double screenCenter[3]{0, .4, .28}, screenWidth = .84, screenHeight = .49;
    int cols = 1, rows = 1;
    double Scale() const { return std::abs(pose.scale[0]); }
    double CellWidth() const { return screenWidth * Scale() / (std::max)(1, cols); }
    double CellHeight() const { return screenHeight * std::abs(pose.scale[1]) / (std::max)(1, rows); }
};
ScreenBox BoxOf(const LayoutRoom& room);
RegionPlacement PlacementOf(const LayoutRoom& room);

// room を、地図で隣にある neighbor の横（上下）に置く姿勢。向きは neighbor と同じ、縮尺は room のまま。
// 共有する辺の向きは地図の重なりの中央で揃え、辺をまたぐ向きは筐体の間を gap だけ空け、画面の面は neighbor に揃える。
std::optional<Pose> AttachedPose(const ScreenBox& room, const RegionPlacement& roomPlace, const ScreenBox& neighbor,
                                 const RegionPlacement& neighborPlace, double gap = .12);

// 機構で地図の位置が a から b へ変わるテレビの 3D の動き（状態 A の姿勢を基準に、基準の向きで測る）。
// b で隣になるテレビがあれば、動く向きの成分だけをその隣に揃える。なければ画面のマスの大きさに比例させる。
// aligned には隣に揃えられたかを返す。
MachineObjectDelta MoveDelta(const ScreenBox& room, const RegionPlacement& a, const RegionPlacement& b,
                             const std::vector<std::pair<ScreenBox, RegionPlacement>>& neighbors, bool* aligned = nullptr);
// テレビの基準で測った移動を、向きの違う別の物体の基準で測り直す（回転は写さない）。
MachineObjectDelta RetargetMove(const MachineObjectDelta& d, const Pose& from, const Pose& to);
}
