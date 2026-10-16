#pragma once
// 舞台へ出す部品（ScreenKit07）を組み立てる。部品の原点はマスの下辺中央・正面（1 マス = 1）。
// Add の x は中心、bottomY は下辺のマス。作った要求は呼び手がエンジンへ出す（出さなかった部品は消える）。
// 性能のための決まり：
// ・部品の組（持ち主・ID・モデル）は状態で変えない。見た目が状態で変わる部品は全部出しておき、
// 使わないものは決まった退避位置へ置く。組や表示が変わるとエンジンが GPU のシーン全体を作り直す（10〜25 ms）。
// ・退避位置は部品ごとに固定する（装置はその場の下、人物と演出は世界の外の一点）。
#include "Rules/TD2Rules.h"
#include "TD2StageProjection.h"
#include "HIKARI/Project/PresentationTypes.h"
#include <cstdint>
#include <string_view>
#include <vector>

namespace TD2 {
using PartRequest = HIKARI::GameProjectPresentationModelRequest;
// 人物は地形の奥行きの中ほどに立たせる（正面が 0、モデルの奥行きは約 0.4 マス）。
inline constexpr double kActorDepth = -0.15;
// 世界の外の退避点（世界のマス）。人物・残像・予告の輪の使わないモデルをここへ置く。
inline constexpr double kParkX = -64, kParkY = -64;

class StagePartBuilder {
public:
    StagePartBuilder(const StageProjection& projection, int palette, uint64_t owner, std::vector<PartRequest>& out)
        : projection_(projection), palette_(palette), owner_(owner), out_(out) {}
    // 意味名ごとに ID の系列を固定する（マスや装置が増減しても別の部品の ID を使い回さない）。
    void Begin(std::string_view key);
    // leanPivot は傾きを釣り合わせる高さ（部品の下辺から、マス）。負なら部品の高さの中ほど。
    bool Add(std::string_view part, double x, double bottomY, double width = 1, double height = 1, double depth = 0,
             HIKARI::GameProjectQuaternion rotation = {}, double leanPivot = -1, bool visible = true);
    // 背景：闇の板と上から差す光の輪、1 マスの目盛り。(x, y) は部屋の左上の世界のマス。
    void Backdrop(int x, int y, int cols, int rows);
    // 地形。occupancy は部屋と周り 1 マスの占有（(cols + 2) × (rows + 2)。部屋の無い所は埋まり扱い）。
    void Terrain(int x, int y, int cols, int rows, const std::vector<char>& occupancy);
    // 装置一つ（世界のマス）。状態で変わる部品は全部出し、表示だけを切り替える。since は通電・実体が最後に変わってからの
    // 実秒（変わっていなければ大きな値）。扉・橋の出入りと通電灯の光の輪をこれで動かす。
    void Device(const Rules::DeviceSnapshot& device, bool checkpoint, double since);
    // 光線一組（棒と当たりの火花）。beam が無い時は発射器の場所で隠す。
    void Beam(const Rules::DeviceSnapshot& laser, const Rules::BeamState* beam);
    // 人物：姿勢と向きのモデルを全部出し、今の一つだけを rect に立たせる（shown=false なら全部退避）。
    // grow は縦の伸び（足元を固定。残影が足元から現れる時に 0→1）。
    void Actor(const Rules::BodyState& body, const Rules::Rect& rect, bool echo, double time, bool shown, double grow = 1.0);
    size_t Failed() const noexcept { return failed_; }

private:
    // 仕掛けの通電灯（点灯・消灯の二つを出して片方だけ見せる）と、通電が変わった直後に広がる光の輪。
    void Lamp(double x, double bottomY, bool lit, double since);
    // 引っ込んだ仕掛けの輪郭（菫の破線）。部品の数は寸法だけで決まる。
    void Outline(double left, double top, double right, double bottom, bool visible);
    const StageProjection& projection_;
    int palette_ = 0;
    uint64_t owner_ = 0;
    std::vector<PartRequest>& out_;
    uint64_t semantic_ = 1, partIndex_ = 0;
    size_t failed_ = 0;
};
}
