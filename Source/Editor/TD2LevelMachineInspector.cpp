// 機構の棚と属性の欄：レシピの棚、選んだ機構の設定・状態の試し・元データ。
#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
namespace {
// レシピの棚の記号（線だけの簡単な絵）。
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
            if (recipe_ >= 0) { SetNote(recipes[i].televisions == 2 ? "テレビを二台クリック" : "テレビをクリック"); }
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
        ui.Text(recipe_ >= 0 ? "地図のテレビをクリックして作る" : "仕掛け");
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
    if (ui.Button("td2.machine.back", "← 仕掛けの一覧")) { if (previewState_ != 0) { PreviewState(ctx, 0); } machineObject_ = machineComponent_ = 0; focus_ = Focus::None; return; }
    // 状態の見本：3D と地図の点線で、動いた後の様子を見る。
    if (m->kind == MachineKind::Mechanism && !posing_) {
        ui.Text("プレビュー");
        ui.SameLine(); if (ui.Button("td2.machine.preview.a", previewState_ == 0 ? "● A" : "A")) { PreviewState(ctx, 0); machineC_ = false; }
        ui.SameLine(); if (ui.Button("td2.machine.preview.b", previewState_ == 1 ? "● B" : "B")) { PreviewState(ctx, 1); machineC_ = false; }
        if (m->balance) { ui.SameLine(); if (ui.Button("td2.machine.preview.c", previewState_ == -1 ? "● C" : "C")) { PreviewState(ctx, -1); machineC_ = true; } }
        m = SelectedMachine(); if (!m) { return; }
    }
    auto after = *m;
    bool changed = false, motion = false;
    if (m->kind == MachineKind::Mechanism && kind != RecipeKind::Custom) {
        if (!m->balance && m->dwell <= 0) {
            if (ui.Selectable("td2.machine.toggle.off", "踏んでいる間だけ動く", !after.toggle && !after.ride)) { after.toggle = false; after.ride = false; changed = true; }
            if (ui.Selectable("td2.machine.toggle.on", "踏むたびに切り替え", after.toggle && !after.ride)) { after.toggle = true; after.ride = false; changed = true; }
            // 乗り物：動くテレビに入って着地すると運ばれる（区域間の移動に使う）。
            if (ui.Selectable("td2.machine.ride", "乗ると動く", after.ride)) { after.ride = true; after.toggle = false; changed = true; }
        }
        changed |= EditFloat(ui, "td2.machine.seconds", "秒数", after.seconds, .2f, 8);
        if (kind == RecipeKind::Turn) {
            double angle = RecipeAngle(after, layout_);
            ui.Text("回転");
            for (const double option : {90.0, 180.0, -90.0}) {
                ui.SameLine();
                if (ui.Button("td2.machine.angle." + std::to_string(static_cast<int>(option)), (std::abs(angle - option) < 1 ? "● " : "") + ShortNumber(option) + "°")) { SetRecipeAngle(after, layout_, option); changed = true; }
            }
        }
        if (kind == RecipeKind::Shuttle) {
            changed |= EditFloat(ui, "td2.machine.dwell", "両端で止まる秒数", after.dwell, .2f, 20);
            if (!after.offsets.empty() && after.offsets.front().park) {
                double distance = RecipeDistance(after, layout_);
                if (EditFloat(ui, "td2.machine.distance", "後ろへ下がる距離", distance, .3f, 8)) { SetRecipeDistance(after, layout_, distance); changed = true; }
            }
        }
        if (kind == RecipeKind::Balance) { changed |= EditFloat(ui, "td2.machine.settle", "踏み続けて動くまでの秒数", after.settle, 0, 2); }
        // 動いた先：地図で点線の枠を引く。離れる（世界から外れる）も選べる。
        if (!after.offsets.empty() && kind != RecipeKind::Balance && kind != RecipeKind::Flip) {
            auto& o = after.offsets.front();
            bool park = o.park;
            if (ui.Checkbox("td2.machine.park", kind == RecipeKind::Shuttle ? "下がった時に世界から外す" : "動いた後に世界から外す", &park)) {
                o.park = park; motion = true;
                if (!park && kind == RecipeKind::Shuttle) { o.dx = 3; o.dy = 0; }
            }
        }
        if (!after.offsets.empty() && !after.offsets.front().park && kind != RecipeKind::Flip) { ui.Text("地図の点線の枠をドラッグして動いた後の位置を決める"); }
    }
    if (m->kind == MachineKind::Slide) {
        changed |= EditFloat(ui, "td2.machine.seconds", "滑る秒数", after.seconds, .2f, 4);
        ui.Text("Shift+クリックでテレビを加える · 空いたマスをクリックで空き");
    }
    if (m->kind == MachineKind::Carousel) {
        changed |= EditFloat(ui, "td2.machine.seconds", "一面回る秒数", after.seconds, .3f, 6);
        ui.Text("Shift+クリックで面を加える（時計回り：正面・右・背面・左）");
        for (size_t i = 0; i < after.members.size(); ++i) {
            ui.Text(std::to_string(i + 1) + ". " + ShortChoiceLabel(RoomName(after.members[i]), 14));
            ui.SameLine(); if (i > 0 && ui.Button("td2.machine.member.up." + std::to_string(i), "↑")) { std::swap(after.members[i], after.members[i - 1]); changed = true; }
            ui.SameLine(); if (i + 1 < after.members.size() && ui.Button("td2.machine.member.down." + std::to_string(i), "↓")) { std::swap(after.members[i], after.members[i + 1]); changed = true; }
        }
    }
    if (m->kind == MachineKind::Stack) {
        changed |= EditFloat(ui, "td2.machine.pull", "抜く秒数", after.pullSeconds, .2f, 4);
        changed |= EditFloat(ui, "td2.machine.drop", "一段落ちる秒数", after.dropSeconds, .1f, 2);
        changed |= EditFloat(ui, "td2.machine.reset", "戻す秒数", after.resetSeconds, .5f, 6);
        ui.Text("Shift+クリックで加える（下から）· 抜く距離");
        for (size_t i = 0; i < after.dockDistances.size(); ++i) { changed |= EditFloat(ui, "td2.machine.dock." + std::to_string(i), std::to_string(i + 1) + " 台目", after.dockDistances[i], .3f, 10); }
        if (ui.Button("td2.machine.dock.add", "+ 置き場")) { after.dockDistances.push_back(after.dockDistances.empty() ? 2.4 : (std::max)(.6, after.dockDistances.back() - 1.15)); changed = true; }
        if (!after.dockDistances.empty()) { ui.SameLine(); if (ui.Button("td2.machine.dock.remove", "- 置き場")) { after.dockDistances.pop_back(); changed = true; } }
    }
    if (changed || motion) {
        if (ui.GetInteraction().editing) { pendingMachine_ = after; }
        else if (motion && RecipeOf(after) != RecipeKind::Custom && after.kind == MachineKind::Mechanism) { ApplyRecipe(ctx, *m, after, false); }
        else { WriteSelectedMachine(ctx, after); }
        m = SelectedMachine(); if (!m) { return; }
    }
    // 部品：レシピが置いた物の数と、足りない部品。
    if (m->kind == MachineKind::Mechanism && kind != RecipeKind::Custom) {
        EnsureLayout(ctx);
        // 役目の名前（部品の欄は英語の短い名前で持つ）。
        const auto roleName = [](const std::string& role) -> std::string {
            constexpr std::array<std::pair<std::string_view, std::string_view>, 9> names{{{"chain", "吊りチェーン"}, {"box", "巻き上げ箱"}, {"beam", "天秤の梁"}, {"base", "転台の土台"},
                {"plate", "転台の盤"}, {"rail", "レール"}, {"disk", "逆さ転台の盤"}, {"mount", "支柱"}, {"support", "台車"}}};
            for (const auto& [id, name] : names) { if (id == role) { return std::string(name); } }
            return role;
        };
        std::map<std::string, int> roles;
        for (const auto& p : m->parts) { ++roles[roleName(p.role)]; }
        std::string text = "部品：";
        for (const auto& [role, count] : roles) { text += role + (count > 1 ? "×" + std::to_string(count) : std::string{}) + " "; }
        ui.Text(roles.empty() ? "部品：なし" : text);
        if (!PlanParts(*m, layout_).empty() && ui.Button("td2.machine.parts", "部品を補う")) { ApplyRecipe(ctx, *m, *m, true); return; }
    }
    // 連なる圧力板と、動かない理由。
    ui.Separator();
    const auto plates = PlatesFor(*m, checkInput_);
    if (plates.empty() && m->dwell <= 0 && !m->ride) { ui.Text("圧力板がまだありません：「配線」で圧力板から地図の仕掛けのアイコンへドラッグ"); }
    for (const auto& plate : plates) {
        const auto region = plate.substr(0, plate.find('/'));
        if (ui.Selectable("td2.machine.plate." + plate, "圧力板 · " + ShortChoiceLabel(RoomName(region), 12))) {
            for (const auto& room : model_.rooms) { if (room.settings.region.regionId == region) { if (SelectRoom(ctx, room.object.objectId)) { FitRoom(room.settings); focus_ = Focus::Room; } return; } }
        }
    }
    for (const auto& problem : CheckMachine(*m, model_.machines, checkInput_)) { if (problem.find("圧力板がまだありません") == std::string::npos) { ui.Text("⚠ " + problem); } }
    ui.Separator();
    ui.Checkbox("td2.machine.advanced", "詳細", &machineAdvanced_);
    if (machineAdvanced_) { DrawMachineAdvanced(ctx, *m); m = SelectedMachine(); if (!m) { return; } }
    ui.Separator();
    if (!confirmDelete_) { if (ui.Button("td2.machine.delete", "仕掛けを削除")) { confirmDelete_ = true; } }
    else {
        ui.Text("つないだ圧力板は外れ、レシピが置いた部品も一緒に消えます");
        if (ui.Button("td2.machine.delete.confirm", "削除する")) {
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
        ui.SameLine(); if (ui.Button("td2.machine.delete.cancel", "取り消し")) { confirmDelete_ = false; }
    }
}

// 高級：機構の生の欄（地図のずれ・錨・物体の姿勢・カメラの動き）。レシピの機構でも開ける。
void LevelEditor::DrawMachineAdvanced(EditorWorkspaceHostContext& ctx, const Machine& m) {
    auto& ui = *ctx.ui;
    EditText(ui, "td2.machine.id", "ID（圧力板も一緒に変わる）", machineIdDraft_);
    if (machineIdDraft_ != m.id && !ui.GetInteraction().editing) { if (!RenameMachine(ctx, m, machineIdDraft_)) { machineIdDraft_ = m.id; } return; }
    if (m.kind != MachineKind::Mechanism) {
        if (m.kind == MachineKind::Slide) { ui.Text("空き（一台目から）"); for (const auto& [x, y] : m.emptySlots) { ui.Text("· " + std::to_string(x) + ", " + std::to_string(y)); } }
        return;
    }
    auto after = m;
    bool changed = false;
    if (RecipeOf(m) != RecipeKind::Custom) {
        if (ui.Button("td2.machine.custom", "手で編集する（3D の動きを自動で作らない）")) { after.recipe.clear(); changed = true; }
    }
    changed |= ui.Checkbox("td2.machine.balance", "天秤", &after.balance);
    changed |= EditFloat(ui, "td2.machine.dwell", "自動往復の停止秒数", after.dwell, 0, 20);
    changed |= EditFloat(ui, "td2.machine.settle", "踏み続けて動くまでの秒数", after.settle, 0, 2);
    changed |= EditFloat(ui, "td2.machine.lift", "途中のカメラの持ち上げ", after.cameraLift, 0, 4);
    changed |= EditFloat(ui, "td2.machine.pull", "途中のカメラの引き", after.cameraPull, 0, 4);
    if (after.balance) {
        if (ui.Button("td2.machine.side", machineC_ ? "状態 B を編集" : "状態 C を編集")) { if (posing_) { CancelPose(ctx); } machineC_ = !machineC_; }
    } else if (machineC_) { machineC_ = false; }
    ui.Text("区域（状態 " + std::string(machineC_ ? "C" : "B") + "）· Shift+クリックで加える");
    auto& offsets = machineC_ ? after.offsetsC : after.offsets;
    for (size_t i = 0; i < offsets.size(); ++i) {
        auto& o = offsets[i];
        const std::string key = "td2.machine.offset." + std::to_string(i);
        ui.Text(ShortChoiceLabel(RoomName(o.regionId), 14));
        changed |= ui.Checkbox(key + ".park", "世界から外す", &o.park);
        if (!o.park) {
            const auto before = o;
            EditInt(ui, key + ".dx", "ずれ X", o.dx, -100000, 100000);
            EditInt(ui, key + ".dy", "ずれ Y", o.dy, -100000, 100000);
            changed |= o != before;
            changed |= ui.Checkbox(key + ".flip", "180° 反転", &o.flip);
            ui.Text(o.anchor.empty() ? "基準：自分" : "基準：" + ShortChoiceLabel(RoomName(o.anchor), 12));
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
                    if (ui.Button(key + ".anchor." + id, "基準にする：" + ShortChoiceLabel(room.object.name, 10))) { choice = id; }
                }
                if (!o.anchor.empty() && ui.Button(key + ".anchor.self", "自分を基準にする")) { choice = o.regionId; }
                if (!choice.empty()) {
                    if (const auto newBase = PlacementOf(choice)) { o.dx += oldBase->worldX - newBase->worldX; o.dy += oldBase->worldY - newBase->worldY; o.anchor = choice == o.regionId ? std::string{} : choice; changed = true; }
                }
            }
        }
        if (ui.Button(key + ".remove", "外す")) { offsets.erase(offsets.begin() + static_cast<std::ptrdiff_t>(i)); changed = true; break; }
    }
    ui.Text("物体（状態 " + std::string(machineC_ ? "C" : "B") + " の姿勢）");
    auto& deltas = machineC_ ? after.deltasC : after.deltas;
    for (size_t i = 0; i < deltas.size(); ++i) {
        auto& d = deltas[i];
        const std::string key = "td2.machine.delta." + std::to_string(i);
        const auto name = objectNames_.contains(d.object) ? objectNames_[d.object] : "物体 " + std::to_string(d.object);
        ui.Text(ShortChoiceLabel(name, 14) + "  " + ShortNumber(std::hypot(d.position[0], d.position[1], d.position[2])) + " m");
        bool pivot = d.hasPivot;
        if (ui.Checkbox(key + ".pivot", "中心で回す", &pivot)) { d.hasPivot = pivot; changed = true; }
        if (d.hasPivot) { for (size_t k = 0; k < 3; ++k) { changed |= EditFloat(ui, key + ".pivot." + std::to_string(k), std::string("中心 ") + "XYZ"[k], d.pivot[k], -20, 20); } }
        if (!posing_ && ui.Button(key + ".remove", "外す")) { deltas.erase(deltas.begin() + static_cast<std::ptrdiff_t>(i)); changed = true; break; }
    }
    if (!posing_) {
        if (ui.Button("td2.machine.add_selected", "ビューポートで選んだ物体を加える")) {
            const auto state = Execute(ctx, "editor.state", Json::object());
            int added = 0;
            if (state.ok) {
                for (const auto& object : state.data.value("selection", Json::object()).value("objects", Json::array())) {
                    const auto id = ObjectId(object.value("objectId", Json{}));
                    if (!id || std::any_of(deltas.begin(), deltas.end(), [id](const auto& d) { return d.object == id; })) { continue; }
                    MachineObjectDelta d; d.object = id; deltas.push_back(d); ++added;
                }
            }
            if (added) { changed = true; } else { model_.error = "先にビューポートで動かす物体を選んでください"; }
        }
        if (!deltas.empty() && ui.Button("td2.machine.pose", "ビューポートで状態 " + std::string(machineC_ ? "C" : "B") + " を作る")) {
            if (!(after == m)) { WriteSelectedMachine(ctx, after); }
            BeginPose(ctx);
            return;
        }
    } else {
        ui.Text("ビューポートの移動・回転で合わせてから、この姿勢を保存（その間はシーンを保存しない）");
        if (ui.Button("td2.machine.pose.save", "この姿勢を保存")) { SavePose(ctx); return; }
        ui.SameLine(); if (ui.Button("td2.machine.pose.cancel", "やめる")) { CancelPose(ctx); }
    }
    if (changed) {
        if (ui.GetInteraction().editing) { pendingMachine_ = after; }
        else if (RecipeOf(after) != RecipeKind::Custom) { ApplyRecipe(ctx, m, after, false); }
        else { WriteSelectedMachine(ctx, after); }
    }
}
}
