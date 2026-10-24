#pragma once
#include "TD2ScreenTone.h"
#include "HIKARI/Project/GameComponent.h"
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kScreenStyleComponentType = "TD2.ScreenStyle";

// 画面内の描き方（表示だけ）。ルールや工場の後処理は変えない。
// オリジナルは 3D 描画そのまま、ピクセルは 1 マス 16 画素で塗り直し、素描は暗い紙に鉛筆で描く。
enum class ScreenStyleMode : int { Original = 0, Pixel = 1, Sketch = 2 };

struct ScreenStyleSettings {
    bool enabled = true;
    int mode = static_cast<int>(ScreenStyleMode::Pixel);
    float outline = 1.0f;         // ピクセルの外側の輪郭（ゲーム画素、0 で無し）
    float shadow = 0.85f;         // 壁に落ちる影の濃さ（ピクセル用。1 で輪郭と同じ暗さ）
    float sketchDepth = 1.0f;     // 素描の奥行き（0 で平面的）
    float sketchFill = 0.6f;      // 素描で物体の色を紙に乗せる濃さ（0.32 で以前の淡い塗り）
    float brightness = 1.0f;      // 画面の明るさ（区域の明るさと掛け合わせる）
    // 画面の中は青瓷の立体の組（斜投影を焼いた部品で、ブロックの天板と右の側面が見える）。正面はマスにぴったり残り、判定は変えない。
    // 立体の空間感（後処理。舞台カメラの深度を使う）。
    float atmosphere = 0.85f;     // 奥の大広間の霧（奥ほど霞む）
    float occlusion = 0.6f;       // 物の周り・入隅の暗がり
    float castLight = 0.6f;       // 物の高さに応じた影（像素・素描の落ち影はこれに置き換わる）
    float inkLine = 0.8f;         // 原版の外形の墨線
    // 後ろの壁の窓と、その向こうの大広間（共有設定だけが効く）。parallax は外のカメラが画面を見る角度に合わせて
    // 奥の層をずらす強さ（1 で角度どおり）。テレビが奥行きのある箱に見える。
    bool hall = true;
    float parallax = 1.0f;
    // 受光（共有設定だけが効き、Play で透視の窓と一緒の時だけ）：陰影を焼いていない実体の地形の組に替える。
    bool lit = false;
    // 舞台の灯（TD2.StageLight）で受光の組を照らすか。写る部屋が替わるたびに灯が部屋を移り、明るさが跳ねるので既定は切る。
    bool stageLights = false;
    // Play で遠く小さな画面と画の外の画面を毎フレーム描き直さない（前の絵を使う）。切ると全部の画面を毎フレーム描く。
    bool thinScreens = true;
    // 実体の地形で人物が立つ奥行き（部屋の正面から奥へのマス数）。手前の地形が自然な前景になる。
    float playDepth = 1.5f;
    // 人物（プレイヤーと残影）の見た目の大きさ。判定は変えない。
    float actorScale = 1.35f;
    // 奥行きの格子（共有設定だけが効き、Play の時だけ）：地形を奥へ depthRows 列延ばし、後ろの壁をその奥へ下げる。
    // 判定は手前の一列のまま。
    bool depthGrid = false;
    int depthRows = 4;
    // 透視の窓（共有設定だけが効き、Play の時だけ）：操作中の部屋（主）と同じカメラの大区域の残りの部屋（副）を透視カメラで撮り、
    // 外のカメラが画面を見る角度に合わせて目の位置を動かす。視差の強さは parallax。透視カメラは窓ごとに別の物体（画角で透視の強さが決まる）。
    bool perspective = false;
    uint64_t perspectiveCameraObject = 0;
    uint64_t perspectiveCamera2Object = 0;
    // 遠くのテレビ（共有設定だけが効き、Play の時だけ）：ほかの窓に入らない写っているテレビを地図で近いものどうしの二組に分けて
    // 撮る、画角の狭い透視カメラ（組ごとに一台）。写すテレビの範囲に合わせて近づけたり離したりする（キャプチャの大きさは変えない）。
    uint64_t farCameraObject = 0, farCamera2Object = 0;
    // 編集中に舞台を撮る正投影カメラ（詰めて並べた地図の二組。二つ目は未配置の地図）。主の高さが舞台の縮尺を決める。
    uint64_t captureCameraObject = 0;
    uint64_t unplacedCameraObject = 0;
    // 色調（TD2.ScreenTone から写す。この部品の欄ではない）。
    ScreenToneSettings tone;
};

class ScreenStyleComponent final : public HIKARI::IGameComponent {
public:
    const ScreenStyleSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Bool("enabled", "テレビの 3D 画面", settings_.enabled);
        // 描き方は名前で保存する選択肢。並びは ScreenStyleMode の値の順。名前はシーンに保存されるので変えない。
        static constexpr std::string_view kModeNames[]{"標準", "ピクセル", "スケッチ"};
        fields.Choice("mode", "描き方", settings_.mode, kModeNames);
        fields.Float("outline", "ピクセルの輪郭", settings_.outline, 0, 2);
        fields.Float("shadow", "壁の影", settings_.shadow, 0, 1);
        fields.Float("sketchDepth", "スケッチの奥行き", settings_.sketchDepth, 0, 1);
        fields.Float("sketchFill", "スケッチの塗り", settings_.sketchFill, 0, 1);
        fields.Float("brightness", "明るさ", settings_.brightness, .5f, 2);
        fields.Float("atmosphere", "遠くの霧", settings_.atmosphere, 0, 1);
        fields.Float("occlusion", "環境遮蔽", settings_.occlusion, 0, 1);
        fields.Float("castLight", "立体の影", settings_.castLight, 0, 1);
        fields.Float("inkLine", "標準の輪郭線", settings_.inkLine, 0, 1);
        fields.Bool("hall", "奥の窓と大広間", settings_.hall);
        fields.Float("parallax", "視差", settings_.parallax, 0, 2);
        fields.Bool("lit", "受光", settings_.lit);
        fields.Bool("stageLights", "舞台の灯", settings_.stageLights);
        fields.Bool("thinScreens", "遠い画面の間引き", settings_.thinScreens);
        fields.Float("actorScale", "人物の大きさ", settings_.actorScale, .8f, 2.0f);
        fields.Float("playDepth", "人物の奥行き", settings_.playDepth, 0, 4);
        fields.Bool("depthGrid", "奥行きのマス", settings_.depthGrid);
        fields.Int("depthRows", "奥行きの列数", settings_.depthRows, 1, 8);
        fields.Bool("perspective", "透視の窓", settings_.perspective);
        fields.SceneObject("perspectiveCameraObject", "鮮明の透視カメラ", settings_.perspectiveCameraObject);
        fields.SceneObject("perspectiveCamera2Object", "大区域の透視カメラ", settings_.perspectiveCamera2Object);
        fields.SceneObject("farCameraObject", "遠くのテレビのカメラ 1", settings_.farCameraObject);
        fields.SceneObject("farCamera2Object", "遠くのテレビのカメラ 2", settings_.farCamera2Object);
        fields.SceneObject("captureCameraObject", "編集用の正投影カメラ", settings_.captureCameraObject);
        fields.SceneObject("unplacedCameraObject", "編集用の正投影カメラ 2", settings_.unplacedCameraObject);
    }
private:
    ScreenStyleSettings settings_;
};
}
