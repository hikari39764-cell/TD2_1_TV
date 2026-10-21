#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
int LevelEditor::AreaIndex(std::string_view regionId) const {
    if (!layoutValid_) { return -1; }
    for (size_t i = 0; i < layout_.areas.size(); ++i) {
        const auto& r = layout_.areas[i].regions;
        if (std::find(r.begin(), r.end(), regionId) != r.end()) { return static_cast<int>(i); }
    }
    return -1;
}

EditorWorkspaceColor LevelEditor::AreaColor(int index) {
    // 抑えた色数：琥珀・青緑・菫・砂・青・苔。
    constexpr std::array<EditorWorkspaceColor, 6> colors{{{.93f, .72f, .36f, 1}, {.36f, .78f, .74f, 1}, {.68f, .55f, .92f, 1},
                                                          {.82f, .74f, .56f, 1}, {.42f, .62f, .92f, 1}, {.60f, .78f, .45f, 1}}};
    return colors[static_cast<size_t>(index) % colors.size()];
}

void LevelEditor::DrawAreaTint(IEditorWorkspaceUi& ui, const RegionSettings& s, EditorWorkspacePoint a, EditorWorkspacePoint b) {
    if (tool_ != Tool::Camera) { return; }
    const int area = AreaIndex(s.region.regionId);
    if (area < 0) { ui.CanvasFill(a, b, {.85f, .3f, .3f, .10f}); return; }
    auto tint = AreaColor(area); tint.a = layout_.areas[static_cast<size_t>(area)].name == areaFocus_ ? .32f : .18f;
    ui.CanvasFill(a, b, tint);
    if (view_.tile >= 5) { auto label = AreaColor(area); ui.CanvasText({a.x + 4, b.y - 18}, label, ShortChoiceLabel(layout_.areas[static_cast<size_t>(area)].name, 10)); }
}

const LayoutEdge* LevelEditor::LayoutEdgeOf(const EdgeSelection& e) const {
    const auto* ra = model_.Room(e.a); const auto* rb = model_.Room(e.b);
    if (!ra || !rb) { return nullptr; }
    const auto& ia = ra->settings.region.regionId; const auto& ib = rb->settings.region.regionId;
    for (const auto& edge : layout_.edges) { if ((edge.a == ia && edge.b == ib) || (edge.a == ib && edge.b == ia)) { return &edge; } }
    return nullptr;
}

void LevelEditor::DrawEdgeKind(IEditorWorkspaceUi& ui, const EdgeSelection& e, EditorWorkspacePoint a, EditorWorkspacePoint b) const {
    const auto* edge = LayoutEdgeOf(e);
    if (!edge) { return; }
    if (!edge->passable) { ui.CanvasLine(a, b, {.32f, .36f, .38f, .7f}, 1.0f); return; }   // 壁で隔てた辺（越えられない）
    if (edge->between) {
        // 大区域どうし：演出が流れる。まだ演出が無い辺は赤みで知らせる。
        const EditorWorkspaceColor color = edge->sequenceGuid.empty() ? EditorWorkspaceColor{.95f, .45f, .40f, .95f} : EditorWorkspaceColor{.97f, .79f, .39f, .9f};
        ui.CanvasLine(a, b, color, 3.0f);
        ui.CanvasCircle({(a.x + b.x) * .5f, (a.y + b.y) * .5f}, 4.5f, color, 2.0f);
        return;
    }
    // 大区域の中：点線（カメラは自動で追い、演出は流れない）。
    const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
    for (float t = 0; t < length; t += 10) {
        const float t1 = (std::min)(t + 5, length);
        ui.CanvasLine({a.x + dx * t / length, a.y + dy * t / length}, {a.x + dx * t1 / length, a.y + dy * t1 / length}, {.45f, .70f, .68f, .65f}, 1.5f);
    }
}

// 生成は途中で layout_ を何度も読み直す。呼び手が layout_ の中を指す名前を渡すことがあるので、先に値で写す。
void LevelEditor::Generate(EditorWorkspaceHostContext& ctx, bool cameras, bool travels, std::string_view areaName, std::string_view roomId,
                           const LayoutEdge* onlyEdge, bool force) {
    const std::string area(areaName), room(roomId);
    const std::optional<LayoutEdge> edgeCopy = onlyEdge ? std::optional<LayoutEdge>(*onlyEdge) : std::nullopt;
    const LayoutEdge* edge = edgeCopy ? &*edgeCopy : nullptr;
    if (!model_.Commit(ctx) || !EnsureLayout(ctx, true)) { return; }
    std::string error; GenerateReport cameraReport, travelReport;
    if (!SyncScreenGeometry(ctx, layout_, cameraReport, error)) { model_.error = error; return; }
    cameraReport = {};
    if (cameras && !GenerateCameras(ctx, layout_, area, room, force, cameraReport, error)) { model_.error = error; return; }
    if (travels && !GenerateTravels(ctx, layout_, area, edge, force, travelReport, error)) { model_.error = error; return; }
    std::string note = cameras && travels ? "カメラと演出" : cameras ? "カメラ" : "演出";
    note += "：書き込み " + std::to_string(cameraReport.written + travelReport.written);
    if (cameraReport.kept + travelReport.kept) { note += "、手直しを残した " + std::to_string(cameraReport.kept + travelReport.kept); }
    if (cameraReport.failed + travelReport.failed) { note += "、失敗 " + std::to_string(cameraReport.failed + travelReport.failed); }
    for (const auto* list : {&cameraReport.notes, &travelReport.notes}) for (size_t i = 0; i < list->size() && i < 3; ++i) { note += "\n· " + (*list)[i]; }
    SetNote(note);
    MarkSceneChanged(); model_.error.clear(); model_.refresh = true;
}

// 新しく置いたテレビのカメラ：隣のテレビが大区域に入っていれば同じ大区域へ入れてから作る。
void LevelEditor::SetUpNewRoom(EditorWorkspaceHostContext& ctx, const std::string& regionId, const std::string& besideRegion) {
    if (!besideRegion.empty() && EnsureLayout(ctx, true)) {
        if (const auto* area = layout_.AreaOf(besideRegion)) {
            const std::string previous = areaFocus_;
            areaFocus_ = area->name;
            ToggleArea(ctx, regionId);
            areaFocus_ = previous;
        }
    }
    Generate(ctx, true, false, {}, regionId, nullptr, false);
}

void LevelEditor::ToggleArea(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
    if (!EnsureLayout(ctx, true)) { return; }
    const auto focus = std::find_if(layout_.areas.begin(), layout_.areas.end(), [this](const auto& a) { return a.name == areaFocus_; });
    if (focus == layout_.areas.end()) { return; }
    // 選んだ大区域に入っていれば外し、入っていなければ入れる（他の大区域からは外す）。一回の取り消しにまとめる。
    Json commands = Json::array();
    const bool inside = std::find(focus->regions.begin(), focus->regions.end(), regionId) != focus->regions.end();
    for (const auto& area : layout_.areas) {
        auto regions = area.regions;
        const auto found = std::find(regions.begin(), regions.end(), regionId);
        if (&area == &*focus) { if (inside) { regions.erase(found); } else { regions.push_back(regionId); } }
        else if (found != regions.end()) { regions.erase(found); }
        else { continue; }
        commands.push_back(PropertyCommand(area.objectId, area.componentId, "regions", MachineText::JoinNames(regions)));
    }
    if (!Transact(ctx, commands, model_.error)) { return; }
    MarkSceneChanged(); model_.error.clear(); layoutStale_ = true;
}

// 下の棚：大区域を色で並べる。選んで地図のテレビを押すと入れる・外す。
void LevelEditor::DrawCameraShelf(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    EnsureLayout(ctx);
    if (!layoutValid_) { return; }
    constexpr size_t columns = 8;
    if (ui.BeginTable("td2.palette.areas", static_cast<uint32_t>(columns))) {
        for (size_t i = 0; i <= layout_.areas.size(); ++i) {
            if (i % columns == 0) { ui.TableNextRow(); }
            ui.TableNextColumn();
            if (i == layout_.areas.size()) {
                if (Tile(ui, "td2.palette.area.new", "+ 大区域", false, nullptr)) {
                    std::string error;
                    const auto name = "大区域 " + std::to_string(layout_.areas.size() + 1);
                    if (CreateArea(ctx, name, {}, error)) { areaFocus_ = name; MarkSceneChanged(); } else { model_.error = error; }
                    return;
                }
                break;
            }
            const auto& area = layout_.areas[i];
            const auto color = AreaColor(static_cast<int>(i));
            const auto count = std::to_string(area.regions.size());
            if (Tile(ui, "td2.palette.area." + std::to_string(area.objectId), area.name, area.name == areaFocus_, [color, count](IEditorWorkspaceUi& u, EditorWorkspacePoint s) {
                u.CanvasFill({10, 8}, {s.x - 10, s.y - 22}, {color.r, color.g, color.b, .55f});
                u.CanvasText({14, 10}, {.05f, .07f, .08f, 1}, count + " 台");
            })) {
                areaFocus_ = areaFocus_ == area.name ? std::string{} : area.name;
                focus_ = Focus::None; edge_ = {};
            }
        }
        ui.EndTable();
    }
    if (ui.Button("td2.palette.plan", plan_ ? "● 見下ろし" : "見下ろし")) { plan_ = !plan_; }
    ui.SameLine();
    if (ui.Button("td2.palette.generate.all", "カメラと演出をすべて作る")) { Generate(ctx, true, true, {}, {}, nullptr, false); }
}

// 左の欄（カメラの道具で何も選んでいない時）：選んだ大区域の構図と生成。
void LevelEditor::DrawAreaInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    EnsureLayout(ctx);
    if (!layoutValid_) { return; }
    int loose = 0;
    for (const auto& room : layout_.rooms) { if (room.placed && AreaIndex(room.regionId) < 0) { ++loose; } }
    if (loose) { ui.Text("△ " + std::to_string(loose) + " 台のテレビがどの大区域にも入っていない（赤）"); }
    const auto focus = std::find_if(layout_.areas.begin(), layout_.areas.end(), [this](const auto& a) { return a.name == areaFocus_; });
    if (focus == layout_.areas.end()) { ui.Text("下で大区域を選び、テレビをクリックして加える・外す"); return; }
    ui.Text(focus->name + "（" + std::to_string(focus->regions.size()) + " 台）");
    double fill = focus->frameFill, orbit = focus->orbitDegrees, lift = focus->cameraLift;
    bool changed = EditFloat(ui, "td2.area.fill", "画面が占める高さ", fill, .15f, .8f);
    changed |= EditFloat(ui, "td2.area.orbit", "出口側への角度", orbit, 0, 40);
    changed |= EditFloat(ui, "td2.area.lift", "画面より上の高さ", lift, -1, 2);
    if (changed) { areaDraft_ = {fill, orbit, lift}; areaDraftPending_ = true; areaDraftObject_ = focus->objectId; areaDraftComponent_ = focus->componentId; }
    if (areaDraftPending_ && !ui.GetInteraction().editing) {
        const auto set = [this](std::string_view id, double v) { return PropertyCommand(areaDraftObject_, areaDraftComponent_, id, v); };
        (void)Transact(ctx, Json::array({set("frameFill", areaDraft_[0]), set("orbitDegrees", areaDraft_[1]), set("cameraLift", areaDraft_[2])}), model_.error);
        areaDraftPending_ = false; MarkSceneChanged();
    }
    // 生成の後は layout_ が作り直されて focus が指す先も消えるので、このフレームの残りは描かずに戻る。
    if (ui.Button("td2.area.cameras", "この大区域のカメラを作る")) { Generate(ctx, true, false, focus->name, {}, nullptr, false); return; }
    if (ui.Button("td2.area.travels", "この大区域に出入りする演出を作る")) { Generate(ctx, false, true, focus->name, {}, nullptr, false); return; }
    if (ui.Button("td2.area.force", "自動に戻す（手直しを上書き）")) { Generate(ctx, true, true, focus->name, {}, nullptr, true); return; }
    int dead = 0;
    for (const auto& edge : layout_.edges) { if ((!edge.between || !edge.passable) && edge.bindingObject) { ++dead; } }
    if (dead && ui.Button("td2.area.prune", "区域内の古い演出を消す（" + std::to_string(dead) + "）")) {
        int removed = 0; std::string error;
        if (PruneInAreaBindings(ctx, layout_, removed, error)) { SetNote("古い結び付けを " + std::to_string(removed) + " 個消しました"); MarkSceneChanged(); } else { model_.error = error; }
    }
}

std::string LevelEditor::ResolveStyleLabel(const LayoutEdge& edge) const {
    auto copy = edge; copy.style = "auto";
    const auto id = ResolveStyle(layout_, copy);
    for (const auto& style : TravelStyles()) { if (style.id == id) { return std::string(style.label); } }
    return id;
}

void LevelEditor::TrialFromEdge(EditorWorkspaceHostContext& ctx, const LayoutEdge& edge, bool fromA) {
    if (!model_.Commit(ctx)) { return; }
    double x = 0, y = 0; uint64_t room = 0;
    if (!EdgeTrialSpawn(layout_, edge, fromA, x, y, room)) { model_.error = "この境の近くに立てる場所がありません"; return; }
    StartPreview(ctx, room, {x, y});
}

// 左の欄：選んだ辺の切り替え（大区域どうしの演出の様式と秒数、試し歩き）。
void LevelEditor::DrawEdgeInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    EnsureLayout(ctx);
    const auto* a = model_.Room(edge_.a); const auto* b = model_.Room(edge_.b);
    if (!a || !b) { focus_ = Focus::None; return; }
    const auto ra = a->settings.region.regionId, rb = b->settings.region.regionId;
    const LayoutEdge* edge = nullptr;
    for (const auto& e : layout_.edges) { if ((e.a == ra && e.b == rb) || (e.a == rb && e.b == ra)) { edge = &e; } }
    if (!edge) { ui.Text("読み込み中…"); return; }
    ui.Text(ShortChoiceLabel(RoomName(edge->a), 10) + " ↔ " + ShortChoiceLabel(RoomName(edge->b), 10));
    if (edgeDraftKey_ != edge->a + "|" + edge->b) { edgeDraft_ = *edge; edgeDraftKey_ = edge->a + "|" + edge->b; }
    if (!edge->passable) { ui.Text("地形でふさがっていて通れません"); return; }
    if (!edge->between) { ui.Text("同じ大区域：カメラが自動で追う"); }
    else {
        ui.Text(edge->sequenceGuid.empty() ? "大区域の境 · 演出なし" : edge->keepAuthored ? "大区域の境 · 手直しを残す" : "大区域の境");
        bool changed = false;
        for (const auto& style : TravelStyles()) {
            const bool current = edgeDraft_.style == style.id;
            if (ui.Selectable("td2.edge.style." + std::string(style.id), std::string(style.label) + (style.id == std::string_view("auto") ? "（" + ResolveStyleLabel(*edge) + "）" : ""), current)) {
                edgeDraft_.style = std::string(style.id); changed = true;
            }
        }
        double seconds = edgeDraft_.seconds;
        if (EditFloat(ui, "td2.edge.seconds", "秒数", seconds, .4f, 10.f)) { edgeDraft_.seconds = seconds; changed = true; }
        changed |= ui.Checkbox("td2.edge.keep", "手直しを残す", &edgeDraft_.keepAuthored);
        if (changed) { edgeDraftPending_ = true; }
        if (edgeDraftPending_ && !ui.GetInteraction().editing) {
            std::string error;
            if (!WriteEdgeSettings(ctx, edgeDraft_, error)) { model_.error = error; }
            edgeDraftPending_ = false; MarkSceneChanged();
        }
        // 生成の後は layout_ が作り直されて edge が指す先が消えるので、すぐ戻る。
        if (ui.Button("td2.edge.generate", edge->sequenceGuid.empty() ? "演出を作る" : "作り直す")) { Generate(ctx, false, true, {}, {}, &edgeDraft_, false); return; }
        if (!edge->sequenceGuid.empty()) {
            ui.SameLine(); if (ui.Button("td2.edge.force", "自動に戻す")) { auto draft = edgeDraft_; draft.keepAuthored = false; Generate(ctx, false, true, {}, {}, &draft, true); return; }
            if (ui.Button("td2.edge.edit", "演出ワークスペースで編集")) {
                RegionSequenceBinding binding; binding.regionA = edge->a; binding.regionB = edge->b;
                binding.sequenceAssetGuid = edge->sequenceGuid; binding.speed = static_cast<float>(edge->speed);
                OpenTransitionCamera(ctx, binding, model_.error);
            }
        }
    }
    if (ui.Button("td2.edge.trial.a", "▶ " + ShortChoiceLabel(RoomName(edge->a), 8) + " から歩く")) { const auto copy = *edge; TrialFromEdge(ctx, copy, true); return; }
    if (ui.Button("td2.edge.trial.b", "▶ " + ShortChoiceLabel(RoomName(edge->b), 8) + " から歩く")) { const auto copy = *edge; TrialFromEdge(ctx, copy, false); return; }
}

// 俯瞰図：テレビ（色は大区域）・画面の向き・区域カメラ（視錐）・大区域の間の経路・障害物を上から見る。
void LevelEditor::DrawPlan(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input) {
    auto& ui = *ctx.ui;
    if (!layoutValid_) { return; }
    ui.CanvasFill({0, 0}, input.size, {.06f, .08f, .09f, 1});
    double x0 = 1e9, z0 = 1e9, x1 = -1e9, z1 = -1e9;
    const auto grow = [&](const Vec3& p) { x0 = (std::min)(x0, p[0]); x1 = (std::max)(x1, p[0]); z0 = (std::min)(z0, p[2]); z1 = (std::max)(z1, p[2]); };
    for (const auto& r : layout_.rooms) { if (r.placed) { grow(r.tv.position); if (r.cameraId) { grow(r.camera.position); } } }
    if (x1 <= x0) { return; }
    const double margin = 1.5; x0 -= margin; x1 += margin; z0 -= margin; z1 += margin;
    const double scale = (std::min)(input.size.x / (x1 - x0), input.size.y / (z1 - z0));
    const double ox = (input.size.x - (x1 - x0) * scale) * .5, oy = (input.size.y - (z1 - z0) * scale) * .5;
    // 上から見る：+x を右、-z を上（+z を向くテレビは画の下を向く）。
    const auto P = [&](double x, double z) { return EditorWorkspacePoint{static_cast<float>(ox + (x - x0) * scale), static_cast<float>(oy + (z1 - z) * scale)}; };
    for (const auto& box : layout_.obstacles) {
        if (box.hi[0] < x0 || box.lo[0] > x1 || box.hi[2] < z0 || box.lo[2] > z1 || box.hi[1] < .2) { continue; }
        ui.CanvasFill(P(box.lo[0], box.hi[2]), P(box.hi[0], box.lo[2]), {.16f, .18f, .19f, 1});
    }
    for (const auto& path : planPaths_) {
        const EditorWorkspaceColor color = path.problem.empty() ? EditorWorkspaceColor{.97f, .79f, .39f, .85f} : EditorWorkspaceColor{.95f, .4f, .38f, .85f};
        for (size_t i = 1; i < path.points.size(); ++i) { ui.CanvasLine(P(path.points[i - 1][0], path.points[i - 1][2]), P(path.points[i][0], path.points[i][2]), color, 2); }
    }
    uint64_t clicked = 0; double best = 14 * 14;
    for (const auto& r : layout_.rooms) {
        if (!r.placed) { continue; }
        const double yaw = r.tv.rotation[1] * Math::kPi / 180;
        const double rx = std::cos(yaw), rz = -std::sin(yaw), nx = std::sin(yaw), nz = std::cos(yaw);
        const double hw = r.body[0] * r.tv.scale[0] / 2, hd = r.body[2] * r.tv.scale[2] / 2;
        const double cx = r.tv.position[0], cz = r.tv.position[2];
        const auto corner = [&](double u, double v) { return P(cx + rx * u + nx * v, cz + rz * u + nz * v); };
        const int area = AreaIndex(r.regionId);
        auto color = area >= 0 ? AreaColor(area) : EditorWorkspaceColor{.6f, .65f, .66f, 1};
        const bool selected = model_.Current() && model_.Current()->settings.region.regionId == r.regionId;
        ui.CanvasLine(corner(-hw, -hd), corner(hw, -hd), color, selected ? 3.f : 1.5f);
        ui.CanvasLine(corner(hw, -hd), corner(hw, hd), color, selected ? 3.f : 1.5f);
        ui.CanvasLine(corner(-hw, hd), corner(-hw, -hd), color, selected ? 3.f : 1.5f);
        ui.CanvasLine(corner(-hw, hd), corner(hw, hd), {.95f, .96f, .92f, 1}, selected ? 4.f : 2.5f);   // 画面の側
        if (r.cameraId) {
            const auto eye = P(r.camera.position[0], r.camera.position[2]);
            const double cy = r.camera.rotation[1] * Math::kPi / 180;
            const auto ray = [&](double offset) { const double a = cy + offset; return P(r.camera.position[0] + std::sin(a) * 1.4, r.camera.position[2] + std::cos(a) * 1.4); };
            const auto camColor = r.cameraLocked || CameraEdited(r) ? EditorWorkspaceColor{.97f, .79f, .39f, 1} : EditorWorkspaceColor{.55f, .82f, .88f, 1};
            ui.CanvasLine(eye, ray(-.39), camColor, 1.2f); ui.CanvasLine(eye, ray(.39), camColor, 1.2f);
            ui.CanvasCircle(eye, selected ? 5.f : 3.5f, camColor, 2);
            const double dx = input.pointer.x - eye.x, dy = input.pointer.y - eye.y;
            if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; clicked = r.objectId; }
        }
        const auto center = corner(0, 0);
        const double dx = input.pointer.x - center.x, dy = input.pointer.y - center.y;
        if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; clicked = r.objectId; }
        if (scale > 18) { ui.CanvasText({center.x + 6, center.y - 6}, {.85f, .88f, .88f, .9f}, ShortChoiceLabel(r.name, 10)); }
    }
    if (input.hovered && (input.pressedButtons & kPrimary) && clicked && SelectRoom(ctx, clicked)) { focus_ = Focus::Room; }
}
}
