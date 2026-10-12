#pragma once
#include "TD2DeviceEditor.h"
#include "TD2MapAssets.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace TD2::Editor {

// 表示座標だけを変換する。ズームと pan は配置・ルール座標を書き換えない。
struct MapCanvasView {
    EditorWorkspacePoint origin{24, 36};
    float tile = 28;
    bool fit = true, panning = false, hasFit = false;
    EditorWorkspacePoint lastSize{};
    EditorWorkspacePoint panPointer{}, panOrigin{};
    EditorWorkspacePoint Screen(float x, float y) const { return {origin.x + x * tile, origin.y + y * tile}; }
    EditorWorkspacePoint World(EditorWorkspacePoint p) const { return {(p.x - origin.x) / tile, (p.y - origin.y) / tile}; }
    // 初回 dock の確定前後で Canvas 幅が変わる。新しい実寸で再 fit し、部屋の見切れを防ぐ。
    void ObserveSize(EditorWorkspacePoint size) {
        if (size.x <= 0 || size.y <= 0) { return; }
        if (lastSize.x <= 0 || lastSize.y <= 0 || std::abs(size.x - lastSize.x) > .5f || std::abs(size.y - lastSize.y) > .5f) { fit = true; }
        lastSize = size;
    }
    void Fit(EditorWorkspacePoint size, float x, float y, float w, float h) {
        tile = std::clamp((std::min)((size.x - 72) / (std::max)(1.0f, w), (size.y - 72) / (std::max)(1.0f, h)), 2.0f, 72.0f);
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
            tile = std::clamp(tile * std::exp(input.wheel * .12f), 2.0f, 96.0f);
            origin = {input.pointer.x - p.x * tile, input.pointer.y - p.y * tile};
        }
    }
};

inline bool PointIn(EditorWorkspacePoint p, EditorWorkspacePoint a, EditorWorkspacePoint b) {
    return p.x >= a.x && p.x < b.x && p.y >= a.y && p.y < b.y;
}

// 背景格は Canvas の範囲だけ描く。極端に離れた世界原点でも反復数を増やさない。
inline void DrawCanvasGrid(IEditorWorkspaceUi& ui, const MapCanvasView& v, EditorWorkspacePoint size) {
    ui.CanvasFill({0, 0}, size, {.045f, .07f, .095f, 1});
    const float spacing = v.tile * (v.tile < 8 ? 4.0f : 1.0f);
    for (float x = std::fmod(v.origin.x, spacing); x < size.x; x += spacing) { ui.CanvasLine({x, 0}, {x, size.y}, {.12f, .17f, .20f, 1}); }
    for (float y = std::fmod(v.origin.y, spacing); y < size.y; y += spacing) { ui.CanvasLine({0, y}, {size.x, y}, {.12f, .17f, .20f, 1}); }
}

inline EditorWorkspaceColor DeviceColor(std::string_view kind) {
    if (kind == "Laser" || kind == "Spikes") { return {.83f, .37f, .42f, 1}; }
    if (kind == "Checkpoint") { return {.42f, .85f, .73f, 1}; }
    if (kind == "Plate" || kind == "Door") { return {.84f, .69f, .38f, 1}; }
    return {.48f, .65f, .71f, 1};
}

// スプライト表示は作者矩形と独立。薄い圧板・足場は一格高で底／上端に合わせる。
inline void DrawDeviceGlyph(IEditorWorkspaceUi& ui, const DeviceDraft& d, const MapCanvasView& v) {
    auto a = v.Screen(static_cast<float>(d.x), static_cast<float>(d.y));
    auto b = v.Screen(static_cast<float>(d.x + d.w), static_cast<float>(d.y + d.h));
    std::string key;
    if (d.kind == "Solid") { key = "terrain.fill"; }
    else if (d.kind == "OneWay") { key = "platform.middle"; b.y = a.y + v.tile; }
    else if (d.kind == "Plate") { key = "plate.up"; a.y = b.y - v.tile; }
    else if (d.kind == "Door") { key = "door.closed"; }
    else if (d.kind == "Checkpoint") { key = "checkpoint.idle"; a.y = b.y - v.tile; }
    else if (d.kind == "LockWall") { key = "lock.closed"; }
    else {
        std::string direction = d.direction;
        std::transform(direction.begin(), direction.end(), direction.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (d.kind == "Spikes") { key = "spikes." + direction; }
        if (d.kind == "Laser") { key = "laser." + direction + ".on"; }
    }
    if (gMapSprites.Draw(ui, key, a, b)) { return; }
    const auto c = DeviceColor(d.kind);
    if (d.kind == "OneWay" || d.kind == "Plate") {
        ui.CanvasFill({a.x, b.y - v.tile * .18f}, b, c);
        ui.CanvasLine({a.x, b.y - v.tile * .24f}, {b.x, b.y - v.tile * .24f}, {c.r, c.g, c.b, .4f}, 2);
    } else {
        ui.CanvasFill(a, b, {c.r * .3f, c.g * .3f, c.b * .3f, 1});
        ui.CanvasRect(a, b, c, 2);
        if (d.kind == "Spikes") {
            for (float x = a.x; x < b.x; x += v.tile * .5f) {
                ui.CanvasLine({x, b.y}, {x + v.tile * .25f, a.y}, c, 2);
                ui.CanvasLine({x + v.tile * .25f, a.y}, {(std::min)(b.x, x + v.tile * .5f), b.y}, c, 2);
            }
        } else if (d.kind == "Laser") {
            ui.CanvasCircle({(a.x + b.x) * .5f, (a.y + b.y) * .5f}, v.tile * .18f, c, 2);
        } else if (d.kind == "Door" || d.kind == "LockWall") {
            ui.CanvasLine({(a.x + b.x) * .5f, a.y}, {(a.x + b.x) * .5f, b.y}, c, 2);
        } else if (d.kind == "Checkpoint") {
            ui.CanvasLine({a.x + v.tile * .3f, b.y}, {a.x + v.tile * .3f, a.y}, c, 2);
            ui.CanvasFill({a.x + v.tile * .3f, a.y}, {a.x + v.tile * .8f, a.y + v.tile * .35f}, c);
        }
    }
}

inline void DrawRoomMap(IEditorWorkspaceUi& ui, const RegionSettings& s, const MapCanvasView& v, bool grid, bool links) {
    const auto valid = ValidateRegion(s.region);
    if (!valid.valid || s.region.cells.size() > 4096) { return; }
    ui.CanvasFill(v.Screen(0, 0), v.Screen(static_cast<float>(s.region.cols), static_cast<float>(s.region.rows)), {.075f, .12f, .15f, 1});
    for (int y = 0; y < s.region.rows; ++y) for (int x = 0; x < s.region.cols; ++x) {
        const auto a = v.Screen(static_cast<float>(x), static_cast<float>(y));
        const auto b = v.Screen(static_cast<float>(x + 1), static_cast<float>(y + 1));
        if (s.region.cells[CellIndex(s.region, x, y)] == '1') {
            const auto empty = [&s](int cx, int cy) { return cx < 0 || cy < 0 || cx >= s.region.cols || cy >= s.region.rows || s.region.cells[CellIndex(s.region, cx, cy)] == '0'; };
            const bool top = empty(x, y - 1), bottom = empty(x, y + 1), left = empty(x - 1, y), right = empty(x + 1, y);
            const std::string key = top ? (left ? "terrain.top_left" : right ? "terrain.top_right" : "terrain.top") : bottom ? (left ? "terrain.bottom_left" : right ? "terrain.bottom_right" : "terrain.bottom") : left ? "terrain.left" : right ? "terrain.right" : "terrain.fill";
            if (gMapSprites.Draw(ui, key, a, b)) { continue; }
            ui.CanvasFill(a, b, {.25f, .42f, .46f, 1});
            ui.CanvasFill(a, {b.x, a.y + v.tile * .16f}, {.53f, .78f, .72f, 1});
            ui.CanvasRect(a, b, {.12f, .25f, .29f, 1});
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
    const auto a = v.Screen(s.spawnX, s.spawnY), b = v.Screen(s.spawnX + .625f, s.spawnY + .875f);
    if (gMapSprites.Draw(ui, "player.idle.right.0", v.Screen(s.spawnX - .1875f, s.spawnY - .125f), v.Screen(s.spawnX + .8125f, s.spawnY + .875f))) { return; }
    ui.CanvasFill(a, b, {.83f, .91f, .94f, 1});
    ui.CanvasFill({a.x + v.tile * .35f, a.y + v.tile * .2f}, {b.x, a.y + v.tile * .28f}, {.12f, .23f, .30f, 1});
}

} // namespace TD2::Editor
