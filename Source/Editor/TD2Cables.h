#pragma once
// 画面の外の信号線：圧力板のテレビから、それが動かす仕掛けまで床と台の縁に沿って引く。
// 線は直管と継ぎ目の部品を並べたもので、一本ずつ TD2.SignalCable を付け、通電した時に光る版へ替える。
#include "TD2CameraLayout.h"
#include "TD2MachineEditor.h"

namespace TD2::Editor {
inline constexpr std::string_view kCableSegmentPath = "TD2Props/SM_TD2_CableSeg.gltf";
inline constexpr std::string_view kCableJointPath = "TD2Props/SM_TD2_CableJoint.gltf";
inline constexpr std::string_view kCableSegmentLit = "9c1ebe2d625143abb7defb769e3c7ba2";
inline constexpr std::string_view kCableJointLit = "5ceec717b6334b88bd8c43bf75deb8da";
inline constexpr std::string_view kSignalCableType = "TD2.SignalCable";

// 線の通り道（折れ線）。from は板のテレビの後ろの足元、to は仕掛けの部品か動くテレビの後ろ。
std::vector<Vec3> RouteCable(const Vec3& from, const Vec3& to, const LayoutSnapshot& layout);
// 折れ線を部品に分ける：直管（+Z を線の向きへ、Z の縮尺が長さ）と継ぎ目。
struct CablePiece { Pose pose; bool joint = false; };
std::vector<CablePiece> CablePieces(const std::vector<Vec3>& points);
// テレビの後ろの足元（線の端）。
Vec3 CableEnd(const LayoutRoom& room);
// 仕掛けの線の行き先：レシピの動かない部品（巻き上げ箱・転台の土台・レール…）、無ければ動くテレビの後ろ。
std::optional<Vec3> MachineCableEnd(const Machine& m, const LayoutSnapshot& layout);
}
