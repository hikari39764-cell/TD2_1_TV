#include "TD2WorldMode.h"

namespace TD2::Editor::Level {
// ---------------------------------------------------------------- 配置：複数選択と平行移動
bool WorldMode::GroupPress(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    uint64_t hit = 0;
    for (auto it = model_->rooms.rbegin(); it != model_->rooms.rend(); ++it) {
        if (it->settings.worldPlaced && ContainsWorldPoint(Placement(it->settings), {p.x, p.y})) { hit = it->object.objectId; break; }
    }
    if (input.shift && hit) {
        // Shift+クリック：複数選択に足す・外す（今の選択も入れておく）。
        if (multi_.empty() && model_->selected && model_->selected != hit) { multi_.insert(model_->selected); }
        if (!multi_.erase(hit)) { multi_.insert(hit); }
        return true;
    }
    if (hit && multi_.size() > 1 && multi_.contains(hit)) {
        if (model_->Dirty() && !model_->Commit(ctx)) { return true; }
        groupMoving_ = true; groupPointer_ = p; groupDx_ = groupDy_ = 0; model_->gesture = true;
        return true;
    }
    if (!hit) {
        // 辺の上でなければ、空いた所からの矩形選択。
        if (EdgeAt(input.pointer)) { return false; }
        boxing_ = true; boxStart_ = p;
        return true;
    }
    multi_.clear();
    return false;
}

void WorldMode::GroupDrag(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    const bool released = (input.releasedButtons & kPrimary) || !(input.heldButtons & kPrimary);
    if (boxing_ && released) {
        boxing_ = false;
        const float x0 = (std::min)(boxStart_.x, p.x), x1 = (std::max)(boxStart_.x, p.x), y0 = (std::min)(boxStart_.y, p.y), y1 = (std::max)(boxStart_.y, p.y);
        multi_.clear();
        if (x1 - x0 < .5f && y1 - y0 < .5f) { return; }
        for (const auto& room : model_->rooms) {
            if (!room.settings.worldPlaced) { continue; }
            const auto r = WorldBounds(Placement(room.settings));
            if (r.left < x1 && r.right > x0 && r.top < y1 && r.bottom > y0) { multi_.insert(room.object.objectId); }
        }
        return;
    }
    if (!groupMoving_) { return; }
    groupDx_ = static_cast<int>(std::round(p.x - groupPointer_.x)); groupDy_ = static_cast<int>(std::round(p.y - groupPointer_.y));
    if (!released) { return; }
    groupMoving_ = false; model_->gesture = false;
    if (!groupDx_ && !groupDy_) { return; }
    // 動かした後の配置で重なりを確かめ、全部を一回の取り消しで書く。仕掛けは相対で書いてあるので一緒に付いて行く。
    std::vector<RegionPlacement> layout; Json commands = Json::array();
    for (const auto& room : model_->rooms) {
        auto placement = Placement(room.settings);
        if (multi_.contains(room.object.objectId) && placement.worldPlaced) {
            placement.worldX += groupDx_; placement.worldY += groupDy_;
            commands.push_back(PropertyCommand(room.target.objectId, room.target.componentId, "worldX", placement.worldX));
            commands.push_back(PropertyCommand(room.target.objectId, room.target.componentId, "worldY", placement.worldY));
        }
        layout.push_back(placement);
    }
    groupDx_ = groupDy_ = 0;
    if (const auto valid = ValidateWorldLayout(layout); !valid.valid) { model_->error = "不能放在这里：" + valid.message + "：" + valid.regionA + " / " + valid.regionB; return; }
    if (!Transact(ctx, commands, model_->error)) { return; }
    model_->error.clear(); model_->sceneDirty = true; model_->refresh = true; model_->Poll(ctx, true); layoutStale_ = true;
}

// ---------------------------------------------------------------- 预算
void WorldMode::DrawBudget(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (!ui.Checkbox("td2.world.budget", "显示预算", &budgetOpen_) && !budgetOpen_) { return; }
    if (!budgetOpen_) { return; }
    if (budget_.empty() || ui.Button("td2.world.budget.refresh", "重新计算")) {
        budget_.clear();
        // 世界の広さ（編集中は全部の部屋を 2048 texel の一枚に写すので、広いほど一台あたりが粗くなる）。
        int64_t left = 0, top = 0, right = 0, bottom = 0; int placed = 0, devices = 0; bool any = false;
        for (const auto& room : model_->rooms) {
            DeviceEditor d; d.Load(room.settings.devices); devices += static_cast<int>(d.Items().size());
            if (!room.settings.worldPlaced) { continue; }
            ++placed;
            const auto r = WorldBounds(Placement(room.settings));
            if (!any) { left = r.left; top = r.top; right = r.right; bottom = r.bottom; any = true; }
            left = (std::min)(left, r.left); top = (std::min)(top, r.top); right = (std::max)(right, r.right); bottom = (std::max)(bottom, r.bottom);
        }
        const int64_t span = (std::max)(right - left, bottom - top);
        const double texels = span > 0 ? (std::min)(48.0, 2048.0 / static_cast<double>(span)) : 48.0;
        budget_.push_back("电视地图 " + std::to_string(model_->rooms.size()) + " 台（世界里 " + std::to_string(placed) + " 台），机关 " + std::to_string(devices) + " 个");
        budget_.push_back("世界范围 " + std::to_string(right - left) + " × " + std::to_string(bottom - top) + " 格 · 编辑预览每格约 " + std::to_string(static_cast<int>(texels)) + " 像素" +
                          (texels < 8 ? "（偏糊：可把相关区域摆得更紧凑）" : ""));
        // 描画物体（エンジンの 1 フレーム上限 2048）：シーンのモデル + 編集中に全部の部屋を描く舞台の部品（部屋あたり約 21）。
        std::vector<SceneItem> models; std::string error;
        if (QueryObjects(ctx, "ModelComponent", models, error)) {
            const int stage = static_cast<int>(model_->rooms.size()) * 21 + devices * 3;
            const int total = static_cast<int>(models.size()) + stage;
            budget_.push_back("绘制物体约 " + std::to_string(total) + " / 2048（场景模型 " + std::to_string(models.size()) + " + 编辑时屏幕部件约 " + std::to_string(stage) + "）" +
                              (total > 1800 ? " · 接近上限" : ""));
        }
        // 大区域ごとの台数（画面の読みやすさの保護は一度に 12 台まで）。
        if (EnsureLayout(ctx, true)) {
            for (const auto& area : layout_.areas) {
                budget_.push_back("「" + ShortChoiceLabel(area.name, 12) + "」 " + std::to_string(area.regions.size()) + " 台" +
                                  (area.regions.size() > 12 ? " · 超过画面保护上限 12，同时看到的屏幕请控制在 12 台以内" : ""));
            }
        }
    }
    for (const auto& line : budget_) { ui.Text(line); }
    ui.Separator();
}

// ---------------------------------------------------------------- 仕掛けの見方
bool WorldMode::WriteSelected(EditorWorkspaceHostContext& ctx, const Machine& after) {
    const auto* before = SelectedMachine();
    if (!before) { return false; }
    std::string error;
    if (!WriteMachine(ctx, *before, after, error)) { model_->error = error; return false; }
    model_->sceneDirty = true; model_->machinesStale = true; model_->RefreshMachines(ctx); checkClock_ = 10;
    model_->error.clear();
    return true;
}

void WorldMode::ToggleMember(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
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
    WriteSelected(ctx, after);
}

void WorldMode::MachinePress(EditorWorkspaceHostContext& ctx, EditorWorkspacePoint p) {
    std::string room;
    for (auto it = model_->rooms.rbegin(); it != model_->rooms.rend(); ++it) {
        if (it->settings.worldPlaced && ContainsWorldPoint(Placement(it->settings), {p.x, p.y})) { room = it->settings.region.regionId; break; }
    }
    auto* m = SelectedMachine();
    if (!m) {
        // 仕掛けを選んでいない時：そのテレビが入っている仕掛けを選ぶ。
        for (const auto& machine : model_->machines) {
            const auto regions = machine.Regions();
            if (!room.empty() && std::find(regions.begin(), regions.end(), room) != regions.end()) {
                machineObject_ = machine.objectId; machineComponent_ = machine.componentId; machineIdDraft_ = machine.id; return;
            }
        }
        return;
    }
    if (posing_) { model_->error = "正在摆状态B：先保存或放弃"; return; }
    // 機構：状態の位置を表す点線の枠をつかむ。
    if (m->kind == MachineKind::Mechanism) {
        const auto& offsets = machineC_ ? m->offsetsC : m->offsets;
        for (size_t i = 0; i < offsets.size(); ++i) {
            const auto& o = offsets[i];
            const auto self = PlacementOf(o.regionId); const auto base = PlacementOf(o.anchor.empty() ? o.regionId : o.anchor);
            if (o.park || !self || !base || !base->worldPlaced) { continue; }
            RegionPlacement ghost = *self; ghost.worldPlaced = true; ghost.worldX = base->worldX + o.dx; ghost.worldY = base->worldY + o.dy;
            if (ContainsWorldPoint(ghost, {p.x, p.y}) && (o.dx || o.dy || !o.anchor.empty())) {
                ghost_ = static_cast<int>(i); ghostGrab_ = p; ghostDx_ = o.dx; ghostDy_ = o.dy; return;
            }
        }
    }
    if (!room.empty()) { ToggleMember(ctx, room); return; }
    // スロット棚：空いた格子を空きスロットにする・外す（一番目のメンバーの大きさの格子に合わせる）。
    if (m->kind == MachineKind::Slide && !m->members.empty()) {
        const auto first = PlacementOf(m->members.front());
        if (!first) { return; }
        const int w = (std::max)(1, first->cols), h = (std::max)(1, first->rows);
        const int ox = static_cast<int>(std::floor((p.x - first->worldX) / w)) * w, oy = static_cast<int>(std::floor((p.y - first->worldY) / h)) * h;
        auto after = *m;
        const auto found = std::find(after.emptySlots.begin(), after.emptySlots.end(), std::pair{ox, oy});
        if (found != after.emptySlots.end()) { after.emptySlots.erase(found); } else { after.emptySlots.push_back({ox, oy}); }
        WriteSelected(ctx, after);
    }
}

void WorldMode::MachineDrag(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    if (ghost_ < 0) { ghostPreview_.reset(); return; }
    auto* m = SelectedMachine();
    if (!m) { ghost_ = -1; ghostPreview_.reset(); return; }
    auto after = *m;
    auto& offsets = machineC_ ? after.offsetsC : after.offsets;
    if (ghost_ >= static_cast<int>(offsets.size())) { ghost_ = -1; ghostPreview_.reset(); return; }
    offsets[static_cast<size_t>(ghost_)].dx = ghostDx_ + static_cast<int>(std::round(p.x - ghostGrab_.x));
    offsets[static_cast<size_t>(ghost_)].dy = ghostDy_ + static_cast<int>(std::round(p.y - ghostGrab_.y));
    ghostPreview_ = after;
    if ((input.releasedButtons & kPrimary) || !(input.heldButtons & kPrimary)) {
        ghost_ = -1; ghostPreview_.reset();
        WriteSelected(ctx, after);
    }
}

void WorldMode::DrawMachineOverlay(IEditorWorkspaceUi& ui) {
    const Machine* m = ghostPreview_ ? &*ghostPreview_ : SelectedMachine();
    constexpr EditorWorkspaceColor magenta{.93f, .42f, .74f, 1}, faint{.93f, .42f, .74f, .35f};
    // 選んでいない仕掛けのテレビは薄く縁取る（どのテレビが仕掛けに入っているか見える）。
    for (const auto& machine : model_->machines) {
        if (m && machine.objectId == m->objectId && machine.componentId == m->componentId) { continue; }
        for (const auto& id : machine.Regions()) {
            const auto p = PlacementOf(id);
            if (!p || !p->worldPlaced) { continue; }
            ui.CanvasRect(view_.Screen(static_cast<float>(p->worldX), static_cast<float>(p->worldY)), view_.Screen(static_cast<float>(p->worldX + p->cols), static_cast<float>(p->worldY + p->rows)), faint, 1);
        }
    }
    if (!m) { return; }
    const auto rect = [&](const RegionPlacement& p, EditorWorkspaceColor color, float width) {
        ui.CanvasRect(view_.Screen(static_cast<float>(p.worldX), static_cast<float>(p.worldY)), view_.Screen(static_cast<float>(p.worldX + p.cols), static_cast<float>(p.worldY + p.rows)), color, width);
    };
    const auto label = [&](const RegionPlacement& p, const std::string& text) {
        if (view_.tile >= 4) { ui.CanvasText(view_.Screen(static_cast<float>(p.worldX) + .3f, static_cast<float>(p.worldY) + .2f), magenta, text); }
    };
    if (m->kind == MachineKind::Mechanism) {
        for (const auto* list : {&m->offsets, &m->offsetsC}) {
            const bool current = (list == &m->offsetsC) == machineC_;
            for (const auto& o : *list) {
                const auto self = PlacementOf(o.regionId);
                if (!self) { continue; }
                if (self->worldPlaced) { rect(*self, current ? magenta : faint, current ? 3.f : 1.f); }
                const auto base = PlacementOf(o.anchor.empty() ? o.regionId : o.anchor);
                if (o.park) { if (current && self->worldPlaced) { label(*self, "离开世界"); } continue; }
                if (!base || !base->worldPlaced) { continue; }
                RegionPlacement ghost = *self; ghost.worldX = base->worldX + o.dx; ghost.worldY = base->worldY + o.dy;
                // 状態の位置：点線の枠と、元の位置からの線。
                const auto a = view_.Screen(static_cast<float>(ghost.worldX), static_cast<float>(ghost.worldY));
                const auto b = view_.Screen(static_cast<float>(ghost.worldX + ghost.cols), static_cast<float>(ghost.worldY + ghost.rows));
                const auto color = current ? magenta : faint;
                for (float t = 0; t < 1; t += .1f) {
                    ui.CanvasLine({a.x + (b.x - a.x) * t, a.y}, {a.x + (b.x - a.x) * (t + .05f), a.y}, color, 2);
                    ui.CanvasLine({a.x + (b.x - a.x) * t, b.y}, {a.x + (b.x - a.x) * (t + .05f), b.y}, color, 2);
                    ui.CanvasLine({a.x, a.y + (b.y - a.y) * t}, {a.x, a.y + (b.y - a.y) * (t + .05f)}, color, 2);
                    ui.CanvasLine({b.x, a.y + (b.y - a.y) * t}, {b.x, a.y + (b.y - a.y) * (t + .05f)}, color, 2);
                }
                if (self->worldPlaced && (o.dx || o.dy || !o.anchor.empty())) {
                    ui.CanvasLine(view_.Screen(self->worldX + self->cols * .5f, self->worldY + self->rows * .5f), {(a.x + b.x) * .5f, (a.y + b.y) * .5f}, color, 1.5f);
                }
                if (current) { label(ghost, std::string(machineC_ ? "状态C" : "状态B") + (o.flip ? " · 翻转" : "")); }
            }
        }
        return;
    }
    for (size_t i = 0; i < m->members.size(); ++i) {
        const auto p = PlacementOf(m->members[i]);
        if (!p) { continue; }
        if (!p->worldPlaced) { continue; }
        rect(*p, magenta, 3);
        std::string text = std::to_string(i + 1);
        if (m->kind == MachineKind::Stack) { text = i == 0 ? "底 1" : std::to_string(i + 1); }
        if (m->kind == MachineKind::Carousel && m->members.size() == 4) { constexpr std::array<std::string_view, 4> names{"正面", "右", "背面", "左"}; text = std::string(names[i]); }
        label(*p, text);
    }
    if (m->kind == MachineKind::Slide && !m->members.empty()) {
        if (const auto first = PlacementOf(m->members.front())) {
            for (const auto& [dx, dy] : m->emptySlots) {
                RegionPlacement slot = *first; slot.worldX += dx; slot.worldY += dy;
                rect(slot, faint, 2); label(slot, "空槽");
            }
        }
    }
}

void WorldMode::RefreshCheckInput(EditorWorkspaceHostContext& ctx) {
    checkClock_ += ctx.unscaledDeltaSeconds;
    if (checkClock_ < 2) { return; }
    checkClock_ = 0;
    checkInput_ = {};
    checkInput_.placements = model_->Layout();
    checkInput_.plateTargets = model_->PlateTargets();
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

// ID を変えたら、その仕掛けを目標にしている圧力板の目標も同じ取り消しの中で書き換える。
bool WorldMode::RenameMachine(EditorWorkspaceHostContext& ctx, const Machine& before, const std::string& id) {
    if (!IsStableId(id)) { model_->error = "ID 只能用英文字母、数字、点、短横线和下划线"; return false; }
    if (model_->Dirty() && !model_->Commit(ctx)) { return false; }
    auto after = before; after.id = id;
    const auto oldReceivers = before.Receivers(), newReceivers = after.Receivers();
    Json commands = Json::array();
    for (const auto& room : model_->rooms) {
        DeviceEditor devices; devices.Load(room.settings.devices);
        bool changed = false;
        for (const auto& device : devices.Items()) {
            for (const auto& target : device.targets) {
                for (size_t i = 0; i < oldReceivers.size() && i < newReceivers.size(); ++i) {
                    if (target != oldReceivers[i].first) { continue; }
                    devices.ToggleTarget(device.id, target); devices.ToggleTarget(device.id, newReceivers[i].first); changed = true;
                }
            }
        }
        if (changed) { commands.push_back(PropertyCommand(room.target.objectId, room.target.componentId, "devices", devices.Serialize())); }
    }
    const auto idField = before.kind == MachineKind::Mechanism ? "mechanismId" : before.kind == MachineKind::Slide ? "rackId" : before.kind == MachineKind::Stack ? "stackId" : "carouselId";
    commands.push_back(PropertyCommand(before.objectId, before.componentId, idField, id));
    if (!Transact(ctx, commands, model_->error)) { return false; }
    model_->sceneDirty = true; model_->machinesStale = true; model_->refresh = true; model_->Poll(ctx, true); model_->RefreshMachines(ctx);
    return true;
}

bool WorldMode::ReadPoses(EditorWorkspaceHostContext& ctx, std::map<uint64_t, Pose>& poses, std::map<uint64_t, Json>& expected, std::string& sceneId, uint64_t& revision) {
    const auto layout = Execute(ctx, "scene.layout.query", Json::object());
    if (!layout.ok) { model_->error = layout.error; return false; }
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

// 状態 B（または天平の状態 C）をシーンに置いて見せる。作者はビューポートで物体を動かし、保存で相対の姿勢に直す。
bool WorldMode::BeginPose(EditorWorkspaceHostContext& ctx) {
    auto* m = SelectedMachine();
    if (!m || m->kind != MachineKind::Mechanism) { return false; }
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return false; }
    const auto& deltas = machineC_ ? m->deltasC : m->deltas;
    Json commands = Json::array();
    poseA_.clear();
    for (const auto& d : deltas) {
        const auto a = poses.find(d.object);
        if (a == poses.end()) { model_->error = "物体不存在：" + std::to_string(d.object); return false; }
        poseA_[d.object] = a->second;
        const auto base = d.anchor ? poses.find(d.anchor) : a;
        if (base == poses.end()) { model_->error = "锚点物体不存在：" + std::to_string(d.anchor); return false; }
        const auto b = ApplyDelta(a->second, base->second, d);
        commands.push_back({{"objectId", d.object}, {"expected", expected[d.object]}, {"position", b.position}, {"rotation", b.rotation}});
    }
    if (!commands.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", commands}});
        if (!applied.ok) { model_->error = applied.error; return false; }
    }
    posing_ = true; posingC_ = machineC_; model_->error.clear();
    return true;
}

bool WorldMode::SavePose(EditorWorkspaceHostContext& ctx) {
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
        // 錨は動かさない前提で、今の錨の姿勢を基準にする（錨が同じ機構で動く物なら状態 A の姿勢）。
        Pose base = a->second;
        if (d.anchor) { base = poseA_.contains(d.anchor) ? poseA_[d.anchor] : poses.contains(d.anchor) ? poses[d.anchor] : base; }
        d = MakeDelta(a->second, base, b->second, d);
        restore.push_back({{"objectId", d.object}, {"expected", expected[d.object]}, {"position", a->second.position}, {"rotation", a->second.rotation}});
    }
    if (!restore.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", restore}});
        if (!applied.ok) { model_->error = "状态B已读取，但恢复状态A失败：" + applied.error; return false; }
    }
    posing_ = false; poseA_.clear();
    return WriteSelected(ctx, after);
}

void WorldMode::CancelPose(EditorWorkspaceHostContext& ctx) {
    if (!posing_) { return; }
    posing_ = false;
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return; }
    Json restore = Json::array();
    for (const auto& [id, a] : poseA_) {
        if (!expected.contains(id)) { continue; }
        restore.push_back({{"objectId", id}, {"expected", expected[id]}, {"position", a.position}, {"rotation", a.rotation}});
    }
    poseA_.clear();
    if (restore.empty()) { return; }
    const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", restore}});
    if (!applied.ok) { model_->error = "恢复状态A失败：" + applied.error; }
}

void WorldMode::DrawMechanism(EditorWorkspaceHostContext& ctx, const Machine m) {
    auto& ui = *ctx.ui;
    auto after = m;
    bool changed = false;
    changed |= ui.Checkbox("td2.machine.toggle", "每踩一次切换（关：踩住时才动）", &after.toggle);
    changed |= ui.Checkbox("td2.machine.balance", "天平（两块压板对抗，三个状态）", &after.balance);
    changed |= EditFloat(ui, "td2.machine.seconds", "移动秒数", after.seconds, .2f, 8);
    changed |= EditFloat(ui, "td2.machine.dwell", "自动往复：两端停留秒数（0=由压板驱动）", after.dwell, 0, 20);
    changed |= EditFloat(ui, "td2.machine.settle", "压稳多久才动（秒）", after.settle, 0, 2);
    changed |= EditFloat(ui, "td2.machine.lift", "移动途中镜头抬高（米）", after.cameraLift, 0, 4);
    changed |= EditFloat(ui, "td2.machine.pull", "移动途中镜头后拉（米）", after.cameraPull, 0, 4);
    if (after.balance) {
        ui.Text("正在编辑：" + std::string(machineC_ ? "状态C（mech/ID/r 一侧）" : "状态B（mech/ID 一侧）"));
        if (ui.Button("td2.machine.side", machineC_ ? "改编辑状态B" : "改编辑状态C")) { if (posing_) { CancelPose(ctx); } machineC_ = !machineC_; }
    } else if (machineC_) { machineC_ = false; }
    // 区域：状態での世界地図の位置。
    ui.Separator(); ui.Text("区域（状态" + std::string(machineC_ ? "C" : "B") + "的位置）");
    auto& offsets = machineC_ ? after.offsetsC : after.offsets;
    if (offsets.empty()) { ui.Text("在地图上点电视加入"); }
    for (size_t i = 0; i < offsets.size(); ++i) {
        auto& o = offsets[i];
        const std::string key = "td2.machine.offset." + std::to_string(i);
        ui.Text(ShortChoiceLabel(o.regionId, 16));
        changed |= ui.Checkbox(key + ".park", "离开世界（不与任何区域相连）", &o.park);
        if (!o.park) {
            EditInt(ui, key + ".dx", "偏移 X", o.dx, -100000, 100000);
            EditInt(ui, key + ".dy", "偏移 Y", o.dy, -100000, 100000);
            changed |= ui.Checkbox(key + ".flip", "翻转 180°", &o.flip);
            ui.Text(o.anchor.empty() ? "相对：自己原来的位置" : "相对：区域 " + o.anchor);
            // 錨：状態の位置で辺を接するテレビに固定すると、そのテレビ（の章）を動かしても隣に付いて行く。錨を変えても状態の位置は変えない。
            const auto self = PlacementOf(o.regionId);
            const auto oldBase = PlacementOf(o.anchor.empty() ? o.regionId : o.anchor);
            if (self && oldBase && oldBase->worldPlaced) {
                RegionPlacement ghost = *self; ghost.worldPlaced = true; ghost.worldX = oldBase->worldX + o.dx; ghost.worldY = oldBase->worldY + o.dy;
                std::string choice;
                for (const auto& room : model_->rooms) {
                    const auto& id = room.settings.region.regionId;
                    if (id == o.regionId || id == o.anchor || !room.settings.worldPlaced) { continue; }
                    if (std::any_of(offsets.begin(), offsets.end(), [&](const auto& other) { return other.regionId == id; })) { continue; }
                    if (!FindSharedWorldEdge(ghost, Placement(room.settings))) { continue; }
                    if (ui.Button(key + ".anchor." + id, "锚定到相邻的 " + ShortChoiceLabel(room.object.name, 12))) { choice = id; }
                }
                if (!o.anchor.empty() && ui.Button(key + ".anchor.self", "改为相对自己")) { choice = o.regionId; }
                if (!choice.empty()) {
                    const auto newBase = PlacementOf(choice);
                    if (newBase) { o.dx += oldBase->worldX - newBase->worldX; o.dy += oldBase->worldY - newBase->worldY; o.anchor = choice == o.regionId ? std::string{} : choice; changed = true; }
                }
            }
        }
        if (ui.Button(key + ".remove", "移除此区域")) { offsets.erase(offsets.begin() + static_cast<std::ptrdiff_t>(i)); changed = true; break; }
    }
    // 物体：状態 A（シーンに置いた姿勢）から見た状態 B の姿勢。
    ui.Separator(); ui.Text("物体（状态" + std::string(machineC_ ? "C" : "B") + "的姿势）");
    auto& deltas = machineC_ ? after.deltasC : after.deltas;
    for (size_t i = 0; i < deltas.size(); ++i) {
        auto& d = deltas[i];
        const std::string key = "td2.machine.delta." + std::to_string(i);
        const auto name = objectNames_.contains(d.object) ? objectNames_[d.object] : "物体 " + std::to_string(d.object);
        ui.Text(ShortChoiceLabel(name, 16) + "  移动 " + ShortNumber(std::hypot(d.position[0], d.position[1], d.position[2])) + " 米");
        if (d.anchor) { ui.Text("相对锚点：" + (objectNames_.contains(d.anchor) ? objectNames_[d.anchor] : std::to_string(d.anchor))); }
        bool pivot = d.hasPivot;
        if (ui.Checkbox(key + ".pivot", "绕中心转动（倒立、翻转用）", &pivot)) { d.hasPivot = pivot; changed = true; }
        if (d.hasPivot) {
            for (size_t k = 0; k < 3; ++k) { changed |= EditFloat(ui, key + ".pivot." + std::to_string(k), std::string("中心 ") + "XYZ"[k] + "（物体自身坐标，米）", d.pivot[k], -20, 20); }
        }
        if (!posing_ && ui.Button(key + ".remove", "移除此物体")) { deltas.erase(deltas.begin() + static_cast<std::ptrdiff_t>(i)); changed = true; break; }
    }
    if (!posing_) {
        if (ui.Button("td2.machine.add_selected", "加入场景中选中的物体")) {
            const auto state = Execute(ctx, "editor.state", Json::object());
            int added = 0;
            if (state.ok) {
                for (const auto& object : state.data.value("selection", Json::object()).value("objects", Json::array())) {
                    const auto id = ObjectId(object.value("objectId", Json{}));
                    if (!id || std::any_of(deltas.begin(), deltas.end(), [id](const auto& d) { return d.object == id; })) { continue; }
                    MachineObjectDelta d; d.object = id; deltas.push_back(d); ++added;
                }
            }
            if (added) { changed = true; } else { model_->error = "先在场景里选中要动的物体（可多选）"; }
        }
        if (!deltas.empty() && ui.Button("td2.machine.pose", "在视口里摆状态" + std::string(machineC_ ? "C" : "B"))) {
            if (!(after == m)) { WriteSelected(ctx, after); }
            BeginPose(ctx);
            return;
        }
    } else {
        ui.Text("正在摆状态" + std::string(posingC_ ? "C" : "B") + "：用视口的移动、旋转工具摆好这些物体，然后保存。");
        ui.Text("摆的期间请不要保存场景。");
        if (ui.Button("td2.machine.pose.save", "保存这个姿势")) { SavePose(ctx); return; }
        ui.SameLine(); if (ui.Button("td2.machine.pose.cancel", "放弃，回到状态A")) { CancelPose(ctx); }
    }
    // 入力中の数値は入力が終わってから一回で書く。
    if (changed || !(after == m)) {
        if (!ui.GetInteraction().editing) { WriteSelected(ctx, after); } else { pendingMachine_ = after; }
    }
}

void WorldMode::DrawMachinePanel(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    model_->RefreshMachines(ctx);
    RefreshCheckInput(ctx);
    // 数値の入力中に貯めた変更は、入力が終わった時に一回で書く。
    if (pendingMachine_ && !ui.GetInteraction().editing) { const auto next = *pendingMachine_; pendingMachine_.reset(); WriteSelected(ctx, next); }
    ui.Text("机关");
    for (const auto& machine : model_->machines) {
        const bool selected = machine.objectId == machineObject_ && machine.componentId == machineComponent_;
        const auto problems = CheckMachine(machine, model_->machines, checkInput_);
        if (ui.Selectable("td2.machine.item." + std::to_string(machine.objectId) + "." + std::to_string(machine.componentId),
                          machine.KindLabel() + "  " + machine.id + (problems.empty() ? "" : "  ⚠"), selected)) {
            if (posing_) { CancelPose(ctx); }
            machineObject_ = machine.objectId; machineComponent_ = machine.componentId; machineIdDraft_ = machine.id; confirmDelete_ = false; machineC_ = false;
        }
    }
    // 新規作成：機構・スロット棚・テレビの塔は空の物体に付ける。回転柱は柱の物体に付ける（シーンで柱を選んでから）。
    ui.Separator(); ui.Text("新建机关");
    constexpr std::array<std::string_view, 4> kinds{"机构（转台、吊车、往复…）", "滑块货架", "电视塔", "转柱（先在场景选中柱子）"};
    for (size_t i = 0; i < kinds.size(); ++i) {
        if (ui.Selectable("td2.machine.newkind." + std::to_string(i), std::string(kinds[i]), newKind_ == static_cast<int>(i))) { newKind_ = static_cast<int>(i); }
    }
    EditText(ui, "td2.machine.newid", "ID", newMachineId_);
    if (ui.Button("td2.machine.create", "新建")) {
        const auto kind = static_cast<MachineKind>(newKind_);
        std::string error;
        uint64_t host = 0;
        if (kind == MachineKind::Carousel) { host = SelectedObject(ctx, error); }
        if (kind == MachineKind::Carousel && !host) { model_->error = "先在场景里选中转柱的柱子（柱子的位置就是转轴）"; }
        else if (!IsStableId(newMachineId_)) { model_->error = "ID 只能用英文字母、数字、点、短横线和下划线"; }
        else if (!CreateMachine(ctx, kind, host, newMachineId_, error)) { model_->error = error; }
        else {
            model_->sceneDirty = true; model_->machinesStale = true; model_->RefreshMachines(ctx); checkClock_ = 10;
            for (const auto& machine : model_->machines) { if (machine.kind == kind && machine.id == newMachineId_) { machineObject_ = machine.objectId; machineComponent_ = machine.componentId; machineIdDraft_ = machine.id; } }
            model_->error.clear();
        }
    }
    auto* m = SelectedMachine();
    if (!m) { return; }
    ui.Separator();
    ui.Text(m->KindLabel() + "  " + m->id + "  （" + ShortChoiceLabel(m->objectName, 14) + "）");
    EditText(ui, "td2.machine.id", "ID（改名会同步压板的目标）", machineIdDraft_);
    if (machineIdDraft_ != m->id && !ui.GetInteraction().editing) {
        if (!RenameMachine(ctx, *m, machineIdDraft_)) { machineIdDraft_ = m->id; }
        return;
    }
    if (m->kind == MachineKind::Mechanism) { DrawMechanism(ctx, *m); m = SelectedMachine(); if (!m) { return; } }
    else {
        auto after = *m;
        bool changed = false;
        if (m->kind == MachineKind::Slide) { changed |= EditFloat(ui, "td2.machine.seconds", "滑动秒数", after.seconds, .2f, 4); }
        if (m->kind == MachineKind::Carousel) { changed |= EditFloat(ui, "td2.machine.seconds", "每转一面秒数", after.seconds, .3f, 6); }
        if (m->kind == MachineKind::Stack) {
            changed |= EditFloat(ui, "td2.machine.pull", "抽出秒数", after.pullSeconds, .2f, 4);
            changed |= EditFloat(ui, "td2.machine.drop", "每层下落秒数", after.dropSeconds, .1f, 2);
            changed |= EditFloat(ui, "td2.machine.reset", "复位秒数", after.resetSeconds, .5f, 6);
            ui.Text("抽出距离（米，向最下面电视的背面；先抽的停得远）");
            for (size_t i = 0; i < after.dockDistances.size(); ++i) {
                changed |= EditFloat(ui, "td2.machine.dock." + std::to_string(i), "第 " + std::to_string(i + 1) + " 台", after.dockDistances[i], .3f, 10);
            }
            if (ui.Button("td2.machine.dock.add", "+ 停放位")) { after.dockDistances.push_back(after.dockDistances.empty() ? 2.4 : (std::max)(.6, after.dockDistances.back() - 1.15)); changed = true; }
            if (!after.dockDistances.empty()) { ui.SameLine(); if (ui.Button("td2.machine.dock.remove", "- 停放位")) { after.dockDistances.pop_back(); changed = true; } }
        }
        ui.Separator();
        ui.Text(m->kind == MachineKind::Stack ? "电视（自下而上）" : m->kind == MachineKind::Carousel ? "面（顺时针：正面、右、背面、左）" : "成员（第一台是空槽的基准）");
        for (size_t i = 0; i < after.members.size(); ++i) {
            const auto p = PlacementOf(after.members[i]);
            ui.Text(std::to_string(i + 1) + ". " + ShortChoiceLabel(after.members[i], 16) + (p && p->worldPlaced ? "" : m->kind == MachineKind::Carousel ? "（口袋：不在世界里）" : "（未放进世界）"));
            if (m->kind == MachineKind::Carousel || m->kind == MachineKind::Slide) {
                ui.SameLine();
                if (i > 0 && ui.Button("td2.machine.member.up." + std::to_string(i), "↑")) { std::swap(after.members[i], after.members[i - 1]); changed = true; }
                ui.SameLine();
                if (i + 1 < after.members.size() && ui.Button("td2.machine.member.down." + std::to_string(i), "↓")) { std::swap(after.members[i], after.members[i + 1]); changed = true; }
            }
        }
        if (m->kind == MachineKind::Slide) {
            ui.Text("空槽（相对第一台）");
            for (const auto& [x, y] : after.emptySlots) { ui.Text("· " + std::to_string(x) + ", " + std::to_string(y)); }
        }
        if (changed || !(after == *m)) {
            if (!ui.GetInteraction().editing) { WriteSelected(ctx, after); m = SelectedMachine(); if (!m) { return; } } else { pendingMachine_ = after; }
        }
    }
    // 確かめ：実行時に動かない理由と、つながっている圧力板。
    ui.Separator();
    const auto problems = CheckMachine(*m, model_->machines, checkInput_);
    if (problems.empty()) { ui.Text("✓ 可以运行"); }
    for (const auto& problem : problems) { ui.Text("⚠ " + problem); }
    const auto plates = PlatesFor(*m, checkInput_);
    if (!plates.empty()) {
        ui.Text("连到它的压板");
        for (const auto& plate : plates) {
            const auto region = plate.substr(0, plate.find('/'));
            if (ui.Button("td2.machine.plate." + plate, "打开 " + ShortChoiceLabel(plate, 22))) {
                for (const auto& room : model_->rooms) { if (room.settings.region.regionId == region) { OpenTelevision(ctx, room.object.objectId); return; } }
            }
        }
    }
    ui.Separator();
    if (!confirmDelete_) { if (ui.Button("td2.machine.delete", "删除这个机关")) { confirmDelete_ = true; } }
    else {
        ui.Text("删除后连到它的压板会显示「找不到」。");
        if (ui.Button("td2.machine.delete.confirm", "确认删除")) {
            std::string error;
            if (posing_) { CancelPose(ctx); }
            if (!RemoveMachine(ctx, *m, error)) { model_->error = error; }
            else { model_->sceneDirty = true; model_->machinesStale = true; machineObject_ = machineComponent_ = 0; model_->RefreshMachines(ctx); }
            confirmDelete_ = false;
        }
        ui.SameLine(); if (ui.Button("td2.machine.delete.cancel", "取消")) { confirmDelete_ = false; }
    }
}

void WorldMode::RefreshAssets(EditorWorkspaceHostContext& ctx) {
    assetsRefresh_ = false; assets_.clear(); cameras_.clear();
    // インポーターの公開の索引を使う。選んだ資産のスロットは標準の文書から取る。
    const bool listed = QueryAll(ctx, "asset.query", {{"type", "Sequence"}}, "assets", [this](const Json& item) {
        const auto guid = item.value("guid", ""), name = item.value("name", item.value("path", "Sequence"));
        if (!guid.empty()) { assets_.push_back({guid, name, {}, false}); }
        return true;
    }, model_->error);
    if (!listed) { return; }
    std::sort(assets_.begin(), assets_.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    QueryObjects(ctx, "CameraComponent", cameras_, model_->error);
}

bool WorldMode::ReadAssetSlots(EditorWorkspaceHostContext& ctx, SequenceAsset& asset) {
    if (asset.slotsKnown) { return true; }
    const auto opened = Execute(ctx, "authoring.session.open", {{"workspace", "cinematics"}, {"assetGuid", asset.guid}});
    if (!opened.ok) { model_->error = opened.error; return false; }
    const auto session = opened.data.value("sessionId", "");
    if (session.empty()) { model_->error = "演出未返回文档会话"; return false; }
    const auto result = Execute(ctx, "cinematics.document", {{"sessionId", session}});
    if (!result.ok) { model_->error = result.error; return false; }
    const auto document = result.data.value("document", Json::object());
    if (!document.is_object() || !document.contains("bindings") || !document["bindings"].is_array()) { model_->error = "原生演出缺少有效绑定定义"; return false; }
    asset.slots.clear();
    for (const auto& binding : document["bindings"]) {
        if (binding.value("targetKind", "") == "slot") {
            const auto name = binding.value("slotName", "");
            if (!name.empty() && std::find(asset.slots.begin(), asset.slots.end(), name) == asset.slots.end()) { asset.slots.push_back(name); }
        }
    }
    asset.slotsKnown = true; return true;
}

void WorldMode::ReadBinding(EditorWorkspaceHostContext& ctx) {
    bindingRefresh_ = false; bindingTarget_ = {}; binding_ = {}; bindingValid_ = false; bindingRevision_ = model_->revision;
    if (!edge_.a || !edge_.b) { return; }
    const auto* a = model_->Room(edge_.a);
    const auto* b = model_->Room(edge_.b);
    if (!a || !b) { return; }
    binding_.regionA = a->settings.region.regionId; binding_.regionB = b->settings.region.regionId;
    std::vector<SceneItem> items;
    if (!QueryObjects(ctx, kRegionSequenceComponentType, items, model_->error)) { return; }
    for (const auto& item : items) {
        const auto instances = Execute(ctx, "component.instances.query", {{"objectId", item.objectId}});
        if (!instances.ok) { model_->error = instances.error; return; }
        for (const auto& component : instances.data.value("components", Json::array())) {
            if (component.value("type", "") != kRegionSequenceComponentType) { continue; }
            const auto componentId = ObjectId(component.value("componentId", Json{}));
            const auto result = Execute(ctx, "component.describe", {{"objectId", item.objectId}, {"componentId", componentId}, {"propertyIds", {"regionA", "regionB", "sequenceAssetGuid", "sequenceSlotName", "cameraObject", "speed"}}});
            if (!result.ok) { model_->error = result.error; return; }
            const auto fields = result.data.value("fields", Json::array());
            const auto* fa = FieldValue(fields, "regionA"); const auto* fb = FieldValue(fields, "regionB");
            const auto* guid = FieldValue(fields, "sequenceAssetGuid"); const auto* slot = FieldValue(fields, "sequenceSlotName"); const auto* camera = FieldValue(fields, "cameraObject");
            if (!fa || !fa->is_string() || !fb || !fb->is_string() || !guid || !guid->is_string() || !slot || !slot->is_string() || !camera || !camera->is_number_integer()) { model_->error = "Sequence 绑定字段无效"; return; }
            RegionSequenceBinding value{fa->get<std::string>(), fb->get<std::string>(), guid->get<std::string>(), slot->get<std::string>(), ObjectId(*camera)};
            if (const auto* speed = FieldValue(fields, "speed"); speed && speed->is_number()) value.speed = speed->get<float>();
            if (!SameRegionPair(binding_, value)) { continue; }
            if (bindingTarget_.objectId) { model_->error = "同一交界有重复 Sequence 绑定"; bindingTarget_ = {}; return; }
            binding_ = std::move(value); bindingTarget_ = {item.objectId, componentId};
        }
    }
    observedBinding_ = binding_; bindingValid_ = true;
    if (sequenceDraft_ && !SameRegionPair(pendingBinding_, binding_)) { sequenceDraft_ = false; }
}

bool WorldMode::WriteBinding(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& requested) {
    if (!bindingValid_) { return false; }
    const auto next = requested;
    if (next.sequenceAssetGuid.empty()) {
        if (!next.sequenceSlotName.empty() || next.cameraObject) { model_->error = "未绑定演出时请清除槽和摄像机"; return false; }
    }
    else {
        const auto asset = std::find_if(assets_.begin(), assets_.end(), [&next](const auto& a) { return a.guid == next.sequenceAssetGuid; });
        if (asset == assets_.end()) { model_->error = "Sequence 资产不存在"; return false; }
        if (!ReadAssetSlots(ctx, *asset)) { return false; }
        if (!next.sequenceSlotName.empty() && std::find(asset->slots.begin(), asset->slots.end(), next.sequenceSlotName) == asset->slots.end()) { model_->error = "所选槽不属于该演出"; return false; }
        if (!next.sequenceSlotName.empty() && std::none_of(cameras_.begin(), cameras_.end(), [&next](const auto& camera) { return camera.objectId == next.cameraObject; })) { model_->error = "请选择有效摄像机"; return false; }
    }
    if (const auto valid = ValidateRegionSequenceBinding(next); !valid.valid) { model_->error = valid.message; return false; }
    const auto expected = sequenceDraft_ ? pendingObserved_ : observedBinding_;
    const auto expectedTarget = sequenceDraft_ ? pendingTarget_ : bindingTarget_;
    ReadBinding(ctx);
    if (!bindingValid_ || bindingTarget_ != expectedTarget || binding_ != expected) { model_->error = "Sequence 绑定已被其他编辑修改；请刷新后重试"; return false; }
    if (next == binding_) { sequenceDraft_ = false; model_->error.clear(); return true; }
    RegionTarget target = bindingTarget_;
    bool createdRecord = false;
    if (!target.objectId) {
        if (next.sequenceAssetGuid.empty()) { sequenceDraft_ = false; return true; }
        // 辺ごとに小さい記録を持つ。同じカメラを複数の辺で使っても記録を上書きしない。
        const auto created = Execute(ctx, "scene.create_empty_object", {{"name", "电视交界演出"}});
        if (!created.ok) { model_->error = created.error; return false; }
        target.objectId = ObjectId(created.data.value("objectId", Json{}));
        if (!target.objectId) { model_->error = "Sequence 配置对象创建失败"; return false; }
        createdRecord = true;
        const auto added = Execute(ctx, "component.add", {{"objectId", target.objectId}, {"componentType", kRegionSequenceComponentType}});
        if (!added.ok) { Execute(ctx, "scene.delete_object", {{"objectId", target.objectId}}); model_->error = added.error; return false; }
        for (const auto& c : added.data.value("addedComponents", Json::array())) { if (c.value("type", "") == kRegionSequenceComponentType) { target.componentId = ObjectId(c.value("componentId", Json{})); } }
        if (!target.componentId) { Execute(ctx, "scene.delete_object", {{"objectId", target.objectId}}); model_->error = "Sequence 配置组件创建失败"; return false; }
    }
    Json commands = Json::array();
    const auto add = [&commands, &target](std::string_view id, const auto& value) { commands.push_back(PropertyCommand(target.objectId, target.componentId, id, value)); };
    add("regionA", next.regionA); add("regionB", next.regionB); add("sequenceAssetGuid", next.sequenceAssetGuid); add("sequenceSlotName", next.sequenceSlotName); add("cameraObject", next.cameraObject);
    if (!Transact(ctx, commands, model_->error)) { if (createdRecord) { Execute(ctx, "scene.delete_object", {{"objectId", target.objectId}}); } return false; }
    model_->sceneDirty = true;
    bindingRefresh_ = true; ReadBinding(ctx);
    if (bindingTarget_ != target || binding_ != next) { model_->error = "Sequence 写入回读不一致"; return false; }
    sequenceDraft_ = false; model_->error.clear(); return true;
}

void WorldMode::DrawProperties(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (mode_ == 1) { DrawAreaPanel(ctx); return; }
    if (mode_ == 3) { DrawMachinePanel(ctx); return; }
    DrawBudget(ctx);
    if (pickingStart_) {
        ui.Text("在世界地图中选择出生位置");
        ui.Text("绿色轮廓是角色位置；点击立即开始");
        if (ui.Button("td2.world.spawn.cancel.properties", "取消")) { pickingStart_ = false; }
        return;
    }
    if (edge_.a) {
        if (assetsRefresh_) { RefreshAssets(ctx); }
        if (bindingRefresh_ || bindingRevision_ != model_->revision) { ReadBinding(ctx); }
        const auto* a = model_->Room(edge_.a);
        const auto* b = model_->Room(edge_.b);
        if (!a || !b) { return; }
        ui.Text("相邻区域 / 切换演出");
        if (ui.Button("td2.world.sequence.back", "返回房间属性")) { LeaveSequence(ctx); return; }
        DrawEdgeCamera(ctx, a->settings.region.regionId, b->settings.region.regionId);
        ui.Checkbox("td2.world.sequence.manual", "高级：手动指定演出资产", &edgeAdvanced_);
        if (!edgeAdvanced_) { return; }
        const auto beginDraft = [this] {
            if (!sequenceDraft_) { pendingBinding_ = binding_; pendingObserved_ = observedBinding_; pendingTarget_ = bindingTarget_; sequenceDraft_ = true; }
        };
        const std::string selectedGuid = sequenceDraft_ ? pendingBinding_.sequenceAssetGuid : binding_.sequenceAssetGuid;
        auto selectedAsset = std::find_if(assets_.begin(), assets_.end(), [&selectedGuid](const auto& asset) { return asset.guid == selectedGuid; });
        ui.Text("演出：" + (selectedGuid.empty() ? std::string("保持当前镜头") : selectedAsset == assets_.end() ? std::string("资产缺失") : ShortChoiceLabel(selectedAsset->name)));
        ui.Text(sequenceDraft_ && pendingBinding_ != observedBinding_ ? "绑定草稿未应用" : model_->sceneDirty ? "绑定已应用 · 场景未保存" : model_->savedOnce ? "绑定已保存" : "场景中的绑定");
        if (ui.Button("td2.world.sequence.pick", picker_ == 1 ? "收起演出列表" : "选择切换演出")) { picker_ = picker_ == 1 ? 0 : 1; }
        ui.SameLine(); if (ui.Button("td2.world.sequence.refresh", "刷新")) { assetsRefresh_ = true; bindingRefresh_ = true; }
        if (picker_ == 1) {
            EditText(ui, "td2.world.sequence.search", "查找演出", assetSearch_);
            std::vector<EditorChoice> choices{{"td2.world.sequence.none", "保持当前镜头", selectedGuid.empty()}};
            std::vector<std::string> guids{std::string{}};
            for (const auto& asset : assets_) {
                if (!assetSearch_.empty() && asset.name.find(assetSearch_) == std::string::npos) { continue; }
                choices.push_back({"td2.world.sequence." + asset.guid, asset.name, selectedGuid == asset.guid}); guids.push_back(asset.guid);
            }
            if (const int selected = DrawPagedChoices(ui, choices, assetPage_, "td2.world.sequences", 4); selected >= 0) {
                beginDraft(); pendingBinding_.sequenceAssetGuid = guids[static_cast<size_t>(selected)];
                const auto asset = std::find_if(assets_.begin(), assets_.end(), [this](const auto& value) { return value.guid == pendingBinding_.sequenceAssetGuid; });
                pendingBinding_.sequenceSlotName.clear(); pendingBinding_.cameraObject = 0;
                if (asset != assets_.end() && ReadAssetSlots(ctx, *asset) && !asset->slots.empty()) {
                    pendingBinding_.sequenceSlotName = asset->slots.front();
                    if (!cameras_.empty()) { pendingBinding_.cameraObject = cameras_.front().objectId; }
                }
                picker_ = 0;
            }
            if (assets_.empty()) { ui.Text("项目没有 Sequence；可先保持镜头"); }
        }
        if (ui.Button("td2.world.sequence.none.quick", "保持当前镜头") && !selectedGuid.empty()) {
            beginDraft(); pendingBinding_.sequenceAssetGuid.clear(); pendingBinding_.sequenceSlotName.clear(); pendingBinding_.cameraObject = 0;
            picker_ = 0;
        }
        const auto& editable = sequenceDraft_ ? pendingBinding_ : binding_;
        if (!editable.sequenceAssetGuid.empty()) {
            const auto asset = std::find_if(assets_.begin(), assets_.end(), [&editable](const auto& s) { return s.guid == editable.sequenceAssetGuid; });
            if (asset != assets_.end() && ReadAssetSlots(ctx, *asset)) {
                if (asset->slots.empty()) { ui.Text("使用资产内部镜头"); }
                else {
                    const auto currentCamera = std::find_if(cameras_.begin(), cameras_.end(), [&editable](const auto& camera) { return camera.objectId == editable.cameraObject; });
                    ui.Text("执行相机：" + (currentCamera == cameras_.end() ? std::string("未指定") : ShortChoiceLabel(currentCamera->name)));
                    ui.Checkbox("td2.world.sequence.advanced", "高级：执行相机 / 绑定槽", &sequenceAdvanced_);
                    if (sequenceAdvanced_) {
                        ui.Text("执行相机承载演出");
                        ui.Text("机位在原生演出工作区编辑");
                        if (ui.Button("td2.world.sequence.slot.pick", "槽：" + (editable.sequenceSlotName.empty() ? std::string("未指定") : editable.sequenceSlotName))) { picker_ = picker_ == 2 ? 0 : 2; }
                        if (picker_ == 2) {
                            std::vector<EditorChoice> choices;
                            for (const auto& slot : asset->slots) { choices.push_back({"td2.world.sequence.slot." + slot, slot, editable.sequenceSlotName == slot}); }
                            if (const int selected = DrawPagedChoices(ui, choices, slotPage_, "td2.world.slots", 4); selected >= 0) { beginDraft(); pendingBinding_.sequenceSlotName = asset->slots[static_cast<size_t>(selected)]; picker_ = 0; }
                        }
                        if (ui.Button("td2.world.sequence.camera.pick", "选择执行相机")) { picker_ = picker_ == 3 ? 0 : 3; }
                        if (picker_ == 3) {
                            EditText(ui, "td2.world.sequence.camera.search", "查找相机", cameraSearch_);
                            std::vector<EditorChoice> choices; std::vector<uint64_t> ids;
                            for (const auto& camera : cameras_) {
                                if (!cameraSearch_.empty() && camera.name.find(cameraSearch_) == std::string::npos) { continue; }
                                choices.push_back({"td2.world.sequence.camera." + std::to_string(camera.objectId), camera.name, editable.cameraObject == camera.objectId}); ids.push_back(camera.objectId);
                            }
                            if (const int selected = DrawPagedChoices(ui, choices, cameraPage_, "td2.world.cameras", 4); selected >= 0) { beginDraft(); pendingBinding_.cameraObject = ids[static_cast<size_t>(selected)]; picker_ = 0; }
                            if (cameras_.empty()) { ui.Text("场景没有 CameraComponent；先添加相机"); }
                        }
                    }
                }
            }
        }
        if (ui.Button("td2.world.sequence.apply", "应用绑定")) { if (sequenceDraft_) { WriteBinding(ctx, pendingBinding_); } }
        ui.SameLine(); if (ui.Button("td2.world.sequence.discard", "撤回草稿")) { sequenceDraft_ = false; bindingRefresh_ = true; model_->error.clear(); }
        if (!bindingValid_) { ui.Text("绑定读取失败；请刷新或处理上方错误"); return; }
        if (!editable.sequenceAssetGuid.empty() && ui.Button("td2.world.sequence.edit", "编辑演出 / 预览")) {
            if (!sequenceDraft_ || WriteBinding(ctx, pendingBinding_)) OpenTransitionCamera(ctx, binding_, model_->error);
        }
        // 演出の中身はエンジンの演出ワークスペースで作る。ここは区域の組へ既存の演出資産を結ぶだけ。
        ui.Text("镜头关键帧请在引擎的演出（Cinematics）工作区编辑");
        return;
    }
    if (!model_->ready) { return; }
    auto& s = model_->draft;
    if (const auto* item = model_->Current()) { ui.Text(item->object.name); }
    ui.Text(std::to_string(s.region.cols) + " × " + std::to_string(s.region.rows));
    if (ui.Button("td2.world.room.edit", "编辑电视地图")) { OpenTelevision(ctx, model_->selected); }
    DrawRoomCamera(ctx, s.region.regionId);
    if (s.worldPlaced) {
        // 正式なスポーン位置は変えず、次のキャンバスのクリックを一回限りの試遊の入口にする。
        if (ui.Button("td2.world.room.play", "从此区域开始")) {
            if (model_->Commit(ctx)) {
                pickingStart_ = true; hand_ = false; moving_ = libraryDrag_ = false;
                view_.Fit(view_.lastSize, static_cast<float>(s.worldX), static_cast<float>(s.worldY),
                          static_cast<float>(s.region.cols), static_cast<float>(s.region.rows));
            }
        }
        const auto before = s;
        EditInt(ui, "td2.world.room.x", "世界 X", s.worldX, -1000000, 1000000);
        EditInt(ui, "td2.world.room.y", "世界 Y", s.worldY, -1000000, 1000000);
        if (s != before) { propertyPending_ = true; }
        if (propertyPending_ && !ui.GetInteraction().editing && !model_->gesture) { model_->Finish(ctx, true); propertyPending_ = false; }
        if (ui.Button("td2.world.room.remove", "从世界移除")) { s.worldPlaced = false; model_->Commit(ctx); }
    }
    ui.Separator(); ui.Text("相邻区域 / 切换演出");
    const auto edges = Edges(); std::vector<EdgeSelection> neighbors; std::vector<EditorChoice> choices;
    for (const auto& edge : edges) {
        if (edge.a != model_->selected && edge.b != model_->selected) { continue; }
        const auto otherId = edge.a == model_->selected ? edge.b : edge.a;
        const auto other = std::find_if(model_->rooms.begin(), model_->rooms.end(), [otherId](const auto& room) { return room.object.objectId == otherId; });
        if (other == model_->rooms.end()) { continue; }
        const auto side = edge.a == model_->selected ? edge.edge.sideA : edge.edge.sideB;
        const std::string direction = side == WorldSide::Left ? "← " : side == WorldSide::Right ? "→ " : side == WorldSide::Top ? "↑ " : "↓ ";
        choices.push_back({"td2.world.neighbor." + std::to_string(otherId), direction + other->object.name, false}); neighbors.push_back(edge);
    }
    if (choices.empty()) { ui.Text(s.worldPlaced ? "没有共享边；拖动电视贴齐即可相邻" : "先把电视拖入世界地图"); }
    else {
        ui.Text("选择邻居配置镜头切换");
        if (const int selected = DrawPagedChoices(ui, choices, neighborPage_, "td2.world.neighbors", 4); selected >= 0) { ChooseEdge(ctx, neighbors[static_cast<size_t>(selected)]); }
    }
}
}
