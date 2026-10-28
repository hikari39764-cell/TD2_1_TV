#include "TD2Ui.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/CameraViewSnapshot.h"
#include <algorithm>
#include <string>

namespace TD2::Ui {
namespace {
// 塗りつぶしは内蔵の一色貼図を使い、起動直後の PNG 読込に暗転を依存させない。
constexpr std::string_view kWhite = "generated://solid/ffffffff";
// 素材のパス（Assets からの相対）。
std::string Part(std::string_view name) { return "UI/TD2/" + std::string(name) + ".png"; }
}

Painter::Painter(HIKARI::WorldView& world, int order) : ui_(world.GetUi()), order_(order), next_(0x7D2E0000ull + static_cast<uint64_t>(order) * 4096) {
    HIKARI::GameProjectCameraViewSnapshot view{};
    if (world.GetCamera().TryGetViewSnapshot(view) && view.uiLogicalWidth > 0 && view.uiLogicalHeight > 0) {
        width_ = static_cast<float>(view.uiLogicalWidth); height_ = static_cast<float>(view.uiLogicalHeight);
    }
}

Painter::Painter(HIKARI::WorldView& world, HIKARI::GameProjectUiCanvasHandle canvas, float width, float height, int order)
    : canvases_(world.GetUiCanvases()), canvas_(canvas), width_(width), height_(height), order_(order),
      next_(0x7D3E0000ull + static_cast<uint64_t>(order) * 4096) {}

void Painter::Image(std::string_view texture, Rect r, Color tint, uint32_t flags) {
    if (r.w <= 0 || r.h <= 0 || alpha_ <= 0)
        return;
    HIKARI::GameProjectUiSpriteRequest request{};
    request.sourceId = next_++;
    request.order = order_++;
    request.referenceWidth = width_;
    request.referenceHeight = height_;
    request.x = r.x;
    request.y = r.y;
    request.width = r.w;
    request.height = r.h;
    request.flags = flags;
    request.tint = {tint.r * gain_, tint.g * gain_, tint.b * gain_, tint.a * alpha_};
    if (canvas_.IsValid())
        (void)canvases_.SubmitSprite(canvas_, texture, request);
    else
        (void)ui_.SubmitSprite(texture, request);
}

void Painter::Fill(Rect r, Color color) {
    // 全画面の黒は、前の視点の画布寸法でなく現在の出力の全面を覆う。画布内の背景は従来の配置を保つ。
    const bool full = !canvas_.IsValid() && r.x == 0 && r.y == 0 && r.w == width_ && r.h == height_;
    Image(kWhite, r, color, full ? HIKARI::kGameProjectUiFillViewport | HIKARI::kGameProjectUiSubmissionModal : 0u);
}

Rect Painter::Window(Rect r, float s, std::string_view title) {
    // 上の帯（左上・伸ばす中・右上）、中の帯（左・地・右）、下の帯（左下・伸ばす下・右下）。帯ごとに端の幅が違う。
    const float top = kWindowTop * s, left = kWindowTopLeft * s, right = kWindowTopRight * s;
    const float side = kWindowSide * s, bottom = kWindowBottom * s, corner = kWindowCorner * s;
    const float middle = r.h - top - bottom;
    // 右下へ落ちる柔らかい影（薄い矩形を少しずつずらして重ねる）。
    for (int i = 1; i <= 4; ++i) {
        const float o = 10 * s * static_cast<float>(i) / 4.f;
        Fill({r.x + o * .6f, r.y + o, r.w, r.h}, {0, 0, 0, .09f});
    }
    Image(Part("UI_Window_TopLeft"), {r.x, r.y, left, top});
    Image(Part("UI_Window_Top"), {r.x + left, r.y, r.w - left - right, top});
    Image(Part("UI_Window_TopRight"), {r.Right() - right, r.y, right, top});
    Image(Part("UI_Window_Left"), {r.x, r.y + top, side, middle});
    Fill({r.x + side, r.y + top, r.w - 2 * side, middle}, kPaper);
    Image(Part("UI_Window_Right"), {r.Right() - side, r.y + top, side, middle});
    Image(Part("UI_Window_BottomLeft"), {r.x, r.Bottom() - bottom, corner, bottom});
    Image(Part("UI_Window_Bottom"), {r.x + corner, r.Bottom() - bottom, r.w - 2 * corner, bottom});
    Image(Part("UI_Window_BottomRight"), {r.Right() - corner, r.Bottom() - bottom, corner, bottom});
    WindowTitle(r, s, title);
    return {r.x + side, r.y + top, r.w - 2 * side, middle};
}

void Painter::WindowTitle(Rect r, float s, std::string_view title) {
    // 題：暗い帯は素材の上から 14〜68 画素。
    if (title.empty()) return;
    Text(title, {r.x + kWindowTopLeft * s, r.y + 14 * s, r.w - (kWindowTopLeft + kWindowTopRight) * s, 54 * s}, 38 * s, kTitleInk);
}

void Painter::Row(Rect r, bool selected) {
    const std::string_view kind = selected ? "UI_RowSelected" : "UI_Row";
    const float cap = 30.f / 73.f * r.h;
    Image(Part(std::string(kind) + "_Left"), {r.x, r.y, cap, r.h});
    Image(Part(std::string(kind) + "_Middle"), {r.x + cap, r.y, r.w - 2 * cap, r.h});
    Image(Part(std::string(kind) + "_Right"), {r.Right() - cap, r.y, cap, r.h});
}

void Painter::Button(Rect r, std::string_view label, State state, float textSize) {
    Image(Part(state == State::Selected ? "UI_Button_Selected" : state == State::Pressed ? "UI_Button_Pressed" : "UI_Button_Idle"), r);
    // 押した時は字も少し下へ沈める。
    const float sink = state == State::Pressed ? r.h * .04f : 0;
    Text(label, {r.x, r.y + sink, r.w, r.h}, textSize, kInk, Align::Center);
}

void Painter::Cursor(float x, float y, float height) {
    // 素材の矢印の先は左上から (4, 2) 画素。
    const float k = height / 88.f;
    Image(Part("UI_Cursor"), {x - 4 * k, y - 2 * k, 54 * k, height});
}

void Painter::Text(std::string_view text, Rect r, float size, Color color, Align align) {
    if (text.empty() || alpha_ <= 0) return;
    HIKARI::GameProjectUiTextRequest request{};
    request.sourceId = next_++; request.order = order_++;
    request.referenceWidth = width_; request.referenceHeight = height_;
    request.x = r.x; request.y = r.y; request.maxWidth = (std::max)(1.f, r.w); request.maxHeight = (std::max)(1.f, r.h);
    request.pixelHeight = size; request.lineHeightMultiplier = 1.1f;
    request.textColor = {color.r * gain_, color.g * gain_, color.b * gain_, color.a * alpha_};
    // 影は付けない。縁取りは頼まれた時だけ：暗い字は同じ色合いの更に暗い縁（線を太らせる）、明るい字は黒い縁。
    request.shadowColor = {0, 0, 0, 0}; request.shadowOffsetX = request.shadowOffsetY = 0;
    request.outlineWidth = outline_ > 0 ? (std::max)(1.f, size * outline_) : 0.f;
    const bool light = color.r * .3f + color.g * .59f + color.b * .11f > .5f;
    const float k = light ? .1f : .55f;
    request.outlineColor = light ? HIKARI::GameProjectUiColor{.03f, .03f, .04f, color.a * alpha_}
                                 : HIKARI::GameProjectUiColor{color.r * k * gain_, color.g * k * gain_, color.b * k * gain_, color.a * alpha_};
    request.horizontalAlign = align == Align::Center ? HIKARI::GameProjectUiTextAlign::Center
                            : align == Align::Right ? HIKARI::GameProjectUiTextAlign::Right : HIKARI::GameProjectUiTextAlign::Left;
    request.verticalAlign = HIKARI::GameProjectUiVerticalAlign::Center;
    request.wrap = HIKARI::GameProjectUiTextWrap::None;
    if (canvas_.IsValid()) (void)canvases_.SubmitText(canvas_, kFont, text, request);
    else (void)ui_.SubmitText(kFont, text, request);
}

void Painter::Meter(Rect r, int lit, int count) {
    // 灯の素材は 38×41。間を灯の幅の 0.3 倍あける。
    const float h = r.h, w = h * 38.f / 41.f, gap = w * .3f;
    for (int i = 0; i < count; ++i) {
        Image(Part(i < lit ? "UI_Led_On" : "UI_Led_Off"), {r.x + i * (w + gap), r.y, w, h});
    }
}
}
