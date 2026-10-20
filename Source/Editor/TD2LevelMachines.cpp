#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
namespace {
constexpr EditorWorkspaceColor kMagenta{.93f, .42f, .74f, 1}, kFaint{.93f, .42f, .74f, .35f};

// 配方の棚の記号（線だけの簡単な絵）。
void RecipeIcon(IEditorWorkspaceUi& ui, RecipeKind kind, EditorWorkspacePoint size) {
    const float cx = size.x * .5f, cy = (size.y - 18) * .5f, r = (std::min)(size.x, size.y - 18) * .32f;
    const auto line = [&](float x1, float y1, float x2, float y2) { ui.CanvasLine({cx + x1 * r, cy + y1 * r}, {cx + x2 * r, cy + y2 * r}, kMagenta, 2); };
    const auto box = [&](float x, float y, float w, float h) { ui.CanvasRect({cx + (x - w) * r, cy + (y - h) * r}, {cx + (x + w) * r, cy + (y + h) * r}, {.75f, .85f, .86f, 1}, 1.5f); };
    switch (kind) {
    case RecipeKind::Lift: box(0, .35f, .6f, .4f); line(0, -.1f, 0, -1); line(-.25f, -.7f, 0, -1); line(.25f, -.7f, 0, -1); break;
    case RecipeKind::Turn: box(0, 0, .55f, .4f); ui.CanvasCircle({cx, cy}, r * .95f, kMagenta, 2); break;
    case RecipeKind::Balance: box(-.6f, .45f, .35f, .3f); box(.6f, -.1f, .35f, .3f); line(-1, -.8f, 1, -.8f); line(-.6f, -.8f, -.6f, .15f); line(.6f, -.8f, .6f, -.4f); break;
    case RecipeKind::Shuttle: box(0, 0, .45f, .35f); line(-1, .7f, 1, .7f); line(-1, .7f, -.75f, .5f); line(1, .7f, .75f, .5f); break;
    case RecipeKind::Flip: box(0, 0, .55f, .4f); line(-.3f, -.1f, .3f, .1f); ui.CanvasCircle({cx, cy}, r * .2f, kMagenta, 2); line(.6f, -.7f, .9f, -.4f); break;
    case RecipeKind::Slide: box(-.5f, -.35f, .4f, .3f); box(.5f, -.35f, .4f, .3f); box(-.5f, .35f, .4f, .3f); line(.2f, .35f, .8f, .35f); break;
    case RecipeKind::Stack: box(0, -.6f, .5f, .25f); box(0, 0, .5f, .25f); box(0, .6f, .5f, .25f); line(.7f, .6f, 1, .6f); break;
    case RecipeKind::Carousel: ui.CanvasCircle({cx, cy}, r * .5f, {.75f, .85f, .86f, 1}, 1.5f); box(0, -.85f, .35f, .12f); box(.85f, 0, .12f, .35f); box(0, .85f, .35f, .12f); box(-.85f, 0, .12f, .35f); break;
    default: break;
    }
}

std::string NewMachineId(const std::vector<Machine>& machines, MachineKind kind, std::string_view base) {
    for (int n = 1;; ++n) {
        const auto id = std::string(base) + std::to_string(n);
        if (std::none_of(machines.begin(), machines.end(), [&](const Machine& m) { return m.kind == kind && m.id == id; })) { return id; }
    }
}
}

Machine* LevelEditor::SelectedMachine() {
    const auto it = std::find_if(model_.machines.begin(), model_.machines.end(), [this](const Machine& m) { return m.objectId == machineObject_ && m.componentId == machineComponent_; });
    return it == model_.machines.end() ? nullptr : &*it;
}

std::optional<RegionPlacement> LevelEditor::PlacementOf(const std::string& regionId) const {
    for (const auto& room : model_.rooms) { if (room.settings.region.regionId == regionId) { return Placement(SettingsOf(room)); } }
    return std::nullopt;
}

std::string LevelEditor::RoomName(const std::string& regionId) const {
    for (const auto& room : model_.rooms) { if (room.settings.region.regionId == regionId) { return room.object.name; } }
    return regionId;
}

void LevelEditor::RefreshCheckInput(EditorWorkspaceHostContext& ctx) {
    checkInputClock_ += ctx.unscaledDeltaSeconds;
    if (checkInputClock_ < 1 || (checkInputPulse_ == model_.documentPulse && checkInputClock_ < 30)) { return; }
    checkInputClock_ = 0; checkInputPulse_ = model_.documentPulse;
    checkInput_ = {};
    checkInput_.placements = model_.Layout();
    checkInput_.plateTargets = model_.PlateTargets();
    objectNames_.clear();
    std::string ignored;
    (void)QueryAll(ctx, "scene.objects.query", Json::object(), "objects", [this](const Json& object) {
        const auto id = ObjectId(object.value("objectId", Json{}));
        if (!id) { return true; }
        checkInput_.sceneObjects.push_back(id);
        objectNames_[id] = object.value("name", "");
        bool model = false;
        for (const auto& c : object.value("components", Json::array())) { model = model || c.value("type", "") == "ModelComponent"; }
        checkInput_.objectHasModel[id] = model;
        return true;
    }, ignored);
}

// 仕掛けの印：最初のテレビの右上の角。同じテレビに仕掛けが重なる時は左へずらす。
std::optional<EditorWorkspacePoint> LevelEditor::MachineIcon(const Machine& m) const {
    const auto regions = m.Regions();
    if (regions.empty()) { return std::nullopt; }
    const auto p = PlacementOf(regions.front());
    if (!p || !p->worldPlaced) { return std::nullopt; }
    int index = 0;
    for (const auto& other : model_.machines) {
        if (other.objectId == m.objectId && other.componentId == m.componentId) { break; }
        const auto r = other.Regions();
        if (!r.empty() && r.front() == regions.front()) { ++index; }
    }
    const float k = 1.4f / (std::max)(.2f, view_.tile / 20.0f);
    return EditorWorkspacePoint{static_cast<float>(p->worldX + p->cols) - .9f * k - index * 1.6f * k, static_cast<float>(p->worldY) + .9f * k};
}

void LevelEditor::DrawMachineIcons(IEditorWorkspaceUi& ui) {
    const auto* selected = SelectedMachine();
    for (const auto& m : model_.machines) {
        const auto icon = MachineIcon(m);
        if (!icon) { continue; }
        const auto at = view_.Screen(icon->x, icon->y);
        const bool chosen = selected && selected->objectId == m.objectId && selected->componentId == m.componentId;
        const float radius = tool_ == Tool::Machine || tool_ == Tool::Link ? 10.f : 6.f;
        ui.CanvasFill({at.x - radius, at.y - radius}, {at.x + radius, at.y + radius}, {.10f, .05f, .09f, .9f});
        ui.CanvasCircle(at, radius, chosen ? EditorWorkspaceColor{.97f, .80f, .40f, 1} : kMagenta, chosen ? 3.f : 2.f);
        if (tool_ == Tool::Machine && view_.tile >= 6) { ui.CanvasText({at.x + radius + 3, at.y - 8}, kMagenta, RecipeLabel(m)); }
    }
}

// 状態の位置：点線の枠と元の位置からの線。選んだ仕掛けのテレビは太く縁取る。
void LevelEditor::DrawMachineOverlay(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input) {
    const Machine* m = ghostPreview_ ? &*ghostPreview_ : SelectedMachine();
    for (const auto& machine : model_.machines) {
        if (m && machine.objectId == m->objectId && machine.componentId == m->componentId) { continue; }
        for (const auto& id : machine.Regions()) {
            const auto p = PlacementOf(id);
            if (!p || !p->worldPlaced) { continue; }
            ui.CanvasRect(view_.Screen(static_cast<float>(p->worldX), static_cast<float>(p->worldY)), view_.Screen(static_cast<float>(p->worldX + p->cols), static_cast<float>(p->worldY + p->rows)), kFaint, 1);
        }
    }
    // 配方を選んで、作る先のテレビを探している時。
    if (recipe_ >= 0 && input.hovered) {
        if (auto* room = RoomAt(view_.World(input.pointer))) {
            const auto& s = SettingsOf(*room);
            ui.CanvasRect(view_.Screen(static_cast<float>(s.worldX), static_cast<float>(s.worldY)), view_.Screen(static_cast<float>(s.worldX + s.region.cols), static_cast<float>(s.worldY + s.region.rows)), kMagenta, 3);
        }
        if (!recipeFirst_.empty()) {
            if (const auto p = PlacementOf(recipeFirst_)) { ui.CanvasRect(view_.Screen(static_cast<float>(p->worldX), static_cast<float>(p->worldY)), view_.Screen(static_cast<float>(p->worldX + p->cols), static_cast<float>(p->worldY + p->rows)), kMagenta, 3); }
        }
    }
    if (!m) { return; }
    const auto rect = [&](const RegionPlacement& p, EditorWorkspaceColor color, float width) {
        ui.CanvasRect(view_.Screen(static_cast<float>(p.worldX), static_cast<float>(p.worldY)), view_.Screen(static_cast<float>(p.worldX + p.cols), static_cast<float>(p.worldY + p.rows)), color, width);
    };
    const auto label = [&](const RegionPlacement& p, const std::string& text) {
        if (view_.tile >= 4) { ui.CanvasText(view_.Screen(static_cast<float>(p.worldX) + .3f, static_cast<float>(p.worldY + p.rows) - .2f - 16 / (std::max)(1.f, view_.tile)), kMagenta, text); }
    };
    if (m->kind == MachineKind::Mechanism) {
        for (const auto* list : {&m->offsets, &m->offsetsC}) {
            const bool current = (list == &m->offsetsC) == machineC_;
            for (const auto& o : *list) {
                const auto self = PlacementOf(o.regionId);
                if (!self) { continue; }
                if (self->worldPlaced) { rect(*self, current ? kMagenta : kFaint, current ? 3.f : 1.f); }
                const auto base = PlacementOf(o.anchor.empty() ? o.regionId : o.anchor);
                if (o.park) { if (current && self->worldPlaced) { label(*self, "B：离开世界"); } continue; }
                if (!base || !base->worldPlaced) { continue; }
                RegionPlacement ghost = *self; ghost.worldX = base->worldX + o.dx; ghost.worldY = base->worldY + o.dy;
                const auto a = view_.Screen(static_cast<float>(ghost.worldX), static_cast<float>(ghost.worldY));
                const auto b = view_.Screen(static_cast<float>(ghost.worldX + ghost.cols), static_cast<float>(ghost.worldY + ghost.rows));
                const auto color = current ? kMagenta : kFaint;
                for (float t = 0; t < 1; t += .1f) {
                    ui.CanvasLine({a.x + (b.x - a.x) * t, a.y}, {a.x + (b.x - a.x) * (t + .05f), a.y}, color, 2);
                    ui.CanvasLine({a.x + (b.x - a.x) * t, b.y}, {a.x + (b.x - a.x) * (t + .05f), b.y}, color, 2);
                    ui.CanvasLine({a.x, a.y + (b.y - a.y) * t}, {a.x, a.y + (b.y - a.y) * (t + .05f)}, color, 2);
                    ui.CanvasLine({b.x, a.y + (b.y - a.y) * t}, {b.x, a.y + (b.y - a.y) * (t + .05f)}, color, 2);
                }
                if (current) { ui.CanvasFill(a, b, {.93f, .42f, .74f, .08f}); }
                if (self->worldPlaced && (o.dx || o.dy || !o.anchor.empty())) {
                    ui.CanvasLine(view_.Screen(self->worldX + self->cols * .5f, self->worldY + self->rows * .5f), {(a.x + b.x) * .5f, (a.y + b.y) * .5f}, color, 1.5f);
                }
                if (current) { label(ghost, std::string(machineC_ ? "C" : "B") + (o.flip ? " · 翻转" : "")); }
            }
        }
        return;
    }
    for (size_t i = 0; i < m->members.size(); ++i) {
        const auto p = PlacementOf(m->members[i]);
        if (!p || !p->worldPlaced) { continue; }
        rect(*p, kMagenta, 3);
        std::string text = std::to_string(i + 1);
        if (m->kind == MachineKind::Stack) { text = i == 0 ? "底 1" : std::to_string(i + 1); }
        if (m->kind == MachineKind::Carousel && m->members.size() == 4) { constexpr std::array<std::string_view, 4> names{"正面", "右", "背面", "左"}; text = std::string(names[i]); }
        label(*p, text);
    }
    if (m->kind == MachineKind::Slide && !m->members.empty()) {
        if (const auto first = PlacementOf(m->members.front())) {
            for (const auto& [dx, dy] : m->emptySlots) { RegionPlacement slot = *first; slot.worldX += dx; slot.worldY += dy; rect(slot, kFaint, 2); label(slot, "空槽"); }
        }
    }
}

// 仕掛けの道具で押した時。印を押すとその仕掛けを選ぶ。配方を選んでいればテレビを押して作る。
bool LevelEditor::PressMachine(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    const auto pointer = input.pointer;
    for (const auto& m : model_.machines) {
        const auto icon = MachineIcon(m);
        if (!icon) { continue; }
        const auto at = view_.Screen(icon->x, icon->y);
        if (std::hypot(pointer.x - at.x, pointer.y - at.y) < 12) {
            if (previewState_ != 0) { PreviewState(ctx, 0); }
            machineObject_ = m.objectId; machineComponent_ = m.componentId; machineIdDraft_ = m.id; machineC_ = false; confirmDelete_ = false;
            focus_ = Focus::Machine; recipe_ = -1;
            return true;
        }
    }
    auto* room = RoomAt(p);
    const std::string region = room ? room->settings.region.regionId : std::string{};
    if (recipe_ >= 0) { if (!region.empty()) { CreateFromRecipe(ctx, region); } return true; }
    auto* m = SelectedMachine();
    if (m && !posing_ && m->kind == MachineKind::Mechanism && previewState_ == 0) {
        // 状態の点線の枠をつかむ。
        const auto& offsets = machineC_ ? m->offsetsC : m->offsets;
        for (size_t i = 0; i < offsets.size(); ++i) {
            const auto& o = offsets[i];
            const auto self = PlacementOf(o.regionId); const auto base = PlacementOf(o.anchor.empty() ? o.regionId : o.anchor);
            if (o.park || !self || !base || !base->worldPlaced) { continue; }
            RegionPlacement ghost = *self; ghost.worldPlaced = true; ghost.worldX = base->worldX + o.dx; ghost.worldY = base->worldY + o.dy;
            if (ContainsWorldPoint(ghost, {p.x, p.y}) && (o.dx || o.dy || !o.anchor.empty() || !self->worldPlaced)) {
                ghost_ = static_cast<int>(i); ghostC_ = machineC_; ghostGrab_ = p; ghostDx_ = o.dx; ghostDy_ = o.dy; drag_ = Drag::Ghost;
                return true;
            }
        }
    }
    // Shift+クリックで、選んだ仕掛けにテレビを入れる・外す。
    if (m && !region.empty() && input.shift && !posing_) { if (previewState_ != 0) { PreviewState(ctx, 0); } ToggleMember(ctx, region); return true; }
    if (m && region.empty() && m->kind == MachineKind::Slide && !m->members.empty()) {
        // スロット棚：空いた格子を空きスロットにする・外す（一番目のメンバーの大きさの格子に合わせる）。
        const auto first = PlacementOf(m->members.front());
        if (!first) { return true; }
        const int w = (std::max)(1, first->cols), h = (std::max)(1, first->rows);
        const int ox = static_cast<int>(std::floor((p.x - first->worldX) / w)) * w, oy = static_cast<int>(std::floor((p.y - first->worldY) / h)) * h;
        auto after = *m;
        const auto found = std::find(after.emptySlots.begin(), after.emptySlots.end(), std::pair{ox, oy});
        if (found != after.emptySlots.end()) { after.emptySlots.erase(found); } else { after.emptySlots.push_back({ox, oy}); }
        WriteSelectedMachine(ctx, after);
        return true;
    }
    if (!region.empty()) {
        // そのテレビを含む仕掛けを選ぶ（今選んでいるものを先に）。無ければ普通の選択へ。
        if (m) { const auto r = m->Regions(); if (std::find(r.begin(), r.end(), region) != r.end()) { focus_ = Focus::Machine; return false; } }
        for (const auto& machine : model_.machines) {
            const auto r = machine.Regions();
            if (std::find(r.begin(), r.end(), region) == r.end()) { continue; }
            if (previewState_ != 0) { PreviewState(ctx, 0); }
            machineObject_ = machine.objectId; machineComponent_ = machine.componentId; machineIdDraft_ = machine.id; machineC_ = false;
            focus_ = Focus::Machine;
            return false;
        }
    }
    // 仕掛けの無いテレビか空いた所：選択を外す。
    if (previewState_ != 0) { PreviewState(ctx, 0); }
    machineObject_ = machineComponent_ = 0;
    return false;
}

void LevelEditor::DragGhost(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    auto* m = SelectedMachine();
    if (ghost_ < 0 || !m) { ghost_ = -1; ghostPreview_.reset(); drag_ = Drag::None; return; }
    auto after = *m;
    auto& offsets = ghostC_ ? after.offsetsC : after.offsets;
    if (ghost_ >= static_cast<int>(offsets.size())) { ghost_ = -1; ghostPreview_.reset(); drag_ = Drag::None; return; }
    auto& o = offsets[static_cast<size_t>(ghost_)];
    o.dx = ghostDx_ + static_cast<int>(std::round(p.x - ghostGrab_.x));
    o.dy = ghostDy_ + static_cast<int>(std::round(p.y - ghostGrab_.y));
    // 天平：もう一台は逆へ、反対側の状態は鏡にする。
    if (RecipeOf(after) == RecipeKind::Balance && after.offsets.size() == 2 && after.offsetsC.size() == 2) {
        const auto& moved = offsets[static_cast<size_t>(ghost_)];
        auto& other = offsets[1 - static_cast<size_t>(ghost_)];
        other.dx = 0; other.dy = -moved.dy; offsets[static_cast<size_t>(ghost_)].dx = 0;
        auto& mirror = ghostC_ ? after.offsets : after.offsetsC;
        for (size_t i = 0; i < 2; ++i) { mirror[i].dx = 0; mirror[i].dy = -offsets[i].dy; }
    }
    ghostPreview_ = after;
    if ((input.releasedButtons & kPrimary) || !(input.heldButtons & kPrimary)) {
        ghost_ = -1; ghostPreview_.reset(); drag_ = Drag::None;
        if (RecipeOf(after) != RecipeKind::Custom) { ApplyRecipe(ctx, *m, after, false); } else { WriteSelectedMachine(ctx, after); }
    }
}

bool LevelEditor::WriteSelectedMachine(EditorWorkspaceHostContext& ctx, const Machine& after) {
    const auto* before = SelectedMachine();
    if (!before) { return false; }
    std::string error;
    if (!WriteMachine(ctx, *before, after, error)) { model_.error = error; return false; }
    MarkSceneChanged(); model_.machinesStale = true; RefreshMachines(ctx); checkInputClock_ = 10;
    model_.error.clear();
    return true;
}

// 配方の機構を書く：地図のずれから 3D の動きを作り、部品を置き直し、足りない部品を置く。
bool LevelEditor::ApplyRecipe(EditorWorkspaceHostContext& ctx, const Machine& before, Machine after, bool rebuildParts) {
    if (!EnsureLayout(ctx, true)) { return false; }
    if (rebuildParts) {
        for (const auto& part : PlanParts(after, layout_)) {
            const uint64_t id = CreateStageModel(ctx, part.path, part.label + " " + after.id, part.pose, model_.error);
            if (!id) { return false; }
            if (const auto* room = layout_.Room(part.region)) { JoinFolderOf(ctx, id, room->tvObject); }
            after.parts.push_back({id, part.region, part.role, part.moves, true});
        }
        if (!EnsureLayout(ctx, true)) { return false; }
    }
    RebuildMotion(after, layout_);
    std::string error;
    if (!WriteMachine(ctx, before, after, error)) { model_.error = error; return false; }
    for (const auto& [object, pose] : AdjustParts(after, layout_)) { (void)SetStagePose(ctx, object, pose, model_.error); }
    MarkSceneChanged(); model_.machinesStale = true; RefreshMachines(ctx); checkInputClock_ = 10;
    return true;
}

void LevelEditor::CreateFromRecipe(EditorWorkspaceHostContext& ctx, const std::string& region) {
    if (recipe_ < 0 || recipe_ >= static_cast<int>(RecipeList().size())) { return; }
    const auto& info = RecipeList()[static_cast<size_t>(recipe_)];
    if (info.televisions == 2 && recipeFirst_.empty()) { recipeFirst_ = region; SetNote("再点第二台电视"); return; }
    if (info.televisions == 2 && recipeFirst_ == region) { return; }
    if (!model_.Commit(ctx) || !EnsureLayout(ctx, true)) { return; }
    RefreshMachines(ctx, true);
    const MachineKind kind = info.kind == RecipeKind::Slide ? MachineKind::Slide : info.kind == RecipeKind::Stack ? MachineKind::Stack
        : info.kind == RecipeKind::Carousel ? MachineKind::Carousel : MachineKind::Mechanism;
    const auto id = NewMachineId(model_.machines, kind, info.id);
    const auto* room = layout_.Room(region);
    if (!room) { return; }
    uint64_t host = 0;
    if (kind == MachineKind::Carousel) {
        // 転柱は柱の物体に付ける：テレビの後ろに台と柱を置く。
        Pose column; column.position = room->tv.position; column.rotation = {0, room->tv.rotation[1], 0};
        const auto back = Math::QRotate(Math::QFromEulerDeg(column.rotation), {0, 0, -1.4});
        column.position = {column.position[0] + back[0], 0, column.position[2] + back[2]};
        const uint64_t base = CreateStageModel(ctx, "TD2Props/SM_TD2_CarouselBase.gltf", "转柱底座 " + id, column, model_.error);
        host = CreateStageModel(ctx, "TD2Props/SM_TD2_CarouselColumn.gltf", "转柱 " + id, column, model_.error);
        if (!base || !host) { return; }
        JoinFolderOf(ctx, base, room->tvObject); JoinFolderOf(ctx, host, room->tvObject);
    }
    std::string error;
    if (!CreateMachine(ctx, kind, host, id, error)) { model_.error = error; return; }
    RefreshMachines(ctx, true);
    const auto created = std::find_if(model_.machines.begin(), model_.machines.end(), [&](const Machine& m) { return m.kind == kind && m.id == id; });
    if (created == model_.machines.end()) { model_.error = "机构创建后读不到"; return; }
    machineObject_ = created->objectId; machineComponent_ = created->componentId; machineIdDraft_ = id; machineC_ = false;
    if (!host) { JoinFolderOf(ctx, created->objectId, room->tvObject); }
    const Machine before = *created;
    Machine after = before;
    std::vector<std::string> regions{region};
    if (info.televisions == 2) { regions.insert(regions.begin(), recipeFirst_); }
    if (kind == MachineKind::Mechanism) {
        InitRecipe(after, info.kind, regions, layout_);
        ApplyRecipe(ctx, before, after, true);
    } else {
        after.members = regions;
        WriteSelectedMachine(ctx, after);
    }
    recipe_ = -1; recipeFirst_.clear(); focus_ = Focus::Machine;
    SetNote("已建「" + std::string(info.label) + "」：用「连线」把压板拖到它的图标");
}

void LevelEditor::ToggleMember(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
    auto* m = SelectedMachine();
    if (!m) { return; }
    auto after = *m;
    if (after.kind == MachineKind::Mechanism) {
        auto& offsets = machineC_ ? after.offsetsC : after.offsets;
        const auto found = std::find_if(offsets.begin(), offsets.end(), [&](const auto& o) { return o.regionId == regionId; });
        if (found != offsets.end()) { offsets.erase(found); } else { offsets.push_back({regionId}); }
    } else {
        const auto found = std::find(after.members.begin(), after.members.end(), regionId);
        if (found != after.members.end()) { after.members.erase(found); } else { after.members.push_back(regionId); }
        // テレビの塔は世界地図の下（Y が大きい）から上へ並べる。
        if (after.kind == MachineKind::Stack) {
            std::stable_sort(after.members.begin(), after.members.end(), [this](const std::string& a, const std::string& b) {
                const auto pa = PlacementOf(a); const auto pb = PlacementOf(b);
                return (pa ? pa->worldY : 0) > (pb ? pb->worldY : 0);
            });
        }
        // スロット棚の一番目が変わったら、空きスロットのずれを新しい一番目から測り直す。
        if (after.kind == MachineKind::Slide && !m->members.empty() && !after.members.empty() && m->members.front() != after.members.front()) {
            const auto oldFirst = PlacementOf(m->members.front()); const auto newFirst = PlacementOf(after.members.front());
            if (oldFirst && newFirst) { for (auto& [x, y] : after.emptySlots) { x += oldFirst->worldX - newFirst->worldX; y += oldFirst->worldY - newFirst->worldY; } }
        }
    }
    if (RecipeOf(after) != RecipeKind::Custom && after.kind == MachineKind::Mechanism) { ApplyRecipe(ctx, *m, after, true); }
    else { WriteSelectedMachine(ctx, after); }
}

void LevelEditor::TogglePlateTarget(EditorWorkspaceHostContext& ctx, const std::string& plate, const std::string& target) {
    const auto slash = plate.find('/');
    if (slash == std::string::npos) { return; }
    const auto region = plate.substr(0, slash), id = plate.substr(slash + 1);
    for (const auto& room : model_.rooms) {
        if (room.settings.region.regionId != region) { continue; }
        const auto object = room.object.objectId;
        if (!SelectRoom(ctx, object)) { return; }
        if (!model_.devices.ToggleTarget(id, target)) { model_.error = "连接失败：" + target; return; }
        model_.draft.devices = model_.devices.Serialize();
        if (model_.Commit(ctx)) {
            for (size_t i = 0; i < model_.devices.Items().size(); ++i) { if (model_.devices.Items()[i].id == id) { model_.devices.Select(static_cast<int>(i)); } }
            focus_ = Focus::Device;
            const bool linked = std::find(model_.devices.Selected()->targets.begin(), model_.devices.Selected()->targets.end(), target) != model_.devices.Selected()->targets.end();
            SetNote(linked ? "已连接" : "已断开");
            // 仕掛けへつないだら、信号線も引く。
            if (linked && IsMachineTarget(target)) { RouteCables(ctx, plate, target); }
            checkInputClock_ = 10;
        }
        return;
    }
}

// ID を変えたら、その仕掛けを目標にしている圧力板の目標も同じ取り消しの中で書き換える。
bool LevelEditor::RenameMachine(EditorWorkspaceHostContext& ctx, const Machine& before, const std::string& id) {
    if (!IsStableId(id)) { model_.error = "ID 只能用英文字母、数字、点、短横线和下划线"; return false; }
    if (model_.Dirty() && !model_.Commit(ctx)) { return false; }
    auto after = before; after.id = id;
    const auto oldReceivers = before.Receivers(), newReceivers = after.Receivers();
    Json commands = Json::array();
    for (const auto& room : model_.rooms) {
        DeviceEditor devices; devices.Load(room.settings.devices);
        bool changed = false;
        for (const auto& device : devices.Items()) for (const auto& target : device.targets) {
            for (size_t i = 0; i < oldReceivers.size() && i < newReceivers.size(); ++i) {
                if (target != oldReceivers[i].first) { continue; }
                devices.ToggleTarget(device.id, target); devices.ToggleTarget(device.id, newReceivers[i].first); changed = true;
            }
        }
        if (changed) { commands.push_back(PropertyCommand(room.target.objectId, room.target.componentId, "devices", devices.Serialize())); }
    }
    const auto idField = before.kind == MachineKind::Mechanism ? "mechanismId" : before.kind == MachineKind::Slide ? "rackId" : before.kind == MachineKind::Stack ? "stackId" : "carouselId";
    commands.push_back(PropertyCommand(before.objectId, before.componentId, idField, id));
    if (!Transact(ctx, commands, model_.error)) { return false; }
    MarkSceneChanged(); model_.machinesStale = true; model_.refresh = true; model_.Poll(ctx, true); RefreshMachines(ctx);
    return true;
}

// ---------------------------------------------------------------- 状態の見本と、手作りの姿勢
bool LevelEditor::ReadPoses(EditorWorkspaceHostContext& ctx, std::map<uint64_t, Pose>& poses, std::map<uint64_t, Json>& expected, std::string& sceneId, uint64_t& revision) {
    const auto layout = Execute(ctx, "scene.layout.query", Json::object());
    if (!layout.ok) { model_.error = layout.error; return false; }
    sceneId = layout.data.value("sceneId", ""); revision = layout.data.value("documentRevision", uint64_t{0});
    const auto vec = [](const Json& v, std::array<double, 3> fallback) {
        if (!v.is_array() || v.size() != 3) { return fallback; }
        return std::array<double, 3>{v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
    };
    for (const auto& object : layout.data.value("objects", Json::array())) {
        const auto id = ObjectId(object.value("objectId", Json{}));
        const auto t = object.value("transform", Json::object());
        Pose pose; pose.position = vec(t.value("position", Json{}), {0, 0, 0});
        pose.rotation = vec(t.contains("rotation") ? t["rotation"] : t.value("rotationEulerDeg", Json{}), {0, 0, 0});
        pose.scale = vec(t.value("scale", Json{}), {1, 1, 1});
        poses[id] = pose; expected[id] = t;
    }
    return true;
}

bool LevelEditor::PreviewState(EditorWorkspaceHostContext& ctx, int state) {
    if (posing_) { return false; }
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return false; }
    Json commands = Json::array();
    // まず A へ戻す。
    for (const auto& [id, a] : poseA_) {
        if (expected.contains(id)) { commands.push_back({{"objectId", id}, {"expected", expected[id]}, {"position", a.position}, {"rotation", a.rotation}}); }
    }
    if (!commands.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", commands}});
        if (!applied.ok) { model_.error = applied.error; return false; }
        if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return false; }
    }
    poseA_.clear(); previewState_ = 0;
    auto* m = SelectedMachine();
    if (state == 0 || !m || m->kind != MachineKind::Mechanism) { return true; }
    const auto& deltas = state < 0 ? m->deltasC : m->deltas;
    commands = Json::array();
    for (const auto& d : deltas) {
        const auto a = poses.find(d.object);
        if (a == poses.end()) { continue; }
        poseA_[d.object] = a->second;
        const auto base = d.anchor ? poses.find(d.anchor) : a;
        if (base == poses.end()) { continue; }
        const auto b = ApplyDelta(a->second, base->second, d);
        commands.push_back({{"objectId", d.object}, {"expected", expected[d.object]}, {"position", b.position}, {"rotation", b.rotation}});
    }
    if (!commands.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", commands}});
        if (!applied.ok) { model_.error = applied.error; poseA_.clear(); return false; }
    }
    previewState_ = state;
    return true;
}

// 手作りの機構：状態 B（または天平の状態 C）をシーンに置き、作者がビューポートで動かしてから保存で相対の姿勢に直す。
bool LevelEditor::BeginPose(EditorWorkspaceHostContext& ctx) {
    if (previewState_ != 0) { PreviewState(ctx, 0); }
    auto* m = SelectedMachine();
    if (!m || m->kind != MachineKind::Mechanism) { return false; }
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return false; }
    const auto& deltas = machineC_ ? m->deltasC : m->deltas;
    Json commands = Json::array();
    poseA_.clear();
    for (const auto& d : deltas) {
        const auto a = poses.find(d.object);
        if (a == poses.end()) { model_.error = "物体不存在：" + std::to_string(d.object); return false; }
        poseA_[d.object] = a->second;
        const auto base = d.anchor ? poses.find(d.anchor) : a;
        if (base == poses.end()) { model_.error = "锚点物体不存在：" + std::to_string(d.anchor); return false; }
        const auto b = ApplyDelta(a->second, base->second, d);
        commands.push_back({{"objectId", d.object}, {"expected", expected[d.object]}, {"position", b.position}, {"rotation", b.rotation}});
    }
    if (!commands.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", commands}});
        if (!applied.ok) { model_.error = applied.error; return false; }
    }
    posing_ = true; posingC_ = machineC_; model_.error.clear();
    return true;
}

bool LevelEditor::SavePose(EditorWorkspaceHostContext& ctx) {
    auto* m = SelectedMachine();
    if (!posing_ || !m) { posing_ = false; return false; }
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return false; }
    auto after = *m;
    auto& deltas = posingC_ ? after.deltasC : after.deltas;
    Json restore = Json::array();
    for (auto& d : deltas) {
        const auto a = poseA_.find(d.object); const auto b = poses.find(d.object);
        if (a == poseA_.end() || b == poses.end()) { continue; }
        Pose base = a->second;
        if (d.anchor) { base = poseA_.contains(d.anchor) ? poseA_[d.anchor] : poses.contains(d.anchor) ? poses[d.anchor] : base; }
        d = MakeDelta(a->second, base, b->second, d);
        restore.push_back({{"objectId", d.object}, {"expected", expected[d.object]}, {"position", a->second.position}, {"rotation", a->second.rotation}});
    }
    if (!restore.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", restore}});
        if (!applied.ok) { model_.error = "状态B已读取，但恢复状态A失败：" + applied.error; return false; }
    }
    posing_ = false; poseA_.clear();
    // 手で姿勢を直したら、配方の自動の動きはやめる（手作りの機構にする）。
    after.recipe.clear();
    return WriteSelectedMachine(ctx, after);
}

void LevelEditor::CancelPose(EditorWorkspaceHostContext& ctx) {
    if (!posing_) { return; }
    posing_ = false;
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return; }
    Json restore = Json::array();
    for (const auto& [id, a] : poseA_) {
        if (expected.contains(id)) { restore.push_back({{"objectId", id}, {"expected", expected[id]}, {"position", a.position}, {"rotation", a.rotation}}); }
    }
    poseA_.clear();
    if (restore.empty()) { return; }
    const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", restore}});
    if (!applied.ok) { model_.error = "恢复状态A失败：" + applied.error; }
}

// ---------------------------------------------------------------- 棚と属性
void LevelEditor::DrawMachineShelf(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    const auto& recipes = RecipeList();
    if (!ui.BeginTable("td2.palette.recipes", static_cast<uint32_t>(recipes.size()))) { return; }
    ui.TableNextRow();
    for (size_t i = 0; i < recipes.size(); ++i) {
        ui.TableNextColumn();
        const auto kind = recipes[i].kind;
        if (Tile(ui, "td2.palette.recipe." + std::string(recipes[i].id), recipes[i].label, recipe_ == static_cast<int>(i), [kind](IEditorWorkspaceUi& u, EditorWorkspacePoint s) { RecipeIcon(u, kind, s); })) {
            recipe_ = recipe_ == static_cast<int>(i) ? -1 : static_cast<int>(i); recipeFirst_.clear();
            if (recipe_ >= 0) { SetNote(recipes[i].televisions == 2 ? "点两台电视" : "点一台电视"); }
        }
    }
    ui.EndTable();
}

void LevelEditor::DrawMachineInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    RefreshMachines(ctx);
    RefreshCheckInput(ctx);
    if (pendingMachine_ && !ui.GetInteraction().editing) { const auto next = *pendingMachine_; pendingMachine_.reset(); WriteSelectedMachine(ctx, next); }
    auto* m = SelectedMachine();
    if (!m) {
        ui.Text(recipe_ >= 0 ? "在地图上点电视建机构" : "机构");
        for (const auto& machine : model_.machines) {
            const auto problems = CheckMachine(machine, model_.machines, checkInput_);
            if (ui.Selectable("td2.machine.item." + std::to_string(machine.objectId) + "." + std::to_string(machine.componentId),
                              RecipeLabel(machine) + "  " + machine.id + (problems.empty() ? "" : "  ⚠"), false)) {
                machineObject_ = machine.objectId; machineComponent_ = machine.componentId; machineIdDraft_ = machine.id; machineC_ = false; confirmDelete_ = false;
                focus_ = Focus::Machine;
            }
        }
        return;
    }
    const auto kind = RecipeOf(*m);
    ui.Text(RecipeLabel(*m) + " · " + m->id);
    if (ui.Button("td2.machine.back", "← 机构列表")) { if (previewState_ != 0) { PreviewState(ctx, 0); } machineObject_ = machineComponent_ = 0; focus_ = Focus::None; return; }
    // 状態の見本：3D と地図の点線で、動いた後の様子を見る。
    if (m->kind == MachineKind::Mechanism && !posing_) {
        ui.Text("预览");
        ui.SameLine(); if (ui.Button("td2.machine.preview.a", previewState_ == 0 ? "● A" : "A")) { PreviewState(ctx, 0); machineC_ = false; }
        ui.SameLine(); if (ui.Button("td2.machine.preview.b", previewState_ == 1 ? "● B" : "B")) { PreviewState(ctx, 1); machineC_ = false; }
        if (m->balance) { ui.SameLine(); if (ui.Button("td2.machine.preview.c", previewState_ == -1 ? "● C" : "C")) { PreviewState(ctx, -1); machineC_ = true; } }
        m = SelectedMachine(); if (!m) { return; }
    }
    auto after = *m;
    bool changed = false, motion = false;
    if (m->kind == MachineKind::Mechanism && kind != RecipeKind::Custom) {
        if (!m->balance && m->dwell <= 0) {
            if (ui.Selectable("td2.machine.toggle.off", "踩住时动，松开回来", !after.toggle)) { after.toggle = false; changed = true; }
            if (ui.Selectable("td2.machine.toggle.on", "踩一下切换", after.toggle)) { after.toggle = true; changed = true; }
        }
        changed |= EditFloat(ui, "td2.machine.seconds", "秒数", after.seconds, .2f, 8);
        if (kind == RecipeKind::Turn) {
            double angle = RecipeAngle(after, layout_);
            ui.Text("转动");
            for (const double option : {90.0, 180.0, -90.0}) {
                ui.SameLine();
                if (ui.Button("td2.machine.angle." + std::to_string(static_cast<int>(option)), (std::abs(angle - option) < 1 ? "● " : "") + ShortNumber(option) + "°")) { SetRecipeAngle(after, layout_, option); changed = true; }
            }
        }
        if (kind == RecipeKind::Shuttle) {
            changed |= EditFloat(ui, "td2.machine.dwell", "两端停留（秒）", after.dwell, .2f, 20);
            if (!after.offsets.empty() && after.offsets.front().park) {
                double distance = RecipeDistance(after, layout_);
                if (EditFloat(ui, "td2.machine.distance", "向后退（米）", distance, .3f, 8)) { SetRecipeDistance(after, layout_, distance); changed = true; }
            }
        }
        if (kind == RecipeKind::Balance) { changed |= EditFloat(ui, "td2.machine.settle", "压稳多久才动（秒）", after.settle, 0, 2); }
        // 動いた先：地図で点線の枠を引く。離れる（世界から外れる）も選べる。
        if (!after.offsets.empty() && kind != RecipeKind::Balance && kind != RecipeKind::Flip) {
            auto& o = after.offsets.front();
            bool park = o.park;
            if (ui.Checkbox("td2.machine.park", kind == RecipeKind::Shuttle ? "后退时离开世界（关：在地图上拖虚线框）" : "动完离开世界", &park)) {
                o.park = park; motion = true;
                if (!park && kind == RecipeKind::Shuttle) { o.dx = 3; o.dy = 0; }
            }
        }
        if (!after.offsets.empty() && !after.offsets.front().park && kind != RecipeKind::Flip) { ui.Text("在地图上拖虚线框改动完的位置"); }
    }
    if (m->kind == MachineKind::Slide) {
        changed |= EditFloat(ui, "td2.machine.seconds", "滑动秒数", after.seconds, .2f, 4);
        ui.Text("Shift+点电视加入成员 · 点空格子设空槽");
    }
    if (m->kind == MachineKind::Carousel) {
        changed |= EditFloat(ui, "td2.machine.seconds", "每转一面秒数", after.seconds, .3f, 6);
        ui.Text("Shift+点电视加入面（顺时针：正面、右、背面、左）");
        for (size_t i = 0; i < after.members.size(); ++i) {
            ui.Text(std::to_string(i + 1) + ". " + ShortChoiceLabel(RoomName(after.members[i]), 14));
            ui.SameLine(); if (i > 0 && ui.Button("td2.machine.member.up." + std::to_string(i), "↑")) { std::swap(after.members[i], after.members[i - 1]); changed = true; }
            ui.SameLine(); if (i + 1 < after.members.size() && ui.Button("td2.machine.member.down." + std::to_string(i), "↓")) { std::swap(after.members[i], after.members[i + 1]); changed = true; }
        }
    }
    if (m->kind == MachineKind::Stack) {
        changed |= EditFloat(ui, "td2.machine.pull", "抽出秒数", after.pullSeconds, .2f, 4);
        changed |= EditFloat(ui, "td2.machine.drop", "每层下落秒数", after.dropSeconds, .1f, 2);
        changed |= EditFloat(ui, "td2.machine.reset", "复位秒数", after.resetSeconds, .5f, 6);
        ui.Text("Shift+点电视加入（自下而上）· 抽出距离（米）");
        for (size_t i = 0; i < after.dockDistances.size(); ++i) { changed |= EditFloat(ui, "td2.machine.dock." + std::to_string(i), "第 " + std::to_string(i + 1) + " 台", after.dockDistances[i], .3f, 10); }
        if (ui.Button("td2.machine.dock.add", "+ 停放位")) { after.dockDistances.push_back(after.dockDistances.empty() ? 2.4 : (std::max)(.6, after.dockDistances.back() - 1.15)); changed = true; }
        if (!after.dockDistances.empty()) { ui.SameLine(); if (ui.Button("td2.machine.dock.remove", "- 停放位")) { after.dockDistances.pop_back(); changed = true; } }
    }
    if (changed || motion) {
        if (ui.GetInteraction().editing) { pendingMachine_ = after; }
        else if (motion && RecipeOf(after) != RecipeKind::Custom && after.kind == MachineKind::Mechanism) { ApplyRecipe(ctx, *m, after, false); }
        else { WriteSelectedMachine(ctx, after); }
        m = SelectedMachine(); if (!m) { return; }
    }
    // 部品：配方が置いた物の数と、足りない部品。
    if (m->kind == MachineKind::Mechanism && kind != RecipeKind::Custom) {
        EnsureLayout(ctx);
        // 役目の名前（部品の欄は英語の短い名前で持つ）。
        const auto roleName = [](const std::string& role) -> std::string {
            constexpr std::array<std::pair<std::string_view, std::string_view>, 9> names{{{"chain", "吊链"}, {"box", "卷扬箱"}, {"beam", "天平梁"}, {"base", "转台底座"},
                {"plate", "转盘"}, {"rail", "滑轨"}, {"disk", "倒立转盘"}, {"mount", "支柱"}, {"support", "台车"}}};
            for (const auto& [id, name] : names) { if (id == role) { return std::string(name); } }
            return role;
        };
        std::map<std::string, int> roles;
        for (const auto& p : m->parts) { ++roles[roleName(p.role)]; }
        std::string text = "零件：";
        for (const auto& [role, count] : roles) { text += role + (count > 1 ? "×" + std::to_string(count) : std::string{}) + " "; }
        ui.Text(roles.empty() ? "零件：无" : text);
        if (!PlanParts(*m, layout_).empty() && ui.Button("td2.machine.parts", "补上零件")) { ApplyRecipe(ctx, *m, *m, true); return; }
    }
    // 連なる圧力板と、動かない理由。
    ui.Separator();
    const auto plates = PlatesFor(*m, checkInput_);
    if (plates.empty() && m->dwell <= 0) { ui.Text("还没有压板：用「连线」从压板拖到地图上的机构图标"); }
    for (const auto& plate : plates) {
        const auto region = plate.substr(0, plate.find('/'));
        if (ui.Selectable("td2.machine.plate." + plate, "压板 · " + ShortChoiceLabel(RoomName(region), 12))) {
            for (const auto& room : model_.rooms) { if (room.settings.region.regionId == region) { if (SelectRoom(ctx, room.object.objectId)) { FitRoom(room.settings); focus_ = Focus::Room; } return; } }
        }
    }
    for (const auto& problem : CheckMachine(*m, model_.machines, checkInput_)) { if (problem.find("还没有压板") == std::string::npos) { ui.Text("⚠ " + problem); } }
    ui.Separator();
    ui.Checkbox("td2.machine.advanced", "高级", &machineAdvanced_);
    if (machineAdvanced_) { DrawMachineAdvanced(ctx, *m); m = SelectedMachine(); if (!m) { return; } }
    ui.Separator();
    if (!confirmDelete_) { if (ui.Button("td2.machine.delete", "删除机构")) { confirmDelete_ = true; } }
    else {
        ui.Text("连到它的压板会断开，配方放的零件一起删除。");
        if (ui.Button("td2.machine.delete.confirm", "确认删除")) {
            if (previewState_ != 0) { PreviewState(ctx, 0); }
            if (posing_) { CancelPose(ctx); }
            const auto parts = m->parts;
            std::string error;
            if (!RemoveMachine(ctx, *m, error)) { model_.error = error; }
            else {
                for (const auto& p : parts) { if (p.owned) { Execute(ctx, "scene.delete_object", {{"objectId", p.object}}); } }
                MarkSceneChanged(); model_.machinesStale = true; machineObject_ = machineComponent_ = 0; RefreshMachines(ctx); focus_ = Focus::None;
            }
            confirmDelete_ = false;
            return;
        }
        ui.SameLine(); if (ui.Button("td2.machine.delete.cancel", "取消")) { confirmDelete_ = false; }
    }
}

// 高級：機構の生の欄（地図のずれ・錨・物体の姿勢・カメラの動き）。配方の機構でも開ける。
void LevelEditor::DrawMachineAdvanced(EditorWorkspaceHostContext& ctx, const Machine& m) {
    auto& ui = *ctx.ui;
    EditText(ui, "td2.machine.id", "ID（改名会同步压板）", machineIdDraft_);
    if (machineIdDraft_ != m.id && !ui.GetInteraction().editing) { if (!RenameMachine(ctx, m, machineIdDraft_)) { machineIdDraft_ = m.id; } return; }
    if (m.kind != MachineKind::Mechanism) {
        if (m.kind == MachineKind::Slide) { ui.Text("空槽（相对第一台）"); for (const auto& [x, y] : m.emptySlots) { ui.Text("· " + std::to_string(x) + ", " + std::to_string(y)); } }
        return;
    }
    auto after = m;
    bool changed = false;
    if (RecipeOf(m) != RecipeKind::Custom) {
        if (ui.Button("td2.machine.custom", "改为手动编辑（不再自动生成 3D 动作）")) { after.recipe.clear(); changed = true; }
    }
    changed |= ui.Checkbox("td2.machine.balance", "天平（两块压板对抗）", &after.balance);
    changed |= EditFloat(ui, "td2.machine.dwell", "自动往复停留（秒，0=压板驱动）", after.dwell, 0, 20);
    changed |= EditFloat(ui, "td2.machine.settle", "压稳多久才动（秒）", after.settle, 0, 2);
    changed |= EditFloat(ui, "td2.machine.lift", "移动时镜头抬高（米）", after.cameraLift, 0, 4);
    changed |= EditFloat(ui, "td2.machine.pull", "移动时镜头后拉（米）", after.cameraPull, 0, 4);
    if (after.balance) {
        if (ui.Button("td2.machine.side", machineC_ ? "改编辑状态 B" : "改编辑状态 C")) { if (posing_) { CancelPose(ctx); } machineC_ = !machineC_; }
    } else if (machineC_) { machineC_ = false; }
    ui.Text("区域（状态" + std::string(machineC_ ? "C" : "B") + "）· Shift+点电视加入");
    auto& offsets = machineC_ ? after.offsetsC : after.offsets;
    for (size_t i = 0; i < offsets.size(); ++i) {
        auto& o = offsets[i];
        const std::string key = "td2.machine.offset." + std::to_string(i);
        ui.Text(ShortChoiceLabel(RoomName(o.regionId), 14));
        changed |= ui.Checkbox(key + ".park", "离开世界", &o.park);
        if (!o.park) {
            const auto before = o;
            EditInt(ui, key + ".dx", "偏移 X", o.dx, -100000, 100000);
            EditInt(ui, key + ".dy", "偏移 Y", o.dy, -100000, 100000);
            changed |= o != before;
            changed |= ui.Checkbox(key + ".flip", "翻转 180°", &o.flip);
            ui.Text(o.anchor.empty() ? "相对：自己" : "相对：" + ShortChoiceLabel(RoomName(o.anchor), 12));
            // 錨：状態の位置で辺を接するテレビに固定すると、そのテレビを動かしても隣に付いて行く。錨を変えても状態の位置は変えない。
            const auto self = PlacementOf(o.regionId);
            const auto oldBase = PlacementOf(o.anchor.empty() ? o.regionId : o.anchor);
            if (self && oldBase && oldBase->worldPlaced) {
                RegionPlacement ghost = *self; ghost.worldPlaced = true; ghost.worldX = oldBase->worldX + o.dx; ghost.worldY = oldBase->worldY + o.dy;
                std::string choice;
                for (const auto& room : model_.rooms) {
                    const auto& id = room.settings.region.regionId;
                    if (id == o.regionId || id == o.anchor || !room.settings.worldPlaced) { continue; }
                    if (std::any_of(offsets.begin(), offsets.end(), [&](const auto& other) { return other.regionId == id; })) { continue; }
                    if (!FindSharedWorldEdge(ghost, Placement(room.settings))) { continue; }
                    if (ui.Button(key + ".anchor." + id, "锚定到 " + ShortChoiceLabel(room.object.name, 10))) { choice = id; }
                }
                if (!o.anchor.empty() && ui.Button(key + ".anchor.self", "改为相对自己")) { choice = o.regionId; }
                if (!choice.empty()) {
                    if (const auto newBase = PlacementOf(choice)) { o.dx += oldBase->worldX - newBase->worldX; o.dy += oldBase->worldY - newBase->worldY; o.anchor = choice == o.regionId ? std::string{} : choice; changed = true; }
                }
            }
        }
        if (ui.Button(key + ".remove", "移除")) { offsets.erase(offsets.begin() + static_cast<std::ptrdiff_t>(i)); changed = true; break; }
    }
    ui.Text("物体（状态" + std::string(machineC_ ? "C" : "B") + "的姿势）");
    auto& deltas = machineC_ ? after.deltasC : after.deltas;
    for (size_t i = 0; i < deltas.size(); ++i) {
        auto& d = deltas[i];
        const std::string key = "td2.machine.delta." + std::to_string(i);
        const auto name = objectNames_.contains(d.object) ? objectNames_[d.object] : "物体 " + std::to_string(d.object);
        ui.Text(ShortChoiceLabel(name, 14) + "  " + ShortNumber(std::hypot(d.position[0], d.position[1], d.position[2])) + " 米");
        bool pivot = d.hasPivot;
        if (ui.Checkbox(key + ".pivot", "绕中心转动", &pivot)) { d.hasPivot = pivot; changed = true; }
        if (d.hasPivot) { for (size_t k = 0; k < 3; ++k) { changed |= EditFloat(ui, key + ".pivot." + std::to_string(k), std::string("中心 ") + "XYZ"[k], d.pivot[k], -20, 20); } }
        if (!posing_ && ui.Button(key + ".remove", "移除")) { deltas.erase(deltas.begin() + static_cast<std::ptrdiff_t>(i)); changed = true; break; }
    }
    if (!posing_) {
        if (ui.Button("td2.machine.add_selected", "加入视口里选中的物体")) {
            const auto state = Execute(ctx, "editor.state", Json::object());
            int added = 0;
            if (state.ok) {
                for (const auto& object : state.data.value("selection", Json::object()).value("objects", Json::array())) {
                    const auto id = ObjectId(object.value("objectId", Json{}));
                    if (!id || std::any_of(deltas.begin(), deltas.end(), [id](const auto& d) { return d.object == id; })) { continue; }
                    MachineObjectDelta d; d.object = id; deltas.push_back(d); ++added;
                }
            }
            if (added) { changed = true; } else { model_.error = "先在视口里选中要动的物体"; }
        }
        if (!deltas.empty() && ui.Button("td2.machine.pose", "在视口里摆状态" + std::string(machineC_ ? "C" : "B"))) {
            if (!(after == m)) { WriteSelectedMachine(ctx, after); }
            BeginPose(ctx);
            return;
        }
    } else {
        ui.Text("用视口的移动、旋转工具摆好，然后保存这个姿势（摆的期间不要保存场景）");
        if (ui.Button("td2.machine.pose.save", "保存这个姿势")) { SavePose(ctx); return; }
        ui.SameLine(); if (ui.Button("td2.machine.pose.cancel", "放弃")) { CancelPose(ctx); }
    }
    if (changed) {
        if (ui.GetInteraction().editing) { pendingMachine_ = after; }
        else if (RecipeOf(after) != RecipeKind::Custom) { ApplyRecipe(ctx, m, after, false); }
        else { WriteSelectedMachine(ctx, after); }
    }
}
}
