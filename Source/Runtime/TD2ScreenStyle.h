#pragma once
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
    int palette = 0;              // 0 白舱、1 青瓷（共有設定だけが効く）
    float outline = 1.0f;         // ピクセルの外側の輪郭（ゲーム画素、0 で無し）
    float shadow = 0.85f;         // 壁に落ちる影の濃さ（ピクセル用。1 で輪郭と同じ暗さ）
    float sketchDepth = 1.0f;     // 素描の奥行き（0 で平面的）
    float sketchFill = 0.6f;      // 素描で物体の色を紙に乗せる濃さ（0.32 で以前の淡い塗り）
    float brightness = 1.0f;      // 画面の明るさ（区域の明るさと掛け合わせる）
    // 立体（共有設定だけが効く）：斜投影を焼いた部品で、ブロックの天板と右の側面を見せる。正面はマスにぴったり残り、判定は変えない。
    // 入れている間は下の俯角・側角を使わない。
    bool depthView = true;
    // 立体の空間感（後処理。舞台カメラの深度を使い、立体の時だけ効く）。
    float atmosphere = 0.85f;     // 奥の大広間の霧（奥ほど霞む）
    float occlusion = 0.6f;       // 物の周り・入隅の暗がり
    float castLight = 0.6f;       // 物の高さに応じた影（像素・素描の落ち影はこれに置き換わる）
    float inkLine = 0.8f;         // 原版の外形の墨線
    // 後ろの壁の窓と、その向こうの大広間（共有設定だけが効く）。parallax は外のカメラが画面を見る角度に合わせて
    // 奥の層をずらす強さ（1 で角度どおり）。テレビが奥行きのある箱に見える。
    bool hall = true;
    float parallax = 1.0f;
    // 受光（共有設定だけが効き、Play の時だけ）：陰影を焼いていない受光の組に替え、舞台の灯（TD2.StageLight）で照らす。
    bool lit = false;
    // 実体の地形で人物が立つ奥行き（部屋の正面から奥へのマス数）。手前の地形が自然な前景になる。
    float playDepth = 1.5f;
    // 人物（プレイヤーと残影）の見た目の大きさ。判定は変えない。
    float actorScale = 1.35f;
    // 奥行きの格子（共有設定だけが効き、Play の時だけ）：地形を奥へ depthRows 列延ばし、後ろの壁をその奥へ下げる。
    // 判定は手前の一列のまま。
    bool depthGrid = false;
    int depthRows = 4;
    // 透視の窓（共有設定だけが効き、Play の時だけ）：目立つ二台を透視カメラで撮り、外のカメラが画面を見る角度に合わせて
    // 目の位置を動かす。視差の強さは parallax。透視カメラは窓ごとに別の物体（画角で透視の強さが決まる）。
    bool perspective = false;
    uint64_t perspectiveCameraObject = 0;
    uint64_t perspectiveCamera2Object = 0;
    // 画面内のカメラ（共有設定だけが効く）。俯角で上面、側角で側面が見える。判定は変えない。
    float pitch = 7.0f;           // 俯角（度、0〜30）
    float yaw = 0.0f;             // 側角（度、-30〜30、正は右から見る）
    // 舞台を撮る正投影カメラ（窓ごとに別の物体）。主は操作中の部屋、周りは周囲のテレビ、
    // 未配置は編集中だけ未配置の地図を撮る。
    uint64_t captureCameraObject = 0;
    uint64_t wideCameraObject = 0;
    uint64_t unplacedCameraObject = 0;
};

class ScreenStyleComponent final : public HIKARI::IGameComponent {
public:
    const ScreenStyleSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Bool("enabled", "显示电视内3D画面", settings_.enabled);
        // 描き方と配色は名前で保存する選択肢。並びは ScreenStyleMode と palette の値の順。
        // 名前はシーンに保存されるので変えない（配色は画面の中の部品を作り直した時に改名した。前の名前は既定の白舱で読む）。
        static constexpr std::string_view kModeNames[]{"原版", "像素", "素描"};
        static constexpr std::string_view kPaletteNames[]{"白舱", "青瓷"};
        fields.Choice("mode", "风格（原版为不加后处理的3D画面）", settings_.mode, kModeNames);
        fields.Choice("palette", "配色", settings_.palette, kPaletteNames);
        fields.Float("outline", "像素轮廓", settings_.outline, 0, 2);
        fields.Float("shadow", "墙面投影（像素）", settings_.shadow, 0, 1);
        fields.Float("sketchDepth", "素描纵深（0平面 1立体）", settings_.sketchDepth, 0, 1);
        fields.Float("sketchFill", "素描上色浓度（0.32 为以前的淡色）", settings_.sketchFill, 0, 1);
        fields.Float("brightness", "画面亮度", settings_.brightness, .5f, 2);
        fields.Bool("depthView", "立体（方块露出顶面和右侧面）", settings_.depthView);
        fields.Float("atmosphere", "远景雾（窗外大厅越深越淡）", settings_.atmosphere, 0, 1);
        fields.Float("occlusion", "环境遮蔽（物体周围与内角变暗）", settings_.occlusion, 0, 1);
        fields.Float("castLight", "立体投影（按高度投下的影子）", settings_.castLight, 0, 1);
        fields.Float("inkLine", "原版描边", settings_.inkLine, 0, 1);
        fields.Bool("hall", "后墙窗口与窗外大厅", settings_.hall);
        fields.Float("parallax", "视差（随外部镜头角度移动深处）", settings_.parallax, 0, 2);
        fields.Bool("lit", "受光（Play 时部件受舞台灯照明；与透视窗口同开时为实体地形）", settings_.lit);
        fields.Float("actorScale", "角色显示大小（判定不变）", settings_.actorScale, .8f, 2.0f);
        fields.Float("playDepth", "角色所在纵深（格，从房间正面往里，实体地形时）", settings_.playDepth, 0, 4);
        fields.Bool("depthGrid", "纵深格子（Play 时地形向里延伸）", settings_.depthGrid);
        fields.Int("depthRows", "纵深格数", settings_.depthRows, 1, 8);
        fields.Bool("perspective", "透视窗口（Play 时清晰的两台随外部视角真透视）", settings_.perspective);
        fields.SceneObject("perspectiveCameraObject", "透视舞台镜头（主，画角决定透视强度）", settings_.perspectiveCameraObject);
        fields.SceneObject("perspectiveCamera2Object", "透视舞台镜头（副）", settings_.perspectiveCamera2Object);
        // 画面内のカメラ。表示だけを変え、判定は変えない。
        fields.Float("pitch", "镜头俯角（看到顶面）", settings_.pitch, 0, 30);
        fields.Float("yaw", "镜头侧角（正值从右侧看）", settings_.yaw, -30, 30);
        if (fields.IsInspecting() && fields.Button("镜头复位")) { settings_.pitch = 7.0f; settings_.yaw = 0.0f; }
        fields.SceneObject("captureCameraObject", "舞台正交镜头（当前与相邻电视）", settings_.captureCameraObject);
        fields.SceneObject("wideCameraObject", "舞台正交镜头（周围电视，高度为主镜头3倍）", settings_.wideCameraObject);
        fields.SceneObject("unplacedCameraObject", "舞台正交镜头（Play 时第二台清晰电视，高度同主镜头；编辑时未布置的地图）", settings_.unplacedCameraObject);
        fields.Note("挂在舞台镜头上是全部电视的设置；挂在某台电视的区域对象上只改那一台的风格（配色、立体和镜头角度以舞台镜头为准）。"
                    "打开「立体」时不使用俯角和侧角。俯角和侧角同时较大时，竖墙会出现轻微错位。"
                    "远景雾、遮蔽、投影、描边、窗外大厅和视差只在「立体」时生效。"
                    "「纵深格子」「透视窗口」只在 Play 时生效；透视窗口的视差强度用「视差」。");
    }
private:
    ScreenStyleSettings settings_;
};
}
