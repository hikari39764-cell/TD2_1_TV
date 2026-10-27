// 世界地図の操作：選ぶ・塗る・置く・つなぐの押しとドラッグ、まとめて動かす、消す、出生点の選び。
#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
namespace {
bool Released(const EditorWorkspaceCanvasInput& input) {
    constexpr uint32_t buttons = kPrimary | kSecondary;
    return (input.releasedButtons & buttons) || (!(input.heldButtons & buttons) && !(input.pressedButtons & buttons));
}
}

// ---------------------------------------------------------------- 押す
void LevelEditor::PressSelect(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    if (input.pressedButtons & kSecondary) { multi_.clear(); return; }
    auto* room = RoomAt(p);
    if (input.shift && room) {
        // Shift+クリック：テレビを複数選ぶ（今の選択も入れる）。
        if (multi_.empty() && model_.selected && model_.selected != room->object.objectId) { multi_.insert(model_.selected); }
        if (!multi_.erase(room->object.objectId)) { multi_.insert(room->object.objectId); }
        return;
    }
    if (room && multi_.size() > 1 && multi_.contains(room->object.objectId)) {
        if (model_.Dirty() && !model_.Commit(ctx)) { return; }
        drag_ = Drag::Group; dragStart_ = p; groupDx_ = groupDy_ = 0; model_.gesture = true;
        return;
    }
    multi_.clear();
    if (!room) {
        // 空いた所のダブルクリックで世界全体を見る。
        if (clock_ - emptyPressAt_ < .35f) { fitChapter_ = false; view_.fit = true; emptyPressAt_ = -1; return; }
        emptyPressAt_ = clock_;
        drag_ = Drag::Box; dragStart_ = p; focus_ = Focus::None; edge_ = {}; model_.devices.Select(-1); return;
    }
    const auto id = room->object.objectId;
    // 選んだ装置のハンドル（大きさを変える）を先に見る。
    if (id == model_.selected && focus_ == Focus::Device) {
        handle_ = HandleAt(input.pointer);
        if (handle_ >= 0) {
            if (const auto* d = model_.devices.Selected()) { original_ = *d; dragStart_ = {p.x - model_.draft.worldX, p.y - model_.draft.worldY}; drag_ = Drag::Resize; model_.gesture = true; return; }
        }
    }
    if (!SelectRoom(ctx, id)) { return; }
    auto& s = model_.draft;
    const float lx = p.x - s.worldX, ly = p.y - s.worldY;
    const int hit = model_.devices.Hit(lx, ly);
    if (hit >= 0) {
        model_.devices.Select(hit); focus_ = Focus::Device;
        original_ = *model_.devices.Selected(); dragStart_ = {lx, ly}; drag_ = Drag::Device; model_.gesture = true;
        return;
    }
    model_.devices.Select(-1); focus_ = Focus::Room;
    // スポーン位置の人の記号（開始テレビだけ）。
    if (s.entryRegion && lx >= s.spawnX && lx < s.spawnX + float(kPlayerWidth) && ly >= s.spawnY && ly < s.spawnY + float(kPlayerHeight)) { drag_ = Drag::Spawn; model_.gesture = true; return; }
    // ダブルクリックでそのテレビへ寄る。
    if (lastClick_ == id && clock_ - lastClickTime_ < .3f) { FitRoom(s); lastClick_ = 0; return; }
    lastClick_ = id; lastClickTime_ = clock_;
    drag_ = Drag::Room; model_.gesture = true; dragOffset_ = {p.x - static_cast<float>(s.worldX), p.y - static_cast<float>(s.worldY)};
}

void LevelEditor::PressTerrain(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    auto* room = RoomAt(p);
    if (!room || !SelectRoom(ctx, room->object.objectId)) { return; }
    focus_ = Focus::Room; model_.devices.Select(-1);
    auto& s = model_.draft;
    const EditorWorkspacePoint local{p.x - s.worldX, p.y - s.worldY};
    erase_ = (input.pressedButtons & kSecondary) != 0;
    erasedDeviceCells_.clear();
    dragStart_ = dragLast_ = local;
    drag_ = Drag::Paint; model_.gesture = true;
    if (!rectanglePaint_) { PaintLine(local, local, erase_); }
}

void LevelEditor::PressDevice(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    auto* room = RoomAt(p);
    if (!room || !SelectRoom(ctx, room->object.objectId)) { return; }
    auto& s = model_.draft;
    const float lx = p.x - s.worldX, ly = p.y - s.worldY;
    if (input.pressedButtons & kSecondary) {
        // 右クリックは消す。
        const int hit = model_.devices.Hit(lx, ly);
        if (hit >= 0) { model_.devices.Select(hit); model_.devices.RemoveSelected(); s.devices = model_.devices.Serialize(); model_.Commit(ctx); focus_ = Focus::Room; }
        return;
    }
    const int hit = model_.devices.Hit(lx, ly);
    if (hit >= 0 && !input.shift) {
        model_.devices.Select(hit); focus_ = Focus::Device;
        original_ = *model_.devices.Selected(); dragStart_ = {lx, ly}; drag_ = Drag::Device; model_.gesture = true;
        return;
    }
    if (model_.devices.Add(kind_, static_cast<int>(std::floor(lx)), static_cast<int>(std::floor(ly)), s.region.cols, s.region.rows)) {
        if (auto* added = model_.devices.Selected()) { ApplyTemplate(*added, s.region.cols, s.region.rows); }
        s.devices = model_.devices.Serialize();
        if (model_.Commit(ctx)) { focus_ = Focus::Device; }
    }
}

void LevelEditor::PressStage(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    if (!(input.pressedButtons & kPrimary)) { return; }
    auto* room = RoomAt(p);
    if (!room) { focus_ = Focus::None; return; }
    if (SelectRoom(ctx, room->object.objectId)) { focus_ = Focus::Room; model_.devices.Select(-1); }
}

// 連線：板を押して、扉・レーザー・橋か仕掛けの印の上で離す。押して離すだけなら、次に押した所が相手になる。
void LevelEditor::PressLink(EditorWorkspaceHostContext& ctx, EditorWorkspacePoint p) {
    auto* room = RoomAt(p);
    if (link_.device.empty()) {
        if (!room) { return; }
        const auto& s = SettingsOf(*room);
        DeviceEditor d; d.Load(s.devices);
        const int hit = d.Hit(p.x - s.worldX, p.y - s.worldY);
        if (hit < 0 || d.Items()[static_cast<size_t>(hit)].kind != "Plate") { return; }
        link_ = {room->object.objectId, d.Items()[static_cast<size_t>(hit)].id};
        if (SelectRoom(ctx, room->object.objectId)) { model_.devices.Select(hit); focus_ = Focus::Device; }
        drag_ = Drag::Link;
        return;
    }
    ReleaseLink(ctx, p);
}

void LevelEditor::ReleaseLink(EditorWorkspaceHostContext& ctx, EditorWorkspacePoint p) {
    if (link_.device.empty()) { return; }
    const auto source = link_;
    const auto* plateRoom = model_.Room(source.room);
    if (!plateRoom) { link_ = {}; return; }
    std::string target;
    // 仕掛けの印。
    const auto pointer = view_.Screen(p.x, p.y);
    for (const auto& m : model_.machines) {
        const auto icon = MachineIcon(m);
        if (!icon) { continue; }
        const auto at = view_.Screen(icon->x, icon->y);
        if (std::hypot(pointer.x - at.x, pointer.y - at.y) < 14) { target = PlateTargetFor(m, SettingsOf(*plateRoom).region.regionId, source.device, SettingsOf(*plateRoom).devices); }
    }
    if (target.empty()) {
        if (auto* room = RoomAt(p)) {
            const auto& s = SettingsOf(*room);
            DeviceEditor d; d.Load(s.devices);
            const int hit = d.Hit(p.x - s.worldX, p.y - s.worldY);
            if (hit >= 0) {
                const auto& t = d.Items()[static_cast<size_t>(hit)];
                if (room->object.objectId == source.room && t.id == source.device) { return; }   // 同じ板の上で離した：次のクリックを待つ
                if (IsLinkableKind(t.kind)) { target = room->object.objectId == source.room ? t.id : s.region.regionId + "/" + t.id; }
            }
        }
    }
    link_ = {};
    if (target.empty()) { return; }
    TogglePlateTarget(ctx, SettingsOf(*plateRoom).region.regionId + "/" + source.device, target);
}

// ---------------------------------------------------------------- ドラッグ
void LevelEditor::ContinueDrag(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p, bool inside) {
    auto& s = model_.draft;
    const EditorWorkspacePoint local{p.x - s.worldX, p.y - s.worldY};
    const EditorWorkspacePoint clamped{std::clamp(local.x, 0.0f, static_cast<float>(s.region.cols) - .001f), std::clamp(local.y, 0.0f, static_cast<float>(s.region.rows) - .001f)};
    switch (drag_) {
    case Drag::Room:
        s.worldPlaced = true;
        s.worldX = static_cast<int>(std::round(std::clamp(p.x - dragOffset_.x, -1e6f, 1e6f)));
        s.worldY = static_cast<int>(std::round(std::clamp(p.y - dragOffset_.y, -1e6f, 1e6f)));
        break;
    case Drag::Library:
        // 「未配置」から引いてきたテレビは、地図の上にある間だけ置き場所を示す。
        if (model_.ready) {
            s.worldPlaced = inside;
            s.worldX = static_cast<int>(std::round(p.x - s.region.cols * .5f)); s.worldY = static_cast<int>(std::round(p.y - s.region.rows * .5f));
        }
        if (dragReleasedAny_ || (!dragHeldAny_ && !dragPressedAny_)) { FinishDrag(ctx, p, inside); }
        return;
    case Drag::Group:
        groupDx_ = static_cast<int>(std::round(p.x - dragStart_.x)); groupDy_ = static_cast<int>(std::round(p.y - dragStart_.y));
        break;
    case Drag::Box: break;
    case Drag::Paint:
        if (!rectanglePaint_) { PaintLine(dragLast_, clamped, erase_); }
        dragLast_ = clamped;
        break;
    case Drag::Spawn:
        // 押したマスの床に足を着ける（体は上のマスへはみ出す）。
        s.spawnX = (std::min)(std::floor(clamped.x) + .1875f, static_cast<float>(s.region.cols) - float(kPlayerWidth));
        s.spawnY = std::clamp(std::floor(clamped.y) + 1 - float(kPlayerHeight), 0.0f, static_cast<float>(s.region.rows) - float(kPlayerHeight));
        break;
    case Drag::Device: case Drag::Resize: {
        auto* d = model_.devices.Selected();
        if (!d) { break; }
        const double dx = std::round(local.x - dragStart_.x), dy = std::round(local.y - dragStart_.y);
        *d = original_;
        if (drag_ == Drag::Device) {
            d->x = std::clamp(original_.x + dx, 0.0, s.region.cols - original_.w);
            d->y = std::clamp(original_.y + dy, 0.0, s.region.rows - original_.h);
        } else {
            double left = d->x, top = d->y, right = d->x + d->w, bottom = d->y + d->h;
            if (handle_ == 0 || handle_ == 6 || handle_ == 7) { left = std::clamp(left + dx, 0.0, right - .125); }
            if (handle_ == 2 || handle_ == 3 || handle_ == 4) { right = std::clamp(right + dx, left + .125, static_cast<double>(s.region.cols)); }
            if (handle_ == 0 || handle_ == 1 || handle_ == 2) { top = std::clamp(top + dy, 0.0, bottom - .125); }
            if (handle_ == 4 || handle_ == 5 || handle_ == 6) { bottom = std::clamp(bottom + dy, top + .125, static_cast<double>(s.region.rows)); }
            d->x = left; d->y = top; d->w = right - left; d->h = bottom - top;
        }
        s.devices = model_.devices.Serialize();
        break;
    }
    case Drag::Link: break;
    case Drag::Ghost: DragGhost(ctx, input, p); return;
    default: break;
    }
    if (Released(input)) { FinishDrag(ctx, p, inside); }
}

void LevelEditor::FinishDrag(EditorWorkspaceHostContext& ctx, EditorWorkspacePoint p, bool inside) {
    const auto kind = drag_;
    drag_ = Drag::None;
    switch (kind) {
    case Drag::Room: {
        const auto before = model_.Current() ? std::optional<RegionSettings>(model_.Current()->settings) : std::nullopt;
        const bool moved = before && (before->worldX != model_.draft.worldX || before->worldY != model_.draft.worldY);
        // 3D で隣に付けてあるテレビは、地図の新しい隣へ 3D でも付いて行く（作者が 3D で外したテレビは動かさない）。
        if (model_.Finish(ctx, true) && moved) {
            if (!before->stageAnchor.empty()) { AttachRoom(ctx, model_.draft.region.regionId, {}); }
            layoutStale_ = true;
        }
        break;
    }
    case Drag::Library:
        if (inside) {
            const bool first = model_.Current() && !model_.Current()->settings.worldPlaced;
            if (model_.Finish(ctx, true)) {
                focus_ = Focus::Room; layoutStale_ = true;
                // 初めて世界に置いたテレビは、地図で隣になったテレビの大区域へ入れてカメラを作る。
                if (first) { SetUpNewRoom(ctx, model_.draft.region.regionId, NeighborRegion(model_.draft)); }
            }
        } else { model_.Discard(); model_.gesture = false; }
        break;
    case Drag::Group: model_.gesture = false; FinishGroupMove(ctx); break;
    case Drag::Box: {
        const float x0 = (std::min)(dragStart_.x, p.x), x1 = (std::max)(dragStart_.x, p.x), y0 = (std::min)(dragStart_.y, p.y), y1 = (std::max)(dragStart_.y, p.y);
        multi_.clear();
        if (x1 - x0 < .5f && y1 - y0 < .5f) { break; }
        for (const auto& room : model_.rooms) {
            if (!room.settings.worldPlaced) { continue; }
            const auto r = WorldBounds(Placement(room.settings));
            if (r.left < x1 && r.right > x0 && r.top < y1 && r.bottom > y0) { multi_.insert(room.object.objectId); }
        }
        break;
    }
    case Drag::Paint:
        if (rectanglePaint_) {
            for (int y = static_cast<int>(std::floor((std::min)(dragStart_.y, dragLast_.y))); y <= static_cast<int>(std::floor((std::max)(dragStart_.y, dragLast_.y))); ++y)
                for (int x = static_cast<int>(std::floor((std::min)(dragStart_.x, dragLast_.x))); x <= static_cast<int>(std::floor((std::max)(dragStart_.x, dragLast_.x))); ++x) { PaintCell(x, y, erase_); }
        }
        model_.Finish(ctx);
        break;
    case Drag::Resize: if (const auto* d = model_.devices.Selected()) { RememberTemplate(*d); } model_.Finish(ctx); break;
    case Drag::Device: case Drag::Spawn: model_.Finish(ctx); break;
    case Drag::Link: ReleaseLink(ctx, p); break;
    default: break;
    }
}

bool LevelEditor::FinishGroupMove(EditorWorkspaceHostContext& ctx) {
    if (!groupDx_ && !groupDy_) { return true; }
    // 動かした後の配置で重なりを確かめ、全部を一回の取り消しで書く。仕掛けは相対で書いてあるので一緒に付いて行く。
    std::vector<RegionPlacement> layout; Json commands = Json::array();
    for (const auto& room : model_.rooms) {
        auto placement = Placement(room.settings);
        if (multi_.contains(room.object.objectId) && placement.worldPlaced) {
            placement.worldX += groupDx_; placement.worldY += groupDy_;
            commands.push_back(PropertyCommand(room.target.objectId, room.target.componentId, "worldX", placement.worldX));
            commands.push_back(PropertyCommand(room.target.objectId, room.target.componentId, "worldY", placement.worldY));
        }
        layout.push_back(placement);
    }
    groupDx_ = groupDy_ = 0;
    if (const auto valid = ValidateWorldLayout(layout); !valid.valid) { model_.error = "ここには置けません：" + valid.message; return false; }
    if (!Transact(ctx, commands, model_.error)) { return false; }
    model_.error.clear(); MarkSceneChanged(); model_.refresh = true; model_.Poll(ctx, true);
    return true;
}

void LevelEditor::PaintLine(EditorWorkspacePoint a, EditorWorkspacePoint b, bool erase) {
    int x = static_cast<int>(std::floor(a.x)), y = static_cast<int>(std::floor(a.y));
    const int ex = static_cast<int>(std::floor(b.x)), ey = static_cast<int>(std::floor(b.y));
    const int dx = std::abs(ex - x), dy = -std::abs(ey - y), sx = x < ex ? 1 : -1, sy = y < ey ? 1 : -1;
    int error = dx + dy;
    for (int steps = 0; steps < 512; ++steps) {
        for (int by = 0; by < brushSize_; ++by) for (int bx = 0; bx < brushSize_; ++bx) { PaintCell(x + bx, y + by, erase); }
        if (x == ex && y == ey) { break; }
        const int twice = error * 2;
        if (twice >= dy) { error += dy; x += sx; }
        if (twice <= dx) { error += dx; y += sy; }
    }
}

void LevelEditor::PaintCell(int x, int y, bool erase) {
    auto& region = model_.draft.region;
    if (x < 0 || y < 0 || x >= region.cols || y >= region.rows) { return; }
    const auto index = CellIndex(region, x, y);
    if (erase) {
        if (erasedDeviceCells_.contains(index)) { return; }
        const int hit = model_.devices.Hit(x + .5, y + .5);
        if (hit >= 0) {
            model_.devices.Select(hit); model_.devices.RemoveSelected(); model_.draft.devices = model_.devices.Serialize();
            erasedDeviceCells_.insert(index); return;
        }
    }
    region.cells[index] = erase ? '0' : '1';
}

void LevelEditor::ApplyTemplate(DeviceDraft& d, int cols, int rows) const {
    const auto found = templates_.find(d.kind);
    if (found == templates_.end()) { return; }
    const auto& t = found->second;
    d.w = std::clamp(t.w, .125, cols - d.x); d.h = std::clamp(t.h, .125, rows - d.y);
    d.direction = t.direction; d.range = t.range; d.inverted = t.inverted; d.latch = t.latch; d.extra = t.extra;
}

// Delete は地図の上にマウスがある時だけ（3D ビューポートの Delete は物体を消すので、同じキーで装置まで消さない）。
void LevelEditor::HandleDelete(EditorWorkspaceHostContext& ctx) {
    const auto interaction = ctx.ui->GetInteraction();
    if (!canvasHovered_ || !model_.ready || model_.gesture || interaction.editing || !interaction.deletePressed) { return; }
    if (focus_ == Focus::Device) {
        if (model_.devices.RemoveSelected()) { model_.draft.devices = model_.devices.Serialize(); model_.Commit(ctx); focus_ = Focus::Room; }
        return;
    }
    if (focus_ == Focus::Room && tool_ == Tool::Select && multi_.empty()) { model_.draft.worldPlaced = false; model_.Commit(ctx); }
}

// 試遊：押した所（人物の足元の中央）から始める。壁の中は赤い枠で示し、別の場所へは移さない。
void LevelEditor::DrawSpawnPicker(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint point) {
    if (!pickingStart_ || !input.hovered || view_.panning) { return; }
    if (input.pressedButtons & kSecondary) { pickingStart_ = false; return; }
    for (const auto& room : model_.rooms) {
        const auto& s = room.settings;
        if (!s.worldPlaced || !ContainsWorldPoint(Placement(s), {point.x, point.y})) { continue; }
        const PreviewSpawn spawn{double(point.x) - s.worldX - kPlayerWidth * .5, double(point.y) - s.worldY - kPlayerHeight};
        bool clear = spawn.x >= 0 && spawn.y >= 0 && spawn.x + kPlayerWidth <= s.region.cols && spawn.y + kPlayerHeight <= s.region.rows && ValidateRegion(s.region).valid;
        if (clear) {
            for (int y = static_cast<int>(std::floor(spawn.y)); y < static_cast<int>(std::ceil(spawn.y + kPlayerHeight)); ++y)
                for (int x = static_cast<int>(std::floor(spawn.x)); x < static_cast<int>(std::ceil(spawn.x + kPlayerWidth)); ++x)
                    if (s.region.cells[CellIndex(s.region, x, y)] == '1') { clear = false; }
        }
        const auto a = view_.Screen(point.x - float(kPlayerWidth) * .5f, point.y - float(kPlayerHeight)), b = view_.Screen(point.x + float(kPlayerWidth) * .5f, point.y);
        const EditorWorkspaceColor color = clear ? EditorWorkspaceColor{.4f, 1, .7f, 1} : EditorWorkspaceColor{1, .3f, .3f, 1};
        ctx.ui->CanvasRect(a, b, color, 2);
        ctx.ui->CanvasCircle(view_.Screen(point.x, point.y - .7f), view_.tile * .13f, color, 2);
        if (input.pressedButtons & kPrimary) {
            if (!clear) { model_.error = "ここには人物が入りません"; return; }
            StartPreview(ctx, room.object.objectId, spawn);
        }
        return;
    }
}
}
