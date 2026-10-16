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
    int palette = 0;              // 0 墨と象牙、1 青緑（共有設定だけが効く）
    float outline = 1.0f;         // ピクセルの外側の輪郭（ゲーム画素、0 で無し）
    float shadow = 0.85f;         // 壁に落ちる影の濃さ（ピクセル用。1 で輪郭と同じ暗さ）
    float sketchDepth = 1.0f;     // 素描の奥行き（0 で平面的）
    float brightness = 1.0f;      // 画面の明るさ（区域の明るさと掛け合わせる）
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
        // 名前はシーンに保存されるので変えない。
        static constexpr std::string_view kModeNames[]{"原版", "像素", "素描"};
        static constexpr std::string_view kPaletteNames[]{"墨蓝", "青绿"};
        fields.Choice("mode", "风格（原版为不加后处理的3D画面）", settings_.mode, kModeNames);
        fields.Choice("palette", "配色", settings_.palette, kPaletteNames);
        fields.Float("outline", "像素轮廓", settings_.outline, 0, 2);
        fields.Float("shadow", "墙面投影（像素）", settings_.shadow, 0, 1);
        fields.Float("sketchDepth", "素描纵深（0平面 1立体）", settings_.sketchDepth, 0, 1);
        fields.Float("brightness", "画面亮度", settings_.brightness, .5f, 2);
        // 画面内のカメラ。表示だけを変え、判定は変えない。
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
}
