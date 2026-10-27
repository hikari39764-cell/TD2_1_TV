#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
namespace {

EditorWorkspacePoint Center(const DeviceDraft& d, int worldX, int worldY) {
    return {static_cast<float>(worldX + d.x + d.w * .5), static_cast<float>(worldY + d.y + d.h * .5)};
}
// 地図の右の「未配置」の幅。
constexpr float kTrayWidth = 112;
bool AnyUnplaced(const MapModel& model) {
    return std::any_of(model.rooms.begin(), model.rooms.end(), [](const RoomItem& r) { return !r.settings.worldPlaced; });
}
// 「裏」の印：枠の右上。
void DrawBackBadge(IEditorWorkspaceUi& ui, EditorWorkspacePoint a, EditorWorkspacePoint b) {
    if (b.x - a.x < 30 || b.y - a.y < 22) { return; }
    ui.CanvasFill({b.x - 23, a.y + 3}, {b.x - 3, a.y + 21}, kAmber);
    ui.CanvasText({b.x - 19, a.y + 4}, {.08f, .07f, .05f, 1}, "裏");
}
}

const RegionSettings& LevelEditor::SettingsOf(const RoomItem& room) const {
    return room.object.objectId == model_.selected && model_.ready ? model_.draft : room.settings;
}

RoomItem* LevelEditor::RoomAt(EditorWorkspacePoint world, bool placedOnly) {
    for (auto it = model_.rooms.rbegin(); it != model_.rooms.rend(); ++it) {
        const auto& s = SettingsOf(*it);
        if (placedOnly && !s.worldPlaced) { continue; }
        if (ContainsWorldPoint(Placement(s), {world.x, world.y})) { return &*it; }
    }
    return nullptr;
}

MapCanvasView LevelEditor::RoomView(const RegionSettings& s) const {
    MapCanvasView local = view_;
    local.origin = view_.Screen(static_cast<float>(s.worldX), static_cast<float>(s.worldY));
    return local;
}

void LevelEditor::TrackDrag(const EditorWorkspaceCanvasInput& input) {
    dragHeldAny_ |= (input.heldButtons & kPrimary) != 0;
    dragPressedAny_ |= (input.pressedButtons & kPrimary) != 0;
    dragReleasedAny_ |= (input.releasedButtons & kPrimary) != 0;
}

std::vector<EdgeSelection> LevelEditor::Edges() const {
    std::vector<EdgeSelection> result;
    for (size_t a = 0; a < model_.rooms.size(); ++a) for (size_t b = a + 1; b < model_.rooms.size(); ++b) {
        const auto& ra = model_.rooms[a]; const auto& rb = model_.rooms[b];
        if (const auto edge = FindSharedWorldEdge(Placement(SettingsOf(ra)), Placement(SettingsOf(rb)))) { result.push_back({ra.object.objectId, rb.object.objectId, *edge}); }
    }
    return result;
}

std::pair<EditorWorkspacePoint, EditorWorkspacePoint> LevelEditor::EdgePoints(const SharedWorldEdge& e) const {
    const float c = static_cast<float>(e.coordinate), lo = static_cast<float>(e.overlapBegin), hi = static_cast<float>(e.overlapEnd);
    const bool vertical = e.sideA == WorldSide::Left || e.sideA == WorldSide::Right;
    return vertical ? std::pair{view_.Screen(c, lo), view_.Screen(c, hi)} : std::pair{view_.Screen(lo, c), view_.Screen(hi, c)};
}

std::optional<EdgeSelection> LevelEditor::EdgeAt(EditorWorkspacePoint pointer) const {
    for (const auto& e : Edges()) {
        const auto [a, b] = EdgePoints(e.edge);
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float t = std::clamp(((pointer.x - a.x) * dx + (pointer.y - a.y) * dy) / (std::max)(.001f, dx * dx + dy * dy), 0.0f, 1.0f);
        if (std::hypot(pointer.x - a.x - t * dx, pointer.y - a.y - t * dy) < 7) { return e; }
    }
    return std::nullopt;
}

void LevelEditor::DrawMap(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    const auto input = ui.BeginCanvas("td2.level.canvas", 0);
    canvasHovered_ = input.hovered != 0;
    view_.ObserveSize(input.size);
    TrackDrag(input);
    if (input.size.x > 0 && input.size.y > 0) {
        if (tool_ == Tool::Camera && plan_) { EnsureLayout(ctx); DrawPlan(ctx, input); ui.EndCanvas(); return; }
        RefreshChapters(ctx);
        EnsureLayout(ctx);   // カメラ・仕掛けのほか、「裏」の印（3D の向き）にも使う
        // 開いた時は今のテレビの章に合わせる（ダブルクリックで世界全体）。「未配置」が出ている時はその幅を除いて合わせる。
        const EditorWorkspacePoint fitSize{input.size.x - (AnyUnplaced(model_) ? kTrayWidth : 0), input.size.y};
        if (view_.fit && (!model_.gesture || !view_.hasFit)) { if (!fitChapter_ || !FitChapter(fitSize)) { FitAll(fitSize); } }
        view_.Navigate(input, false, drag_ != Drag::None);
        DrawCanvasGrid(ui, view_, input.size);
        if (tool_ == Tool::Machine) { RefreshMachines(ctx); RefreshCheckInput(ctx); }
        const auto p = view_.World(input.pointer);
        // 「未配置」の上は地図の外として扱う（引いてきたテレビをそこで離すと取り消し）。
        const bool inside = PointIn(input.pointer, {0, 0}, input.size) && !OverTray(input.pointer, input.size);
        if (drag_ == Drag::None && !pickingStart_ && !view_.panning && input.hovered && (input.pressedButtons & (kPrimary | kSecondary)) && !PressTray(ctx, input)) {
            switch (tool_) {
            case Tool::Select: PressSelect(ctx, input, p); break;
            case Tool::Terrain: PressTerrain(ctx, input, p); break;
            case Tool::Device: PressDevice(ctx, input, p); break;
            case Tool::Link: if (input.pressedButtons & kPrimary) { PressLink(ctx, p); } else { link_ = {}; } break;
            case Tool::Machine:
                // テレビを押すとそのテレビの仕掛けを選び、テレビの移動などは選ぶ道具と同じに扱う。
                if (!PressMachine(ctx, input, p)) { PressSelect(ctx, input, p); if (SelectedMachine() && focus_ == Focus::Room) { focus_ = Focus::Machine; } }
                break;
            case Tool::Camera: {
                if (!(input.pressedButtons & kPrimary)) { break; }
                if (const auto edge = EdgeAt(input.pointer)) { if (model_.Commit(ctx)) { edge_ = *edge; focus_ = Focus::Edge; edgeDraftKey_.clear(); } break; }
                if (auto* room = RoomAt(p)) {
                    const auto id = room->object.objectId; const auto region = room->settings.region.regionId;
                    if (!areaFocus_.empty()) { ToggleArea(ctx, region); }
                    if (SelectRoom(ctx, id)) { focus_ = Focus::Room; }
                } else { focus_ = Focus::None; edge_ = {}; }
                break;
            }
            case Tool::Stage: PressStage(ctx, input, p); break;
            }
        }
        if (drag_ != Drag::None) { ContinueDrag(ctx, input, p, inside); }
        DrawRooms(ctx, input);
        DrawChapterLabels(ui);
        DrawEdges(ui, input);
        if (tool_ == Tool::Machine) { DrawMachineOverlay(ui, input); }
        DrawMachineIcons(ui);
        DrawLinks(ui, input);
        DrawSelectionHandles(ui);
        DrawPlacementGhost(ui, input, p);
        DrawIssues(ui);
        if (drag_ == Drag::Box) {
            ui.CanvasRect(view_.Screen((std::min)(dragStart_.x, p.x), (std::min)(dragStart_.y, p.y)), view_.Screen((std::max)(dragStart_.x, p.x), (std::max)(dragStart_.y, p.y)), kAmber, 1.5f);
        }
        DrawTray(ui, input);
        DrawToolHint(ui, input);
        DrawSpawnPicker(ctx, input, p);
        if (drag_ == Drag::None && input.hovered && !view_.panning && inside) { DrawHover(ui, input, p); }
    }
    ui.EndCanvas();
    HandleDelete(ctx);
}

// ---------------------------------------------------------------- 描く
void LevelEditor::DrawRooms(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput&) {
    auto& ui = *ctx.ui;
    const bool editing = tool_ == Tool::Terrain || tool_ == Tool::Device;
    for (const auto& room : model_.rooms) {
        auto s = SettingsOf(room);
        if (!s.worldPlaced) { continue; }
        if (drag_ == Drag::Group && multi_.contains(room.object.objectId)) { s.worldX += groupDx_; s.worldY += groupDy_; }
        const auto local = RoomView(s);
        const bool selected = room.object.objectId == model_.selected && edge_.a == 0;
        const auto a = local.Screen(0, 0), b = local.Screen(static_cast<float>(s.region.cols), static_cast<float>(s.region.rows));
        // 縮めて全体を見ている時は、テレビを一つの塊で描く（マスは小さすぎて読めない）。
        if (view_.tile >= 2.5f) { DrawRoomMap(ui, s, local, view_.tile >= 12, false, s.entryRegion); }
        else { ui.CanvasFill(a, b, s.entryRegion ? EditorWorkspaceColor{.42f, .64f, .58f, 1} : EditorWorkspaceColor{.20f, .34f, .37f, 1}); }
        // 地形と装置を直している間は、ほかのテレビを少し暗くする（隣は見えたまま）。
        if (editing && !selected) { ui.CanvasFill(a, b, {0, 0, 0, .32f}); }
        DrawAreaTint(ui, s, a, b);
        const bool chosen = selected || multi_.contains(room.object.objectId);
        const bool invalid = model_.gesture && selected && (drag_ == Drag::Room || drag_ == Drag::Library) && !model_.Validate(s).empty();
        ui.CanvasRect(a, b, invalid ? kRed : chosen ? kAmber : kFrame, chosen ? 3.0f : 1.0f);
        if (BackFacing(room)) { DrawBackBadge(ui, a, b); }
        // 名前：中くらいに見ている時はすべて、近くで見ている時は選んだテレビだけ（全体の時は章の名前を出す）。
        if ((view_.tile >= 3 && view_.tile < 14) || (chosen && view_.tile >= 3)) {
            const auto name = ShortChoiceLabel(room.object.name, 12);
            ui.CanvasFill({a.x + 2, a.y + 2}, {a.x + 8 + TextWidth(name), a.y + 18}, {.04f, .06f, .08f, .72f});
            ui.CanvasText({a.x + 5, a.y + 3}, kLabel, name);
        }
        // 圧力板とつなげる装置の英字（連線の相手が分かるように）。
        if (view_.tile >= 16 || tool_ == Tool::Link) {
            DeviceEditor devices; devices.Load(s.devices);
            for (const auto& d : devices.Items()) {
                if (d.kind != "Plate" && !IsLinkableKind(d.kind)) { continue; }
                const auto letter = DeviceLetter(d);
                const auto at = local.Screen(static_cast<float>(d.x), static_cast<float>(d.y));
                ui.CanvasText({at.x + 1, at.y - 14}, d.kind == "Plate" ? kAmber : EditorWorkspaceColor{.85f, .88f, .9f, 1}, letter.empty() ? ShortChoiceLabel(d.id, 8) : letter);
            }
        }
    }
}

// 縮めて全体を見ている時の章の名前：章のテレビをまとめた範囲の左上。
void LevelEditor::DrawChapterLabels(IEditorWorkspaceUi& ui) {
    if (view_.tile >= 3 || chapterOf_.empty()) { return; }
    std::map<std::string, WorldGridRect> bounds;
    for (const auto& room : model_.rooms) {
        const auto& s = SettingsOf(room);
        const auto found = chapterOf_.find(room.object.objectId);
        if (!s.worldPlaced || found == chapterOf_.end()) { continue; }
        const auto r = WorldBounds(Placement(s));
        auto [it, fresh] = bounds.emplace(found->second, r);
        if (!fresh) { auto& b = it->second; b.left = (std::min)(b.left, r.left); b.top = (std::min)(b.top, r.top); b.right = (std::max)(b.right, r.right); b.bottom = (std::max)(b.bottom, r.bottom); }
    }
    for (const auto& [name, b] : bounds) {
        const auto at = view_.Screen(static_cast<float>(b.left), static_cast<float>(b.top));
        ui.CanvasRect(at, view_.Screen(static_cast<float>(b.right), static_cast<float>(b.bottom)), {.45f, .62f, .64f, .45f}, 1);
        ui.CanvasFill({at.x, at.y - 18}, {at.x + 8 + TextWidth(name), at.y}, {.04f, .06f, .08f, .8f});
        ui.CanvasText({at.x + 4, at.y - 17}, {.90f, .94f, .94f, 1}, name);
    }
}

bool LevelEditor::FitChapter(EditorWorkspacePoint size) {
    const auto* current = model_.Current();
    if (!current) { return false; }
    const auto chapter = chapterOf_.find(current->object.objectId);
    bool any = false; WorldGridRect b{};
    for (const auto& room : model_.rooms) {
        const auto& s = SettingsOf(room);
        if (!s.worldPlaced) { continue; }
        const auto found = chapterOf_.find(room.object.objectId);
        const bool same = chapter != chapterOf_.end() ? found != chapterOf_.end() && found->second == chapter->second : room.object.objectId == current->object.objectId;
        if (!same) { continue; }
        const auto r = WorldBounds(Placement(s));
        if (!any) { b = r; any = true; }
        else { b.left = (std::min)(b.left, r.left); b.top = (std::min)(b.top, r.top); b.right = (std::max)(b.right, r.right); b.bottom = (std::max)(b.bottom, r.bottom); }
    }
    if (!any) { return false; }
    const float margin = 6;
    view_.Fit(size, static_cast<float>(b.left) - margin, static_cast<float>(b.top) - margin, static_cast<float>(b.right - b.left) + margin * 2, static_cast<float>(b.bottom - b.top) + margin * 2);
    return true;
}

void LevelEditor::DrawEdges(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input) {
    bool selectedExists = false;
    for (const auto& e : Edges()) {
        const auto [a, b] = EdgePoints(e.edge);
        const bool selected = e.a == edge_.a && e.b == edge_.b;
        if (selected) { edge_ = e; selectedExists = true; }
        const bool vertical = e.edge.sideA == WorldSide::Left || e.edge.sideA == WorldSide::Right;
        const bool hovered = tool_ == Tool::Camera && input.hovered &&
            (vertical ? std::abs(input.pointer.x - a.x) <= 6 && input.pointer.y >= a.y && input.pointer.y <= b.y
                      : std::abs(input.pointer.y - a.y) <= 6 && input.pointer.x >= a.x && input.pointer.x <= b.x);
        if (selected || hovered) { ui.CanvasLine(a, b, selected ? kAmber : EditorWorkspaceColor{.55f, .74f, .72f, .8f}, selected ? 4.0f : 2.5f); }
        else if (tool_ == Tool::Camera) { DrawEdgeKind(ui, e, a, b); }
    }
    if (edge_.a && !selectedExists) { edge_ = {}; if (focus_ == Focus::Edge) { focus_ = Focus::None; } }
}

void LevelEditor::DrawLinks(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input) {
    // 区域 ID → 地図の位置と装置（線を引くためだけに読む）。
    struct RoomDevices { int x, y; std::vector<DeviceDraft> items; };
    std::map<std::string, RoomDevices, std::less<>> rooms;
    for (const auto& room : model_.rooms) {
        const auto& s = SettingsOf(room);
        if (!s.worldPlaced) { continue; }
        DeviceEditor d; d.Load(s.devices);
        rooms[s.region.regionId] = {s.worldX, s.worldY, d.Items()};
    }
    const auto* current = model_.Current();
    const std::string currentRegion = current ? SettingsOf(*current).region.regionId : std::string{};
    const auto* selectedDevice = model_.devices.Selected();
    for (const auto& [region, data] : rooms) for (const auto& d : data.items) {
        if (d.kind != "Plate" || d.targets.empty()) { continue; }
        const auto from = view_.Screen(Center(d, data.x, data.y).x, Center(d, data.x, data.y).y);
        const bool focused = tool_ == Tool::Link || (focus_ == Focus::Device && region == currentRegion && selectedDevice && selectedDevice->id == d.id);
        for (const auto& target : d.targets) {
            std::optional<EditorWorkspacePoint> to;
            bool machine = false;
            const auto kind = ClassifyTarget(target);
            if (kind == TargetKind::Local) {
                for (const auto& t : data.items) { if (t.id == target) { const auto c = Center(t, data.x, data.y); to = view_.Screen(c.x, c.y); } }
            } else if (kind == TargetKind::OtherRegion) {
                const auto slash = target.find('/');
                if (const auto other = rooms.find(target.substr(0, slash)); other != rooms.end()) {
                    for (const auto& t : other->second.items) { if (t.id == target.substr(slash + 1)) { const auto c = Center(t, other->second.x, other->second.y); to = view_.Screen(c.x, c.y); } }
                }
            } else if (kind == TargetKind::Machine) {
                machine = true;
                for (const auto& m : model_.machines) for (const auto& [receiver, label] : m.Receivers()) {
                    if (receiver == target) { if (const auto icon = MachineIcon(m)) { to = view_.Screen(icon->x, icon->y); } }
                }
            }
            if (!to) { continue; }
            auto color = machine ? kMagenta : kAmber;
            color.a = focused ? .95f : .28f;
            ui.CanvasLine(from, *to, color, focused ? 2.5f : 1.2f);
            if (focused) { ui.CanvasCircle(*to, 4, color, 2); }
        }
    }
    // 連線の途中：板からポインタへ。
    if (!link_.device.empty()) {
        for (const auto& room : model_.rooms) {
            if (room.object.objectId != link_.room) { continue; }
            const auto& s = SettingsOf(room);
            DeviceEditor d; d.Load(s.devices);
            for (const auto& item : d.Items()) {
                if (item.id != link_.device) { continue; }
                const auto c = Center(item, s.worldX, s.worldY);
                ui.CanvasLine(view_.Screen(c.x, c.y), input.pointer, kAmber, 2.5f);
                ui.CanvasCircle(view_.Screen(c.x, c.y), 5, kAmber, 2);
            }
        }
    }
}

void LevelEditor::DrawHover(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    std::string text;
    if (auto* room = RoomAt(p)) {
        const auto& s = SettingsOf(*room);
        DeviceEditor d; d.Load(s.devices);
        const int hit = d.Hit(p.x - s.worldX, p.y - s.worldY);
        text = ShortChoiceLabel(room->object.name, 14);
        if (hit >= 0) {
            const auto& device = d.Items()[static_cast<size_t>(hit)];
            text += " · " + DeviceName(device);
            if (device.kind == "Plate") { text += device.latch ? "（踏むと切り替え）" : "（踏んでいる間）"; if (!device.targets.empty()) { text += " → " + std::to_string(device.targets.size()); } }
        }
    }
    if (tool_ == Tool::Machine || tool_ == Tool::Link) {
        for (const auto& m : model_.machines) {
            const auto icon = MachineIcon(m);
            if (!icon) { continue; }
            const auto at = view_.Screen(icon->x, icon->y);
            if (std::hypot(input.pointer.x - at.x, input.pointer.y - at.y) < 12) { text = RecipeLabel(m) + " · " + m.id; }
        }
    }
    if (text.empty()) { return; }
    const float width = 10 + TextWidth(text);
    const EditorWorkspacePoint at{input.pointer.x + 14, input.pointer.y + 12};
    ui.CanvasFill(at, {at.x + width, at.y + 20}, {.03f, .05f, .07f, .86f});
    ui.CanvasText({at.x + 5, at.y + 3}, kLabel, text);
}

void LevelEditor::DrawSelectionHandles(IEditorWorkspaceUi& ui) {
    if (focus_ != Focus::Device || (tool_ != Tool::Select && tool_ != Tool::Device)) { return; }
    const auto* d = model_.devices.Selected(); const auto* room = model_.Current();
    if (!d || !room) { return; }
    const auto& s = SettingsOf(*room);
    const auto local = RoomView(s);
    const auto a = local.Screen(static_cast<float>(d->x), static_cast<float>(d->y));
    const auto b = local.Screen(static_cast<float>(d->x + d->w), static_cast<float>(d->y + d->h));
    ui.CanvasRect(a, b, kAmber, 2);
    const float x = (a.x + b.x) * .5f, y = (a.y + b.y) * .5f;
    for (const auto& h : std::array<EditorWorkspacePoint, 8>{a, EditorWorkspacePoint{x, a.y}, EditorWorkspacePoint{b.x, a.y}, EditorWorkspacePoint{b.x, y}, b, EditorWorkspacePoint{x, b.y}, EditorWorkspacePoint{a.x, b.y}, EditorWorkspacePoint{a.x, y}}) {
        ui.CanvasFill({h.x - 4, h.y - 4}, {h.x + 4, h.y + 4}, kAmber);
    }
}

int LevelEditor::HandleAt(EditorWorkspacePoint screen) const {
    const auto* d = model_.devices.Selected(); const auto* room = model_.Current();
    if (!d || !room || focus_ != Focus::Device) { return -1; }
    const auto local = RoomView(SettingsOf(*room));
    const auto a = local.Screen(static_cast<float>(d->x), static_cast<float>(d->y));
    const auto b = local.Screen(static_cast<float>(d->x + d->w), static_cast<float>(d->y + d->h));
    const float x = (a.x + b.x) * .5f, y = (a.y + b.y) * .5f;
    const std::array<EditorWorkspacePoint, 8> points{a, EditorWorkspacePoint{x, a.y}, EditorWorkspacePoint{b.x, a.y}, EditorWorkspacePoint{b.x, y}, b, EditorWorkspacePoint{x, b.y}, EditorWorkspacePoint{a.x, b.y}, EditorWorkspacePoint{a.x, y}};
    // 薄い装置ではハンドルが近いので、一番近いものを選ぶ。
    int hit = -1; float closest = 81;
    for (size_t i = 0; i < points.size(); ++i) {
        const float dx = screen.x - points[i].x, dy = screen.y - points[i].y, distance = dx * dx + dy * dy;
        if (distance <= closest) { closest = distance; hit = static_cast<int>(i); }
    }
    return hit;
}

// 置く前の影：装置・地形の筆。
void LevelEditor::DrawPlacementGhost(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    if (!input.hovered || view_.panning || drag_ != Drag::None || pickingStart_) { return; }
    auto* room = RoomAt(p);
    if (!room) { return; }
    const auto& s = SettingsOf(*room);
    const auto local = RoomView(s);
    const float lx = p.x - s.worldX, ly = p.y - s.worldY;
    if (tool_ == Tool::Terrain) {
        const float x0 = std::floor(lx), y0 = std::floor(ly);
        ui.CanvasRect(local.Screen(x0, y0), local.Screen(x0 + brushSize_, y0 + brushSize_), kAmber, 1.5f);
    }
    if (tool_ == Tool::Device) {
        DeviceEditor d; d.Load(s.devices);
        if (d.Hit(lx, ly) >= 0 && !input.shift) { return; }
        DeviceDraft ghost; ghost.kind = kind_; ghost.x = std::floor(lx); ghost.y = std::floor(ly);
        if (kind_ == "Door" || kind_ == "LockWall") { ghost.h = (std::min)(3.0, s.region.rows - ghost.y); }
        if (kind_ == "OneWay" || kind_ == "Bridge") { ghost.w = (std::min)(3.0, s.region.cols - ghost.x); }
        if (kind_ == "Laser") { ghost.direction = "Right"; }
        if (kind_ == "Goal") { ghost.w = 2; ghost.h = 3; ghost.y = (std::max)(0.0, std::floor(static_cast<double>(ly)) - 2); }
        ApplyTemplate(ghost, s.region.cols, s.region.rows);
        DrawDeviceGlyph(ui, ghost, local);
        ui.CanvasRect(local.Screen(static_cast<float>(ghost.x), static_cast<float>(ghost.y)), local.Screen(static_cast<float>(ghost.x + ghost.w), static_cast<float>(ghost.y + ghost.h)), {.97f, .80f, .40f, .7f}, 1.5f);
    }
}

// 地図で隣になるテレビ（共有する辺が一番長いもの）の区域 ID。無ければ空。
std::string LevelEditor::NeighborRegion(const RegionSettings& s) const {
    const auto self = Placement(s);
    std::string best; int64_t length = 0;
    for (const auto& room : model_.rooms) {
        if (room.settings.region.regionId == s.region.regionId) { continue; }
        if (const auto edge = FindSharedWorldEdge(self, Placement(room.settings)); edge && edge->overlapEnd - edge->overlapBegin > length) {
            length = edge->overlapEnd - edge->overlapBegin; best = room.settings.region.regionId;
        }
    }
    return best;
}

// ---------------------------------------------------------------- 未配置・操作の一行・裏
// 世界に置いていないテレビを右の列に縦に並べる（入らない分は数だけ出す）。
std::vector<LevelEditor::TrayItem> LevelEditor::TrayItems(EditorWorkspacePoint size) const {
    std::vector<TrayItem> items;
    const float left = size.x - kTrayWidth + 8, width = kTrayWidth - 16;
    float y = 30;
    for (const auto& room : model_.rooms) {
        const auto& r = room.settings.region;
        if (room.settings.worldPlaced) { continue; }
        const float h = std::clamp(width * r.rows / static_cast<float>((std::max)(1, r.cols)), 20.0f, 72.0f);
        if (y + h + 20 > size.y - 24) { break; }
        items.push_back({room.object.objectId, {left, y}, {left + width, y + h}});
        y += h + 26;
    }
    return items;
}

bool LevelEditor::OverTray(EditorWorkspacePoint pointer, EditorWorkspacePoint size) const {
    return pointer.x >= size.x - kTrayWidth && AnyUnplaced(model_);
}

// 「未配置」の中の押し：テレビを押したら地図へ引き始める。列の上の押しは地図へ渡さない。
bool LevelEditor::PressTray(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input) {
    if (!OverTray(input.pointer, input.size)) { return false; }
    if (!(input.pressedButtons & kPrimary)) { return true; }
    for (const auto& item : TrayItems(input.size)) {
        if (!PointIn(input.pointer, item.a, {item.b.x, item.b.y + 18})) { continue; }
        if (SelectRoom(ctx, item.objectId)) { drag_ = Drag::Library; model_.gesture = true; focus_ = Focus::Room; multi_.clear(); }
        break;
    }
    return true;
}

void LevelEditor::DrawTray(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input) {
    if (!AnyUnplaced(model_)) { return; }
    const float x0 = input.size.x - kTrayWidth;
    const bool dragging = drag_ == Drag::Library, over = dragging && OverTray(input.pointer, input.size);
    ui.CanvasFill({x0, 0}, input.size, {.05f, .07f, .09f, .94f});
    ui.CanvasRect({x0, 0}, input.size, over ? kAmber : kFrame, over ? 2.0f : 1.0f);
    ui.CanvasText({x0 + 8, 7}, over ? kAmber : kLabel, over ? "離すと取り消し" : "未配置");
    const auto items = TrayItems(input.size);
    for (const auto& item : items) {
        const auto* room = model_.Room(item.objectId);
        if (!room) { continue; }
        const auto& s = room->settings;
        MapCanvasView v;
        v.tile = (std::min)((item.b.x - item.a.x) / static_cast<float>((std::max)(1, s.region.cols)), (item.b.y - item.a.y) / static_cast<float>((std::max)(1, s.region.rows)));
        v.origin = {(item.a.x + item.b.x - s.region.cols * v.tile) * .5f, (item.a.y + item.b.y - s.region.rows * v.tile) * .5f};
        DrawRoomMap(ui, s, v, false, false, false);
        const auto a = v.Screen(0, 0), b = v.Screen(static_cast<float>(s.region.cols), static_cast<float>(s.region.rows));
        const bool held = dragging && item.objectId == model_.selected;
        if (held) { ui.CanvasFill(a, b, {0, 0, 0, .55f}); }
        const bool hover = !dragging && PointIn(input.pointer, item.a, {item.b.x, item.b.y + 18});
        ui.CanvasRect(a, b, hover || held ? kAmber : kFrame, hover ? 2.0f : 1.0f);
        if (BackFacing(*room)) { DrawBackBadge(ui, a, b); }
        ui.CanvasText({item.a.x, item.b.y + 2}, kLabel, ShortChoiceLabel(room->object.name, 6));
    }
    const auto total = static_cast<size_t>(std::count_if(model_.rooms.begin(), model_.rooms.end(), [](const RoomItem& r) { return !r.settings.worldPlaced; }));
    if (total > items.size()) { ui.CanvasText({x0 + 8, input.size.y - 22}, kLabel, "ほか " + std::to_string(total - items.size()) + " 台"); }
}

// 今の道具でできることを一行だけ（地図の左下）。
void LevelEditor::DrawToolHint(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input) {
    std::string_view text;
    if (pickingStart_) { text = "人物を置く所をクリック · 右クリックでやめる"; }
    else if (drag_ == Drag::Library) { text = "置きたい所で離す"; }
    else switch (tool_) {
    case Tool::Select: text = AnyUnplaced(model_) ? "右の「未配置」から地図へドラッグ · 空いた所をダブルクリックで全体" : "テレビをドラッグで動かす · 空いた所をダブルクリックで全体"; break;
    case Tool::Terrain: text = "左ドラッグで描く · 右ドラッグで消す"; break;
    case Tool::Device: text = "クリックで置く · ドラッグで動かす · 右クリックで消す"; break;
    case Tool::Link: text = "圧力板から扉・レーザー・橋・ゴール・仕掛けへドラッグ"; break;
    case Tool::Machine: text = "下で種類を選び、動かすテレビを押す"; break;
    case Tool::Camera: text = "テレビでカメラ · 境目で切り替えの演出"; break;
    case Tool::Stage: text = "テレビを押して 3D の付き方を直す"; break;
    }
    if (text.empty()) { return; }
    const float y = input.size.y - 26;
    ui.CanvasFill({6, y}, {16 + TextWidth(text), y + 20}, {.04f, .06f, .08f, .8f});
    ui.CanvasText({11, y + 2}, {.80f, .86f, .87f, 1}, text);
}

bool LevelEditor::BackFacing(const RoomItem& room) {
    if (!layoutValid_) { return false; }
    if (const uint64_t key = layoutVersion_ * 1000003u + model_.revision; key != backKey_) { back_.clear(); backKey_ = key; }
    const auto [it, fresh] = back_.try_emplace(room.object.objectId, false);
    if (fresh) { it->second = IsBackFacing(layout_, room.settings.region.regionId); }
    return it->second;
}
}
