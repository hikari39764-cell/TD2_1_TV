#pragma once
#include "TD2DeviceEditor.h"
#include <algorithm>
#include <cmath>

namespace TD2::Editor {

// 表示座標だけを変換する。ズームとパンは配置を変えない。広い世界地図も一画面に収まるよう、1 マス 0.25 画素まで縮める。
inline constexpr float kMinTile = .25f;
struct MapCanvasView {
    EditorWorkspacePoint origin{24, 36};
    float tile = 28;
    bool fit = true, panning = false, hasFit = false;
    EditorWorkspacePoint lastSize{};
    EditorWorkspacePoint panPointer{}, panOrigin{};
    EditorWorkspacePoint Screen(float x, float y) const { return {origin.x + x * tile, origin.y + y * tile}; }
    EditorWorkspacePoint World(EditorWorkspacePoint p) const { return {(p.x - origin.x) / tile, (p.y - origin.y) / tile}; }
    // ドッキング直後は幅が変わるので、新しい幅で合わせ直す。
    void ObserveSize(EditorWorkspacePoint size) {
        if (size.x <= 0 || size.y <= 0) { return; }
        if (lastSize.x <= 0 || lastSize.y <= 0 || std::abs(size.x - lastSize.x) > .5f || std::abs(size.y - lastSize.y) > .5f) { fit = true; }
        lastSize = size;
    }
    void Fit(EditorWorkspacePoint size, float x, float y, float w, float h) {
        tile = std::clamp((std::min)((size.x - 48) / (std::max)(1.0f, w), (size.y - 48) / (std::max)(1.0f, h)), kMinTile, 72.0f);
        origin = {(size.x - w * tile) * .5f - x * tile, (size.y - h * tile) * .5f - y * tile}; fit = false; hasFit = true;
    }
    void Navigate(const EditorWorkspaceCanvasInput& input, bool hand, bool busy) {
        constexpr uint32_t middle = static_cast<uint32_t>(EditorWorkspacePointerButton::Middle);
        constexpr uint32_t primary = static_cast<uint32_t>(EditorWorkspacePointerButton::Primary);
        const uint32_t panButtons = middle | (hand ? primary : 0u);
        if (!busy && input.hovered && (input.pressedButtons & panButtons)) {
            panning = true; panPointer = input.pointer; panOrigin = origin;
        }
        if (panning) {
            origin = {panOrigin.x + input.pointer.x - panPointer.x, panOrigin.y + input.pointer.y - panPointer.y};
            if ((input.releasedButtons & panButtons) || !(input.heldButtons & panButtons)) { panning = false; }
        }
        if (!busy && input.hovered && input.wheel != 0) {
            const auto p = World(input.pointer);
            tile = std::clamp(tile * std::exp(input.wheel * .12f), kMinTile, 96.0f);
            origin = {input.pointer.x - p.x * tile, input.pointer.y - p.y * tile};
        }
    }
};

inline bool PointIn(EditorWorkspacePoint p, EditorWorkspacePoint a, EditorWorkspacePoint b) {
    return p.x >= a.x && p.x < b.x && p.y >= a.y && p.y < b.y;
}

// 背景のマスはキャンバスの範囲だけ描く。
inline void DrawCanvasGrid(IEditorWorkspaceUi& ui, const MapCanvasView& v, EditorWorkspacePoint size) {
    ui.CanvasFill({0, 0}, size, {.045f, .07f, .095f, 1});
    // 線の間が 12 画素以上になる刻み（1・4・16・64 マス）。縮めすぎた時は引かない。
    float spacing = 0;
    for (const float step : {1.0f, 4.0f, 16.0f, 64.0f}) { if (v.tile * step >= 12) { spacing = v.tile * step; break; } }
    if (spacing <= 0) { return; }
    for (float x = std::fmod(v.origin.x, spacing); x < size.x; x += spacing) { ui.CanvasLine({x, 0}, {x, size.y}, {.12f, .17f, .20f, 1}); }
    for (float y = std::fmod(v.origin.y, spacing); y < size.y; y += spacing) { ui.CanvasLine({0, y}, {size.x, y}, {.12f, .17f, .20f, 1}); }
}

inline EditorWorkspaceColor DeviceColor(std::string_view kind) {
    if (kind == "Laser" || kind == "Spikes") { return {.83f, .37f, .42f, 1}; }
    if (kind == "Checkpoint") { return {.42f, .85f, .73f, 1}; }
    if (kind == "Plate" || kind == "Door" || kind == "Bridge") { return {.84f, .69f, .38f, 1}; }
    if (kind == "Spring") { return {.96f, .72f, .24f, 1}; }
    if (kind == "Collectible") { return {.94f, .42f, .66f, 1}; }
    if (kind == "Decal") { return {.55f, .6f, .62f, 1}; }
    return {.48f, .65f, .71f, 1};
}

// 記号は公開の描画 API だけで描く。ゲームの 3D 表示には依存しない。
// 薄い圧力板や足場は選択範囲に合わせて 1 マスで描く。物理矩形は変えない。
inline void DrawDeviceGlyph(IEditorWorkspaceUi& ui, const DeviceDraft& d, const MapCanvasView& v) {
    auto a = v.Screen(static_cast<float>(d.x), static_cast<float>(d.y));
    auto b = v.Screen(static_cast<float>(d.x + d.w), static_cast<float>(d.y + d.h));
    if (d.kind == "OneWay") { b.y = a.y + v.tile; }
    if (d.kind == "Plate" || d.kind == "Checkpoint") { a.y = b.y - v.tile; }
    const auto c = DeviceColor(d.kind);
    const auto p = [&](float x, float y) { return EditorWorkspacePoint{a.x + (b.x - a.x) * x, a.y + (b.y - a.y) * y}; };
    const float stroke = std::clamp(v.tile * .055f, 1.0f, 2.5f);
    const float unit = (std::min)({v.tile, b.x - a.x, b.y - a.y});
    const auto line = [&](float x1, float y1, float x2, float y2) { ui.CanvasLine(p(x1, y1), p(x2, y2), c, stroke); };
    if (d.kind == "OneWay") {
        ui.CanvasFill(p(0, 0), p(1, .13f), c);
        line(.42f, .57f, .5f, .35f); line(.5f, .35f, .58f, .57f); line(.5f, .35f, .5f, .85f);
    } else if (d.kind == "Plate") {
        ui.CanvasFill(p(0, .83f), p(1, 1), {c.r * .45f, c.g * .45f, c.b * .45f, 1});
        ui.CanvasFill(p(.1f, .64f), p(.9f, .8f), c);
        line(.5f, .12f, .5f, .46f); line(.39f, .34f, .5f, .46f); line(.5f, .46f, .61f, .34f);
    } else if (d.kind == "Checkpoint") {
        line(.15f, .93f, .85f, .93f); line(.3f, .93f, .3f, .08f);
        ui.CanvasFill(p(.3f, .08f), p(.82f, .37f), c);
    } else if (d.kind == "Spikes") {
        // 棘の向きは dir に合わせる。端の半マスも切り捨てない。
        const bool vertical = d.direction == "Left" || d.direction == "Right";
        const int teeth = std::clamp(static_cast<int>(std::ceil((vertical ? d.h : d.w) * 2)), 1, 128);
        const auto oriented = [&](float along, float depth) {
            if (d.direction == "Down") { return p(along, 1 - depth); }
            if (d.direction == "Left") { return p(depth, along); }
            if (d.direction == "Right") { return p(1 - depth, along); }
            return p(along, depth);
        };
        ui.CanvasFill(a, b, {c.r * .2f, c.g * .2f, c.b * .2f, 1});
        for (int i = 0; i < teeth; ++i) {
            ui.CanvasLine(oriented(static_cast<float>(i) / teeth, .95f), oriented((i + .5f) / teeth, .08f), c, stroke);
            ui.CanvasLine(oriented((i + .5f) / teeth, .08f), oriented((i + 1.0f) / teeth, .95f), c, stroke);
        }
    } else if (d.kind == "Laser") {
        const auto center = p(.5f, .5f);
        const EditorWorkspacePoint dir = d.direction == "Left" ? EditorWorkspacePoint{-1, 0} : d.direction == "Right" ? EditorWorkspacePoint{1, 0} : d.direction == "Down" ? EditorWorkspacePoint{0, 1} : EditorWorkspacePoint{0, -1};
        const auto tip = p(.5f + .42f * dir.x, .5f + .42f * dir.y);
        ui.CanvasCircle(center, unit * .19f, c, stroke);
        ui.CanvasLine({center.x + dir.x * unit * .19f, center.y + dir.y * unit * .19f}, tip, c, stroke);
        for (const float side : {-1.0f, 1.0f}) { ui.CanvasLine(tip, {tip.x - dir.x * unit * .16f - dir.y * side * unit * .12f, tip.y - dir.y * unit * .16f + dir.x * side * unit * .12f}, c, stroke); }
    } else {
        ui.CanvasFill(a, b, {c.r * .3f, c.g * .3f, c.b * .3f, 1});
        ui.CanvasRect(a, b, c, stroke);
        if (d.kind == "Solid") { line(.12f, .12f, .88f, .12f); }
        else if (d.kind == "Door") {
            line(.5f, .05f, .5f, .95f); ui.CanvasCircle(p(.65f, .54f), unit * .06f, c, stroke);
        } else if (d.kind == "LockWall") {
            const auto center = p(.5f, .5f);
            ui.CanvasCircle({center.x, center.y - unit * .11f}, unit * .16f, c, stroke);
            ui.CanvasFill({center.x - unit * .23f, center.y - unit * .05f}, {center.x + unit * .23f, center.y + unit * .25f}, c);
            ui.CanvasLine({center.x, center.y + unit * .02f}, {center.x, center.y + unit * .17f}, {.1f, .18f, .23f, 1}, stroke);
        }
    }
}

inline void DrawRoomMap(IEditorWorkspaceUi& ui, const RegionSettings& s, const MapCanvasView& v, bool grid, bool links, bool spawnFigure = true) {
    const auto valid = ValidateRegion(s.region);
    if (!valid.valid || s.region.cells.size() > 4096) { return; }
    ui.CanvasFill(v.Screen(0, 0), v.Screen(static_cast<float>(s.region.cols), static_cast<float>(s.region.rows)), {.075f, .12f, .15f, 1});
    for (int y = 0; y < s.region.rows; ++y) for (int x = 0; x < s.region.cols; ++x) {
        const auto a = v.Screen(static_cast<float>(x), static_cast<float>(y));
        const auto b = v.Screen(static_cast<float>(x + 1), static_cast<float>(y + 1));
        if (s.region.cells[CellIndex(s.region, x, y)] == '1') {
            const auto empty = [&s](int cx, int cy) { return cx < 0 || cy < 0 || cx >= s.region.cols || cy >= s.region.rows || s.region.cells[CellIndex(s.region, cx, cy)] == '0'; };
            const bool top = empty(x, y - 1), bottom = empty(x, y + 1), left = empty(x - 1, y), right = empty(x + 1, y);
            ui.CanvasFill(a, b, {.25f, .42f, .46f, 1});
            if (grid && v.tile > 10) { ui.CanvasRect(a, b, {.20f, .35f, .39f, 1}, .5f); }
            if (top) { ui.CanvasLine(a, {b.x, a.y}, {.63f, .84f, .77f, 1}, 2); }
            if (left) { ui.CanvasLine(a, {a.x, b.y}, {.43f, .63f, .62f, 1}); }
            if (right) { ui.CanvasLine({b.x, a.y}, b, {.43f, .63f, .62f, 1}); }
            if (bottom) { ui.CanvasLine({a.x, b.y}, b, {.43f, .63f, .62f, 1}); }
        } else if (grid && v.tile > 10) { ui.CanvasRect(a, b, {.15f, .22f, .25f, .65f}, .5f); }
    }
    DeviceEditor devices; devices.Load(s.devices);
    if (links) for (const auto& d : devices.Items()) for (const auto& id : d.targets) {
        const auto target = std::find_if(devices.Items().begin(), devices.Items().end(), [&id](const auto& t) { return t.id == id; });
        if (target != devices.Items().end()) {
            ui.CanvasLine(v.Screen(static_cast<float>(d.x + d.w * .5), static_cast<float>(d.y + d.h * .5)),
                          v.Screen(static_cast<float>(target->x + target->w * .5), static_cast<float>(target->y + target->h * .5)), {.78f, .65f, .35f, .55f}, 2);
        }
    }
    for (const auto& d : devices.Items()) { DrawDeviceGlyph(ui, d, v); }
    if (!spawnFigure) { return; }
    // スポーン位置は人の記号で、.625 × .875 マスの矩形に収める。
    const auto spawn = [&](float x, float y) { return v.Screen(s.spawnX + x, s.spawnY + y); };
    constexpr EditorWorkspaceColor player{.88f, .94f, .98f, 1};
    const float stroke = std::clamp(v.tile * .07f, 1.0f, 3.0f);
    ui.CanvasCircle(spawn(.3125f, .16f), v.tile * .12f, player, stroke);
    ui.CanvasFill(spawn(.2f, .33f), spawn(.425f, .61f), player);
    ui.CanvasLine(spawn(.2f, .37f), spawn(.08f, .57f), player, stroke);
    ui.CanvasLine(spawn(.425f, .37f), spawn(.545f, .57f), player, stroke);
    ui.CanvasLine(spawn(.25f, .61f), spawn(.17f, .84f), player, stroke);
    ui.CanvasLine(spawn(.375f, .61f), spawn(.455f, .84f), player, stroke);
}

}
