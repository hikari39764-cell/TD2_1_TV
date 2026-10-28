#pragma once
// 古いパソコンの窓の部品（Assets/UI/TD2 に切り出した素材）で UI を描く。描く先は二つ：
// ・画の上（UiView）：後処理の後に重なる。全画面の問いと暗転に使う。
// ・テレビの画面の画布（UiCanvasView）：画布をテレビの画面の材質へ結び、画面の絵そのものとして 3D で写す。
//   画布の中身は画面の中の描き方（線画・色調）を受けない。
// 座標は描く先の画素（左上が原点）。
#include "HIKARI/Project/UiCanvasView.h"
#include "HIKARI/Project/UiView.h"
#include "HIKARI/Project/WorldView.h"
#include <string_view>

namespace TD2::Ui {
inline constexpr std::string_view kFont = "Fonts/Makinas-4-Flat.otf";

// UTF-8 の字の数（改行も一字）と、頭から count 字。打ち出しの途中の字を数えるのに使う。
inline size_t CharCount(std::string_view text) {
    size_t n = 0;
    for (const char c : text) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}
inline std::string_view CharHead(std::string_view text, size_t count) {
    size_t at = 0;
    for (size_t n = 0; at < text.size(); ++at) {
        if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80 && n++ == count) break;
    }
    return text.substr(0, at);
}

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float Right() const noexcept { return x + w; }
    float Bottom() const noexcept { return y + h; }
};
struct Color { float r = 1, g = 1, b = 1, a = 1; };
inline constexpr Color kInk{.11f, .12f, .14f, 1};          // 明るい地の文字
inline constexpr Color kFaint{.42f, .45f, .48f, 1};        // 添え書き
inline constexpr Color kPaper{.835f, .843f, .847f, 1};     // 窓の中の地（素材の 213,215,216）
inline constexpr Color kTitleInk{.90f, .92f, .93f, 1};     // 暗い題の帯の文字

enum class Align { Left, Center, Right };
enum class State { Normal, Selected, Pressed };

// 一フレームの描き手。order が大きいほど上に重なる。alpha で全部を薄くする（出る・消える時）。
class Painter {
public:
    // 画の上へ描く。
    Painter(HIKARI::WorldView& world, int order);
    // 画布へ描く（呼び手が画布の更新を始めてから終えるまでの間だけ使う）。width・height は画布の大きさ。
    Painter(HIKARI::WorldView& world, HIKARI::GameProjectUiCanvasHandle canvas, float width, float height, int order);
    float Width() const noexcept { return width_; }
    float Height() const noexcept { return height_; }
    void SetAlpha(float alpha) noexcept { alpha_ = alpha; }
    // 色の明るさを一律に掛ける（テレビの画面は材質が光らせるので、画布へ描く時は暗めに渡す）。
    void SetGain(float gain) noexcept { gain_ = gain; }
    // 字に描き縁を付ける（字の高さに対する割合。0 で無し）。光る画面で細い線が飛ばないよう太らせる。
    void SetTextOutline(float ratio) noexcept { outline_ = ratio; }

    void Image(std::string_view texture, Rect r, Color tint = {}, uint32_t flags = 0);
    void Fill(Rect r, Color color);
    // 窓。s は素材から描く先への倍率（題の帯の高さが 86·s）。題は暗い帯の左に明るい字で。窓の中の矩形を返す。
    Rect Window(Rect r, float s, std::string_view title);
    // 窓 r の題だけ（開く途中の窓では、枠だけを動かして題は開ききった窓の位置に描く）。
    void WindowTitle(Rect r, float s, std::string_view title);
    // 選べる行（高さに合わせて両端を縮める）。
    void Row(Rect r, bool selected);
    // 字の入ったボタン（素材のボタンをそのまま伸ばす）。
    void Button(Rect r, std::string_view label, State state, float textSize);
    // 矢印のカーソル。先が (x, y)、height は矢印の高さ。
    void Cursor(float x, float y, float height);
    // 一行の字（折り返さない）。縦は矩形の中央。
    void Text(std::string_view text, Rect r, float size, Color color, Align align = Align::Left);
    // 音量の目盛り：灯を count 個並べ、lit 個まで点ける。
    void Meter(Rect r, int lit, int count);

private:
    HIKARI::UiView ui_;
    HIKARI::UiCanvasView canvases_;
    HIKARI::GameProjectUiCanvasHandle canvas_{};
    float width_ = 1920, height_ = 1080, alpha_ = 1, gain_ = 1, outline_ = 0;
    int order_ = 0;
    uint64_t next_ = 0;
};

// 素材の大きさ（切り出しの画素）。配置の計算に使う。
inline constexpr float kWindowTop = 86, kWindowTopLeft = 90, kWindowTopRight = 185, kWindowSide = 22, kWindowBottom = 33, kWindowCorner = 30;
// 窓 r（倍率 s）の中の矩形。Painter::Window が返すのと同じ（描かずに配置だけを求める）。
inline Rect WindowInner(Rect r, float s) {
    return {r.x + kWindowSide * s, r.y + kWindowTop * s, r.w - 2 * kWindowSide * s, r.h - kWindowTop * s - kWindowBottom * s};
}
inline constexpr float kButtonAspect = 276.f / 115.f, kCursorAspect = 54.f / 88.f, kMailFieldsAspect = 517.f / 173.f;
}
