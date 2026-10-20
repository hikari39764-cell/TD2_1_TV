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
// 後ろの壁の窓の上下（部屋の上端・下端から窓の縁までのマス）。0 なら既定。操作の札を避けて窓を上か下へ寄せる時に使う。
struct WindowSpan {
    double top = 0, bottom = 0;
    bool operator==(const WindowSpan&) const = default;
};
// 世界の外の退避点（世界のマス）。人物・残像・予告の輪の使わないモデルをここへ置く。
inline constexpr double kParkX = -64, kParkY = -64;

class StagePartBuilder {
public:
    // kit は部品の組（0 白舱、1 青瓷、2・3 はその立体の組）。
    StagePartBuilder(const StageProjection& projection, int kit, uint64_t owner, std::vector<PartRequest>& out)
        : projection_(projection), palette_(kit), owner_(owner), out_(out) {}
    // 意味名ごとに ID の系列を固定する（マスや装置が増減しても別の部品の ID を使い回さない）。
    void Begin(std::string_view key);
    // 180° 回された部屋の中の物を描く時：(cx, cy)（世界のマス）について点対称に置き、部品も 180° 回す。
    // 回されたテレビの画面は絵ごと逆さに写るので、規則の上で正立している物が画面でも正立して見える。
    void Mirror(double cx, double cy) { mirror_ = true; mirrorX_ = cx; mirrorY_ = cy; }
    void NoMirror() { mirror_ = false; }
    // 編集中（全部の部屋を一度に写す）は目盛りの線と、つながった口の鎧戸を出さない（描画物体の上限に収める）。
    void Compact(bool on) { compact_ = on; }
    // 後ろの壁を窓にして、その向こうに大広間を描くか（立体で編集中でない時）。窓のある時は壁の目盛りを出さない。
    void UseHall(bool on) { hall_ = on; }
    // 奥行きの格子：地形を手前の一列の奥へ rows 列延ばす（0 で無し）。後ろの壁・窓・大広間は最後の列の奥へ下がる。
    void DepthRows(int rows) { depthRows_ = rows; }
    // 実体の地形（受光の組を透視で撮る時）：塊を目地の無い箱にし、奥行き 1 マスの列で部屋の奥まで伸ばす。
    // 後ろの壁の窓と大広間も、斜投影でなくてもこの時は描く。
    void Solid(bool on, double playDepth = 0) { solid_ = on; playDepth_ = playDepth; }
    // 人物の見た目の大きさ（判定の矩形の高さに掛ける。足元は動かさない）。
    void ActorScale(double scale) { actorScale_ = scale; }
    // 装置の小さな動き（硬貨の回転、丸鋸、輪の脈、キーの押し、光線の脈）に使う実時間（秒）。編集中は止める。
    void SetTime(double seconds) { time_ = seconds; }
    // 焼いた静止の人物を使うか（骨つきの人形が立つ間は false にして退避点へ置く）。
    void BakedActors(bool on) { bakedActors_ = on; }
    // leanPivot は傾きを釣り合わせる高さ（部品の下辺から、マス）。負なら部品の高さの中ほど。
    bool Add(std::string_view part, double x, double bottomY, double width = 1, double height = 1, double depth = 0,
             HIKARI::GameProjectQuaternion rotation = {}, double leanPivot = -1, bool visible = true);
    // Add と同じ置き方（回された部屋の点対称・投影・傾きの釣り合い）で舞台の位置と向きだけを求める（縮尺は呼び手）。
    // pivot は傾きを釣り合わせる高さ（マス）。
    HIKARI::GameProjectTransform Place(double x, double bottomY, double depth, HIKARI::GameProjectQuaternion rotation, double pivot = 0) const;
    // 1 マスの舞台上の長さと、写し方（斜投影のずれ）。
    double StageScale() const { return projection_.scale; }
    const StageProjection& Projection() const { return projection_; }
    // 背景：闇の板と上から差す光の輪、1 マスの目盛り。(x, y) は部屋の左上の世界のマス。
    // 編集中以外は、後ろの壁を窓の周りの帯にして窓枠を付ける（窓の向こうは Hall が毎フレーム描く）。
    void Backdrop(int x, int y, int cols, int rows, WindowSpan span = {});
    // 窓の向こうの大広間（奥の壁・灯の帯・柱・梁）。eyeX/Y は画面を見る角度の正接（右・上）で、奥の層ほど大きくずらす。
    // 中身は区域 ID から決まる（部屋ごとに柱の位置などが違う）。
    void Hall(std::string_view regionId, int x, int y, int cols, int rows, WindowSpan span, double eyeX, double eyeY);
    // 部屋の窓の上下：既定の窓の高さにかかる操作の札があれば、札の上か下の広い方へ窓を寄せる（札が窓の縁にまたがらず、
    // 壁に掛かって見える）。どちらも狭ければ窓を作らない。
    WindowSpan SpanAvoidingSigns(const std::vector<Rules::DeviceSnapshot>& devices, std::string_view regionId, int roomY, int rows) const;
    // 地形。occupancy は部屋と周り 1 マスの占有（(cols + 2) × (rows + 2)。部屋の無い所は埋まり扱い）。
    void Terrain(int x, int y, int cols, int rows, const std::vector<char>& occupancy);
    // 部屋の外周の口のうち、外に部屋が無い（どこにもつながっていない）所に鎧戸を下ろす。occupancy は Terrain と同じ、
    // linked は周り 1 マスにほかの部屋があるか（同じ並び）。
    void Edges(int x, int y, int cols, int rows, const std::vector<char>& occupancy, const std::vector<char>& linked);
    // 装置一つ（世界のマス）。状態で変わる部品は全部出し、表示だけを切り替える。since は通電・実体が最後に変わってからの
    // 実秒（変わっていなければ大きな値）。扉・橋の出入りと通電灯の点灯をこれで動かす。
    void Device(const Rules::DeviceSnapshot& device, bool checkpoint, double since);
    // 光線一組（棒と当たりの火花）。beam が無い時は発射器の場所で隠す。
    void Beam(const Rules::DeviceSnapshot& laser, const Rules::BeamState* beam);
    // 人物：姿勢と向きのモデルを全部出し、今の一つだけを rect に立たせる（shown=false なら全部退避）。
    // grow は縦の伸び（足元を固定。残影が足元から現れる時に 0→1）。
    void Actor(const Rules::BodyState& body, const Rules::Rect& rect, bool echo, double time, bool shown, double grow = 1.0);
    size_t Failed() const noexcept { return failed_; }

private:
    // 仕掛けの通電灯（点灯・消灯の二つを出して片方だけ見せる）。点いた直後は灯が一度膨らみ、光の粒が散る。
    void Lamp(double x, double bottomY, bool lit, double since, bool world = false);
    // 引っ込んだ仕掛けの輪郭（菫の破線）。部品の数は寸法だけで決まる。
    void Outline(double left, double top, double right, double bottom, bool visible, bool link = false);
    const StageProjection& projection_;
    int palette_ = 0;
    uint64_t owner_ = 0;
    std::vector<PartRequest>& out_;
    uint64_t semantic_ = 1, partIndex_ = 0;
    // 実体の地形の面の目地（Terrain から呼ぶ。並びは Terrain と同じ）。
    void SolidSeams(int x, int y, int cols, int rows, const std::vector<char>& occupancy);
    // 後ろの壁の奥行き（マス）。奥行きの格子の分だけ奥へ下がる。
    double WallDepth() const;
    // 実体の地形の奥行き（マス）と、その正面が玩法の面より手前に出る量（人物は部屋の奥行きの中ほどに立つ）。
    double SolidDepth() const { return depthRows_ + 1.0; }
    double SolidFront() const { return solid_ ? (std::min)(playDepth_, SolidDepth() - .5) : 0.0; }
    // 明るい壁と窓・大広間・口の暗がりを描けるか（斜投影の組か、実体の地形）。
    bool Spatial() const { return projection_.oblique > 0 || solid_; }
    bool mirror_ = false, compact_ = false, hall_ = false, solid_ = false, bakedActors_ = true;
    int depthRows_ = 0;
    // partDepth_ は Add の奥行きの倍率（実体の箱を部屋の奥まで伸ばす間だけ 1 以外）。
    double actorScale_ = 1, partDepth_ = 1, playDepth_ = 0;
    double mirrorX_ = 0, mirrorY_ = 0, time_ = 0;
    size_t failed_ = 0;
};
}
