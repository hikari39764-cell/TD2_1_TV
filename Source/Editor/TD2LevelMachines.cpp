#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
namespace {

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
            ui.CanvasRect(view_.Screen(static_cast<float>(p->worldX), static_cast<float>(p->worldY)), view_.Screen(static_cast<float>(p->worldX + p->cols), static_cast<float>(p->worldY + p->rows)), kMagentaFaint, 1);
        }
    }
    // レシピを選んで、作る先のテレビを探している時。
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
                if (self->worldPlaced) { rect(*self, current ? kMagenta : kMagentaFaint, current ? 3.f : 1.f); }
                const auto base = PlacementOf(o.anchor.empty() ? o.regionId : o.anchor);
                if (o.park) { if (current && self->worldPlaced) { label(*self, "B：世界から外す"); } continue; }
                if (!base || !base->worldPlaced) { continue; }
                RegionPlacement ghost = *self; ghost.worldX = base->worldX + o.dx; ghost.worldY = base->worldY + o.dy;
                const auto a = view_.Screen(static_cast<float>(ghost.worldX), static_cast<float>(ghost.worldY));
                const auto b = view_.Screen(static_cast<float>(ghost.worldX + ghost.cols), static_cast<float>(ghost.worldY + ghost.rows));
                const auto color = current ? kMagenta : kMagentaFaint;
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
                if (current) { label(ghost, std::string(machineC_ ? "C" : "B") + (o.flip ? " · 反転" : "")); }
            }
        }
        return;
    }
    for (size_t i = 0; i < m->members.size(); ++i) {
        const auto p = PlacementOf(m->members[i]);
        if (!p || !p->worldPlaced) { continue; }
        rect(*p, kMagenta, 3);
        std::string text = std::to_string(i + 1);
        if (m->kind == MachineKind::Stack) { text = i == 0 ? "下 1" : std::to_string(i + 1); }
        if (m->kind == MachineKind::Carousel && m->members.size() == 4) { constexpr std::array<std::string_view, 4> names{"正面", "右", "背面", "左"}; text = std::string(names[i]); }
        label(*p, text);
    }
    if (m->kind == MachineKind::Slide && !m->members.empty()) {
        if (const auto first = PlacementOf(m->members.front())) {
            for (const auto& [dx, dy] : m->emptySlots) { RegionPlacement slot = *first; slot.worldX += dx; slot.worldY += dy; rect(slot, kMagentaFaint, 2); label(slot, "空き"); }
        }
    }
}

// 仕掛けの道具で押した時。印を押すとその仕掛けを選ぶ。レシピを選んでいればテレビを押して作る。
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
    // 天秤：もう一台は逆へ、反対側の状態は鏡にする。
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

// レシピの機構を書く：地図のずれから 3D の動きを作り、部品を置き直し、足りない部品を置く。
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
    if (info.televisions == 2 && recipeFirst_.empty()) { recipeFirst_ = region; SetNote("二台目のテレビをクリック"); return; }
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
        const uint64_t base = CreateStageModel(ctx, "TD2Props/SM_TD2_CarouselBase.gltf", "回転柱の土台 " + id, column, model_.error);
        host = CreateStageModel(ctx, "TD2Props/SM_TD2_CarouselColumn.gltf", "回転柱 " + id, column, model_.error);
        if (!base || !host) { return; }
        JoinFolderOf(ctx, base, room->tvObject); JoinFolderOf(ctx, host, room->tvObject);
    }
    std::string error;
    if (!CreateMachine(ctx, kind, host, id, error)) { model_.error = error; return; }
    RefreshMachines(ctx, true);
    const auto created = std::find_if(model_.machines.begin(), model_.machines.end(), [&](const Machine& m) { return m.kind == kind && m.id == id; });
    if (created == model_.machines.end()) { model_.error = "仕掛けを作りましたが読めません"; return; }
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
    SetNote("「" + std::string(info.label) + "」を作りました：「配線」で圧力板からアイコンへドラッグ");
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
        if (!model_.devices.ToggleTarget(id, target)) { model_.error = "つなげません：" + target; return; }
        model_.draft.devices = model_.devices.Serialize();
        if (model_.Commit(ctx)) {
            for (size_t i = 0; i < model_.devices.Items().size(); ++i) { if (model_.devices.Items()[i].id == id) { model_.devices.Select(static_cast<int>(i)); } }
            focus_ = Focus::Device;
            const bool linked = std::find(model_.devices.Selected()->targets.begin(), model_.devices.Selected()->targets.end(), target) != model_.devices.Selected()->targets.end();
            SetNote(linked ? "つなぎました" : "外しました");
            // 仕掛けへつないだら、信号線も引く。
            if (linked && IsMachineTarget(target)) { RouteCables(ctx, plate, target); }
            checkInputClock_ = 10;
        }
        return;
    }
}

// ID を変えたら、その仕掛けを目標にしている圧力板の目標も同じ取り消しの中で書き換える。
bool LevelEditor::RenameMachine(EditorWorkspaceHostContext& ctx, const Machine& before, const std::string& id) {
    if (!IsStableId(id)) { model_.error = "ID は英数字・点・ハイフン・下線だけ"; return false; }
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

}
