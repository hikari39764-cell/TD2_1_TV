#pragma once
// 舞台へ出す部品（ScreenKit07）の提出要求を作る。部品の原点は格の下辺中央・正面（1 格 = 1）。
// Add の x は中心、bottomY は下辺の世界の格。作った要求は呼び手が引擎へ出す（毎フレーム出さない部品は引擎が消す）。
// 性能の約束：
// ・部品の組（持ち主・実体 ID・模型）は状態で変えない。状態で見た目が変わる部品（姿勢・押した板・開いた扉・光線など）は
//   全部を出しておき、使わないものは決まった退避位置へ置く。組や表示が変わると引擎は GPU の場景全体を作り直す
//   （1 回 10〜25 ms）。
// ・退避位置は部品ごとに動かない所（装置はその場の下、人物と演出は世界の外の一点）。隠した部品が状態に付いて動くと、
//   そのたびに場景の更新になる。
#include "Rules/TD2Rules.h"
#include "TD2StageProjection.h"
#include "HIKARI/Project/PresentationTypes.h"
#include <cstdint>
#include <string_view>
#include <vector>

namespace TD2 {
using PartRequest = HIKARI::GameProjectPresentationModelRequest;
// 人物は地形の奥行きの中ほどに立たせる（正面が 0、模型の奥行きは約 0.4 格）。
inline constexpr double kActorDepth = -0.15;
// 世界の外の退避点（世界の格）。人物・残像・予告の輪の使わない模型をここへ置く。
inline constexpr double kParkX = -64, kParkY = -64;

class StagePartBuilder {
public:
    StagePartBuilder(const StageProjection& projection, int palette, uint64_t owner, std::vector<PartRequest>& out)
        : projection_(projection), palette_(palette), owner_(owner), out_(out) {}
    // 意味名ごとに実体 ID の系列を固定する（格や装置が増減しても別の部品の ID を使い回さない）。
    void Begin(std::string_view key);
    // leanPivot は傾きを釣り合わせる高さ（部品の下辺から、格）。負なら部品の高さの中ほど。
    bool Add(std::string_view part, double x, double bottomY, double width = 1, double height = 1, double depth = 0,
             HIKARI::GameProjectQuaternion rotation = {}, double leanPivot = -1, bool visible = true);
    // 背景：闇の板と上から差す光の輪、1 格の目盛り。(x, y) は部屋の左上の世界の格。
    void Backdrop(int x, int y, int cols, int rows);
    // 地形。occupancy は部屋と周り 1 格の占有（(cols + 2) × (rows + 2)、外周は隣の部屋の格。部屋の無い所は埋まり扱い）。
    void Terrain(int x, int y, int cols, int rows, const std::vector<char>& occupancy);
    // 装置一つ（世界の格）。状態で変わる部品は全部出し、表示だけを切り替える。since は通電・実体が最後に変わってからの
    // 実秒（変わっていなければ大きな値）。扉・橋の出入りと通電灯の光の輪をこれで動かす。
    void Device(const Rules::DeviceSnapshot& device, bool checkpoint, double since);
    // 光線一組（棒と当たりの火花）。beam が無い時は発射器の場所で隠す。
    void Beam(const Rules::DeviceSnapshot& laser, const Rules::BeamState* beam);
    // 人物：姿勢と向きの模型を全部出し、今の一つだけを rect に立たせる（shown=false なら全部退避）。
    // grow は縦だけの伸び（足元を固定。残影を足元から刷り出す時に 0→1）。
    void Actor(const Rules::BodyState& body, const Rules::Rect& rect, bool echo, double time, bool shown, double grow = 1.0);
    size_t Failed() const noexcept { return failed_; }

private:
    // 機関の通電灯（点灯・消灯の二つを出して片方だけ見せる）と、通電が変わった直後に広がる光の輪。
    void Lamp(double x, double bottomY, bool lit, double since);
    // 引っ込んだ機関の輪郭（菫の破線）。部品の数は寸法だけで決まる。
    void Outline(double left, double top, double right, double bottom, bool visible);
    const StageProjection& projection_;
    int palette_ = 0;
    uint64_t owner_ = 0;
    std::vector<PartRequest>& out_;
    uint64_t semantic_ = 1, partIndex_ = 0;
    size_t failed_ = 0;
};
} // namespace TD2
