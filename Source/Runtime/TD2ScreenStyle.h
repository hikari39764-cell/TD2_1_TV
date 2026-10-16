#pragma once
#include "HIKARI/Project/GameComponent.h"
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kScreenStyleComponentType = "TD2.ScreenStyle";

// 電視の中の描き方（表示だけ）。Rules の寸法・衝突・入力と工場全体の後処理は変えない。
// 原版は 3D 描画そのもの（陰影を焼いた ScreenKit07 の色）、像素は 1 格 16 ゲーム画素で塗り直し、素描は墨色の紙に鉛筆。
enum class ScreenStyleMode : int { Original = 0, Pixel = 1, Sketch = 2 };

struct ScreenStyleSettings {
    bool enabled = true;
    int mode = static_cast<int>(ScreenStyleMode::Pixel);
    int palette = 0;              // 0 墨と象牙、1 青緑の蛍光（共有舞台の設定だけが効く。部品の色を焼き分けてある）
    float outline = 1.0f;         // 像素の外側の輪郭（ゲーム画素、0 で無し）
    float shadow = 0.85f;         // 背景の壁へ落ちる影の濃さ（像素。1 で輪郭と同じ闇）
    float sketchDepth = 1.0f;     // 素描の纵深（落ち影・暗い面・接地・空気。0 で平らな草図）
    float brightness = 1.0f;      // 画面の明るさ（区域の屏幕亮度と掛け合わせる）
    // 電視の中のカメラ（共有舞台の設定だけが効く）。俯角で足場の上面、側角で側面が見える。玩法の判定は変えない。
    float pitch = 7.0f;           // 俯角（度、0〜30）
    float yaw = 0.0f;             // 側角（度、-30〜30、正は右から見る）
    // 舞台を撮る正交カメラ（共有舞台の設定だけが効く。窓ごとに別の物体）。主は操作中の部屋を 1 格 32 texel で、
    // 周りは周囲の電視を 16 texel で（正交の高さを主の 3 倍に）、未配置は編集中だけ世界に置いていない地図を撮る。
    uint64_t captureCameraObject = 0;
    uint64_t wideCameraObject = 0;
    uint64_t unplacedCameraObject = 0;
};

class ScreenStyleComponent final : public HIKARI::IGameComponent {
public:
    const ScreenStyleSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Bool("enabled", "显示电视内3D画面", settings_.enabled);
        // 描き方と配色は名前付きの選択肢（Inspector では下拉、場景には名前で保存）。選ぶとその場で変わる（Play 中も可）。
        // 並びは ScreenStyleMode と palette の値の順。保存名は場景に残るので、表示を変えたい時も名前は変えない。
        static constexpr std::string_view kModeNames[]{"原版", "像素", "素描"};
        static constexpr std::string_view kPaletteNames[]{"墨蓝", "青绿"};
        fields.Choice("mode", "风格（原版为不加后处理的3D画面）", settings_.mode, kModeNames);
        fields.Choice("palette", "配色", settings_.palette, kPaletteNames);
        fields.Float("outline", "像素轮廓", settings_.outline, 0, 2);
        fields.Float("shadow", "墙面投影（像素）", settings_.shadow, 0, 1);
        fields.Float("sketchDepth", "素描纵深（0平面 1立体）", settings_.sketchDepth, 0, 1);
        fields.Float("brightness", "画面亮度", settings_.brightness, .5f, 2);
        // 電視の中のカメラ：ドラッグで即時に反映（Play 中も可）。画面だけを変え、関卡の判定は変えない。
        fields.Float("pitch", "镜头俯角（看到顶面）", settings_.pitch, 0, 30);
        fields.Float("yaw", "镜头侧角（正值从右侧看）", settings_.yaw, -30, 30);
        if (fields.IsInspecting() && fields.Button("镜头复位")) { settings_.pitch = 7.0f; settings_.yaw = 0.0f; }
        fields.SceneObject("captureCameraObject", "舞台正交镜头（当前与相邻电视）", settings_.captureCameraObject);
        fields.SceneObject("wideCameraObject", "舞台正交镜头（周围电视，高度为主镜头3倍）", settings_.wideCameraObject);
        fields.SceneObject("unplacedCameraObject", "舞台正交镜头（Play 时第二台清晰电视，高度同主镜头；编辑时未布置的地图）", settings_.unplacedCameraObject);
        fields.Note("挂在舞台镜头上是全部电视的设置；挂在某台电视的区域对象上只改那一台的风格（配色和镜头角度以舞台镜头为准）。"
                    "俯角和侧角同时较大时，竖墙会出现轻微错位。");
    }
private:
    ScreenStyleSettings settings_;
};
} // namespace TD2
