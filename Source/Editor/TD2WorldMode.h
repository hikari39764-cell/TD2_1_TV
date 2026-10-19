#pragma once
// レベル編集の世界地図：テレビの配置、カメラの大区域、俯瞰カメラ、仕掛け、区域どうしの演出を直す。
#include "TD2LevelModel.h"

namespace TD2::Editor::Level {
struct EdgeSelection { uint64_t a = 0, b = 0; SharedWorldEdge edge; };
struct SequenceAsset { std::string guid, name; std::vector<std::string> slots; bool slotsKnown = false; };

// 世界地図は既存テレビの整数原点だけを編集する。
class WorldMode final : public MapMode {
public:
    using MapMode::MapMode;
    bool IsDirty() const override { return MapMode::IsDirty() || (sequenceDraft_ && pendingBinding_ != observedBinding_); }
    void DiscardChanges(EditorWorkspaceHostContext& ctx) override { sequenceDraft_ = false; bindingRefresh_ = true; MapMode::DiscardChanges(ctx); }
    void OnActivate(EditorWorkspaceHostContext& ctx) override { MapMode::OnActivate(ctx); assetsRefresh_ = true; bindingRefresh_ = true; layoutStale_ = true; view_.fit = true; }
    void OnDeactivate(EditorWorkspaceHostContext& ctx) override {
        if (posing_) { CancelPose(ctx); }
        if (libraryDrag_) { model_->Discard(); } else { MapMode::OnDeactivate(ctx); }
        moving_ = false; libraryDrag_ = false; pickingStart_ = false; groupMoving_ = false; boxing_ = false; ghost_ = -1;
    }
    bool ApplyDraft(EditorWorkspaceHostContext& ctx) override {
        if (posing_) { CancelPose(ctx); }
        if (sequenceDraft_ && pendingBinding_ != observedBinding_ && !WriteBinding(ctx, pendingBinding_)) { return false; }
        sequenceDraft_ = false; return MapMode::ApplyDraft(ctx);
    }
    // 見方（配置・カメラ大区域・俯瞰カメラ・仕掛け）を変える。
    void ChangeMode(EditorWorkspaceHostContext& ctx, int mode) { SetMode(ctx, mode); }
    int Mode() const { return mode_; }
    // 新しく置いたテレビのカメラを作る（隣のテレビが大区域に入っていれば同じ大区域へ入れてから）。
    void SetUpNewRoom(EditorWorkspaceHostContext& ctx, const std::string& regionId, const std::string& besideRegion) {
        if (!besideRegion.empty() && EnsureLayout(ctx, true)) {
            if (const auto* area = layout_.AreaOf(besideRegion)) {
                const std::string name = area->name; areaFocus_ = name;
                ToggleArea(ctx, regionId);
            }
        }
        Generate(ctx, true, false, {}, regionId, nullptr, false);
    }
    void BeginFrame(EditorWorkspaceHostContext& ctx) override {
        clock_ += ctx.unscaledDeltaSeconds;
        dragHeldAny_ = dragPressedAny_ = dragReleasedAny_ = false;
    }
    // 左：保存と履歴・テレビの一覧（世界へドラッグして置く）・選んだものの属性。一覧→地図の順でドラッグの入力をまとめる。
    void DrawSide(EditorWorkspaceHostContext& ctx) override {
        auto& ui = *ctx.ui;
        HistoryToolbar(ctx);
        const int unplaced = static_cast<int>(std::count_if(model_->rooms.begin(), model_->rooms.end(), [](const auto& r) { return !r.settings.worldPlaced; }));
        if (ui.Button("td2.world.library.toggle", libraryOpen_ ? "收起电视列表" : "电视列表" + (unplaced ? "（" + std::to_string(unplaced) + " 台未放进世界）" : std::string()))) { libraryOpen_ = !libraryOpen_; }
        if (libraryOpen_ || libraryDrag_) { DrawLibrary(ctx); ui.Separator(); }
        if (!report_.empty()) { ui.Text(report_); }
        DrawProperties(ctx);
    }
    // 右：見方の切り替え・一行の説明・地図（俯瞰カメラの時は上から見た図）。
    void DrawMap(EditorWorkspaceHostContext& ctx) override {
        auto& ui = *ctx.ui;
        ModeBar(ctx, mode_);
        ViewControls(ctx);
        // 説明は短い行に分ける（欄の幅で切れないように）。
        if (pickingStart_) {
            ui.Text("点击角色脚下的位置开始游玩\n空中也可选 · 右键取消");
            if (ui.Button("td2.world.spawn.cancel", "取消选择出生位置")) { pickingStart_ = false; }
            if (!model_->error.empty()) { ui.Text(model_->error); }
        } else if (mode_ == 1) {
            ui.Text(areaFocus_.empty() ? "在左栏选择或新建一个大区域\n再点击电视加入 / 移出" : "点击电视加入或移出\n「" + areaFocus_ + "」");
        } else if (mode_ == 2) {
            ui.Text("点电视或机位选中\n在左栏生成、调整机位");
        } else if (mode_ == 3) {
            const auto* m = SelectedMachine();
            ui.Text(!m ? "在左栏选择或新建机关\n点电视可找到它所属的机关"
                : m->kind == MachineKind::Mechanism ? "点电视加入 / 移出状态" + std::string(machineC_ ? "C" : "B") + "的区域\n拖动虚线框设定它在该状态的位置"
                : m->kind == MachineKind::Slide ? "点电视加入 / 移出货架成员\n点空着的格子设为空槽"
                : m->kind == MachineKind::Stack ? "点电视加入 / 移出电视塔\n自动按自下而上排序"
                : "点电视加入 / 移出转柱的面\n顺序在左栏调整");
        } else if (!multi_.empty()) {
            ui.Text("多选 " + std::to_string(multi_.size()) + " 台 · 拖动其中一台整体平移\nShift+点击增减 · 点空白处取消");
        } else {
            ui.Text("拖动电视移动 · 双击打开电视地图 · 从左栏列表拖入\nShift+点击多选 · 空白处拖框 · Delete 移出世界");
        }
        if (mode_ != 0) { EnsureLayout(ctx); }
        if (mode_ == 2) { DrawPlan(ctx); } else { DrawCanvas(ctx); }
    }
private:
    bool LeaveSequence(EditorWorkspaceHostContext& ctx) {
        // 文脈が変わっても草稿は捨てない。無効な草稿は右欄で取り消せるよう選択を保つ。
        if (sequenceDraft_ && pendingBinding_ != observedBinding_ && !WriteBinding(ctx, pendingBinding_)) { return false; }
        sequenceDraft_ = false; edge_ = {}; picker_ = 0; bindingRefresh_ = true; return true;
    }
    bool ChooseEdge(EditorWorkspaceHostContext& ctx, const EdgeSelection& next) {
        if (next.a == edge_.a && next.b == edge_.b) { return true; }
        if (!LeaveSequence(ctx)) { return false; }
        edge_ = next; bindingRefresh_ = true; return true;
    }
    void OpenTelevision(EditorWorkspaceHostContext& ctx, uint64_t id) { if (LeaveSequence(ctx) && model_->Select(ctx, id) && model_->Commit(ctx) && switcher) { switcher(kTelevisionView); } }
    // テレビの一覧。まだ世界に置いていないテレビを先に並べる。押したまま地図へドラッグすると置ける。ダブルクリックでテレビ地図を開く。
    void DrawLibrary(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        {
            if (ui.Button("td2.world.add_selected", "选中电视建图") && LeaveSequence(ctx)) { model_->AddSelectedTelevision(ctx); OpenTelevision(ctx, model_->selected); }
            ui.SameLine();
            if (ui.Button("td2.world.new", "+ 空白地图") && LeaveSequence(ctx)) { model_->Create(ctx); OpenTelevision(ctx, model_->selected); }
            EditText(ui, "td2.world.rooms.search", "查找", librarySearch_);
            auto rooms = model_->rooms;
            std::stable_partition(rooms.begin(), rooms.end(), [](const auto& r) { return !r.settings.worldPlaced; });
            for (size_t i = 0; i < rooms.size(); ++i) {
                const auto& room = rooms[i];
                if (!librarySearch_.empty() && room.object.name.find(librarySearch_) == std::string::npos &&
                    room.settings.region.regionId.find(librarySearch_) == std::string::npos) { continue; }
                ui.Text((room.settings.worldPlaced ? "" : "○ ") + ShortChoiceLabel(room.object.name) + "  " + std::to_string(room.settings.region.cols) + "×" + std::to_string(room.settings.region.rows));
                const auto input = ui.BeginCanvas("td2.world.thumbnail." + std::to_string(room.object.objectId), 64);
                TrackDrag(input);
                DrawThumbnail(ui, room, input);
                if (input.hovered && (input.pressedButtons & kPrimary)) {
                    pickingStart_ = false;
                    if (LeaveSequence(ctx) && model_->Select(ctx, room.object.objectId)) {
                        if (lastClick_ == room.object.objectId && clock_ - lastClickTime_ < .3f) { OpenTelevision(ctx, room.object.objectId); lastClick_ = 0; }
                        else {
                            libraryDrag_ = true; moving_ = false; model_->gesture = true; edge_ = {}; offset_ = {static_cast<float>(model_->draft.region.cols) * .5f, static_cast<float>(model_->draft.region.rows) * .5f};
                            lastClick_ = room.object.objectId; lastClickTime_ = clock_;
                        }
                    }
                }
                ui.EndCanvas();
            }
        }
    }
    // このフレームのどこかの欄で左ボタンが押された・押されている・離されたかを集める。
    void TrackDrag(const EditorWorkspaceCanvasInput& input) {
        dragHeldAny_ |= (input.heldButtons & kPrimary) != 0;
        dragPressedAny_ |= (input.pressedButtons & kPrimary) != 0;
        dragReleasedAny_ |= (input.releasedButtons & kPrimary) != 0;
    }
    // 画面上の点から 6 px 以内にある辺。
    std::optional<EdgeSelection> EdgeAt(EditorWorkspacePoint pointer) const {
        for (const auto& e : Edges()) {
            const auto [a, b] = EdgePoints(e.edge);
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float t = std::clamp(((pointer.x - a.x) * dx + (pointer.y - a.y) * dy) / (std::max)(.001f, dx * dx + dy * dy), 0.0f, 1.0f);
            if (std::hypot(pointer.x - a.x - t * dx, pointer.y - a.y - t * dy) < 6) { return e; }
        }
        return std::nullopt;
    }
    std::vector<EdgeSelection> Edges() const {
        std::vector<EdgeSelection> result;
        for (size_t a = 0; a < model_->rooms.size(); ++a) for (size_t b = a + 1; b < model_->rooms.size(); ++b) {
            const auto& ra = model_->rooms[a]; const auto& rb = model_->rooms[b];
            const auto sa = ra.object.objectId == model_->selected && model_->gesture ? model_->draft : ra.settings;
            const auto sb = rb.object.objectId == model_->selected && model_->gesture ? model_->draft : rb.settings;
            if (const auto edge = FindSharedWorldEdge(Placement(sa), Placement(sb))) { result.push_back({ra.object.objectId, rb.object.objectId, *edge}); }
        }
        return result;
    }
    std::pair<EditorWorkspacePoint, EditorWorkspacePoint> EdgePoints(const SharedWorldEdge& e) const {
        const float c = static_cast<float>(e.coordinate), lo = static_cast<float>(e.overlapBegin), hi = static_cast<float>(e.overlapEnd);
        const bool vertical = e.sideA == WorldSide::Left || e.sideA == WorldSide::Right;
        return vertical ? std::pair{view_.Screen(c, lo), view_.Screen(c, hi)} : std::pair{view_.Screen(lo, c), view_.Screen(hi, c)};
    }
    void FitWorld(EditorWorkspacePoint size) {
        bool any = false; WorldGridRect bounds{};
        for (const auto& r : model_->rooms) if (r.settings.worldPlaced) {
            const auto rect = WorldBounds(Placement(r.settings));
            if (!any) { bounds = rect; any = true; }
            else { bounds.left = (std::min)(bounds.left, rect.left); bounds.top = (std::min)(bounds.top, rect.top); bounds.right = (std::max)(bounds.right, rect.right); bounds.bottom = (std::max)(bounds.bottom, rect.bottom); }
        }
        view_.Fit(size, static_cast<float>(bounds.left), static_cast<float>(bounds.top), any ? static_cast<float>(bounds.right - bounds.left) : 32.0f, any ? static_cast<float>(bounds.bottom - bounds.top) : 18.0f);
    }
    void DrawCanvas(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        const auto input = ui.BeginCanvas("td2.world.canvas", 0);
        canvasHovered_ = input.hovered != 0;
        view_.ObserveSize(input.size);
        TrackDrag(input);
        if (input.size.x > 0 && input.size.y > 0) {
            if (view_.fit && (!model_->gesture || !view_.hasFit)) { FitWorld(input.size); }
            view_.Navigate(input, !pickingStart_ && hand_, moving_ || libraryDrag_);
            DrawCanvasGrid(ui, view_, input.size);
            const auto p = view_.World(input.pointer);
            const bool geometricInside = PointIn(input.pointer, {0, 0}, input.size);
            if (mode_ == 1 && !pickingStart_ && !hand_ && !view_.panning && input.hovered && (input.pressedButtons & kPrimary)) {
                for (auto it = model_->rooms.rbegin(); it != model_->rooms.rend(); ++it) {
                    if (ContainsWorldPoint(Placement(it->settings), {p.x, p.y})) { ToggleArea(ctx, it->settings.region.regionId); break; }
                }
            }
            if (mode_ == 3 && !pickingStart_ && !hand_ && !view_.panning && input.hovered && (input.pressedButtons & kPrimary)) { MachinePress(ctx, p); }
            if (mode_ == 3) { MachineDrag(ctx, input, p); }
            if (mode_ == 0) { GroupDrag(ctx, input, p); }
            if (mode_ == 0 && !pickingStart_ && !moving_ && !libraryDrag_ && !groupMoving_ && !boxing_ && !hand_ && !view_.panning && input.hovered &&
                (input.pressedButtons & kPrimary) && GroupPress(ctx, input, p)) {}
            else if (mode_ == 0 && !pickingStart_ && !moving_ && !libraryDrag_ && !groupMoving_ && !boxing_ && !hand_ && !view_.panning && input.hovered && (input.pressedButtons & kPrimary)) {
                const auto edge = EdgeAt(input.pointer);
                if (edge) { ChooseEdge(ctx, *edge); }
                else {
                    for (auto it = model_->rooms.rbegin(); it != model_->rooms.rend(); ++it) {
                        if (!ContainsWorldPoint(Placement(it->settings), {p.x, p.y})) { continue; }
                        const auto id = it->object.objectId;
                        if (!LeaveSequence(ctx) || !model_->Select(ctx, id)) { break; }
                        if (lastClick_ == id && clock_ - lastClickTime_ < .3f) { OpenTelevision(ctx, id); lastClick_ = 0; }
                        else {
                            moving_ = true; model_->gesture = true; offset_ = {p.x - static_cast<float>(model_->draft.worldX), p.y - static_cast<float>(model_->draft.worldY)};
                            lastClick_ = id; lastClickTime_ = clock_;
                        }
                        break;
                    }
                }
            }
            if ((moving_ || libraryDrag_) && model_->ready) {
                // ドラッグ元が動いている間は落とし先のホバーを抑える。プレビュー範囲はローカルのポインタで判定する。
                if (moving_ || geometricInside) {
                    model_->draft.worldPlaced = true;
                    constexpr float limit = 1000000;
                    model_->draft.worldX = static_cast<int>(std::round(std::clamp(p.x - offset_.x, -limit, limit)));
                    model_->draft.worldY = static_cast<int>(std::round(std::clamp(p.y - offset_.y, -limit, limit)));
                }
                // ドラッグ元を先に描き、落とし先の処理の後で終了を判定する。
                if (dragReleasedAny_ || (!dragHeldAny_ && !dragPressedAny_)) {
                    const bool accept = moving_ || geometricInside;
                    moving_ = false; libraryDrag_ = false;
                    if (accept) { model_->Finish(ctx, true); }
                    else { model_->Discard(); }
                }
            }
            for (const auto& room : model_->rooms) {
                auto s = room.object.objectId == model_->selected && model_->ready ? model_->draft : room.settings;
                if (!s.worldPlaced) { continue; }
                // 複数選択の平行移動の途中は、動かした先に描く。
                if (groupMoving_ && multi_.contains(room.object.objectId)) { s.worldX += groupDx_; s.worldY += groupDy_; }
                MapCanvasView local = view_; local.origin = view_.Screen(static_cast<float>(s.worldX), static_cast<float>(s.worldY));
                DrawRoomMap(ui, s, local, view_.tile >= 16, false);
                const auto a = local.Screen(0, 0), b = local.Screen(static_cast<float>(s.region.cols), static_cast<float>(s.region.rows));
                if (layoutValid_) {
                    const int area = AreaIndex(s.region.regionId);
                    if (area >= 0) {
                        auto tint = AreaColor(area); tint.a = mode_ == 1 ? .26f : .10f;
                        ui.CanvasFill(a, b, tint);
                        if (mode_ == 1 && view_.tile >= 6) {
                            auto label = AreaColor(area); label.a = 1;
                            ui.CanvasText({a.x + 4, b.y - 18}, label, ShortChoiceLabel(layout_.areas[static_cast<size_t>(area)].name, 10));
                        }
                    }
                }
                const bool selected = (room.object.objectId == model_->selected && edge_.a == 0) || multi_.contains(room.object.objectId);
                const bool invalid = model_->gesture && selected && !model_->Validate(s).empty();
                ui.CanvasRect(a, b, invalid ? EditorWorkspaceColor{.92f, .36f, .38f, 1} : selected ? EditorWorkspaceColor{.93f, .78f, .4f, 1} : EditorWorkspaceColor{.32f, .49f, .53f, 1}, selected ? 3.0f : 1.0f);
                if (selected && view_.tile >= 16) { ui.CanvasText({a.x + 5, a.y + 4}, {.9f, .94f, .94f, 1}, room.object.name); }
            }
            bool selectedEdgeExists = false;
            for (const auto& e : Edges()) {
                const auto [a, b] = EdgePoints(e.edge);
                const bool selected = e.a == edge_.a && e.b == edge_.b;
                if (selected) { edge_ = e; selectedEdgeExists = true; }
                const bool vertical = e.edge.sideA == WorldSide::Left || e.edge.sideA == WorldSide::Right;
                const bool hovered = input.hovered && (vertical ? std::abs(input.pointer.x - a.x) <= 5 && input.pointer.y >= a.y && input.pointer.y <= b.y : std::abs(input.pointer.y - a.y) <= 5 && input.pointer.x >= a.x && input.pointer.x <= b.x);
                if (selected || hovered) { ui.CanvasLine(a, b, selected ? EditorWorkspaceColor{.97f, .79f, .39f, 1} : EditorWorkspaceColor{.55f, .74f, .72f, .75f}, selected ? 4.0f : 2.0f); }
                else if (layoutValid_) { DrawEdgeKind(ui, e, a, b); }
            }
            if (edge_.a && !selectedEdgeExists) {
                if (sequenceDraft_) { model_->error = "该相邻区域已移动；请撤回演出草稿"; }
                else { edge_ = {}; bindingRefresh_ = true; }
            }
            if (mode_ == 3) { DrawMachineOverlay(ui); }
            if (boxing_) { ui.CanvasRect(view_.Screen((std::min)(boxStart_.x, p.x), (std::min)(boxStart_.y, p.y)), view_.Screen((std::max)(boxStart_.x, p.x), (std::max)(boxStart_.y, p.y)), {.95f, .78f, .4f, 1}, 1.5f); }
            DrawSpawnPicker(ctx, input, p);
        }
        ui.EndCanvas();
        // 配置の見方だけで、選んだテレビを世界から外す（ほかの見方の Delete では外さない）。
        // 地図の上にマウスがある時だけ（3D ビューポートの Delete は物体を消すので、同じキーでテレビまで外さない）。
        if (canvasHovered_ && mode_ == 0 && multi_.empty() && !pickingStart_ && !model_->gesture && !ui.GetInteraction().editing && ui.GetInteraction().deletePressed && edge_.a == 0 && model_->ready) { model_->draft.worldPlaced = false; model_->Commit(ctx); }
    }
    // 配置のドラッグと試遊のクリックは排他的に扱う。マウスは人物の足元中央を指し、半マスへ丸めない。
    void DrawSpawnPicker(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint point) {
        if (!pickingStart_ || !input.hovered || view_.panning) { return; }
        if (input.pressedButtons & kSecondary) { pickingStart_ = false; return; }
        for (const auto& room : model_->rooms) {
            const auto& s = room.settings;
            if (!ContainsWorldPoint(Placement(s), {point.x, point.y})) { continue; }
            const PreviewSpawn spawn{double(point.x) - s.worldX - .3125, double(point.y) - s.worldY - .875};
            bool clear = spawn.x >= 0 && spawn.y >= 0 && spawn.x + .625 <= s.region.cols && spawn.y + .875 <= s.region.rows;
            // 壁の中は赤い枠で示し、別の場所へは移さない。空中や仕掛けの近くは自由に指定できる。
            if (clear && ValidateRegion(s.region).valid) {
                for (int y = static_cast<int>(std::floor(spawn.y)); y < static_cast<int>(std::ceil(spawn.y + .875)); ++y)
                    for (int x = static_cast<int>(std::floor(spawn.x)); x < static_cast<int>(std::ceil(spawn.x + .625)); ++x)
                        if (s.region.cells[CellIndex(s.region, x, y)] == '1') { clear = false; }
            } else { clear = false; }
            const auto a = view_.Screen(point.x - .3125f, point.y - .875f), b = view_.Screen(point.x + .3125f, point.y);
            const EditorWorkspaceColor color = clear ? EditorWorkspaceColor{.4f, 1, .7f, 1} : EditorWorkspaceColor{1, .3f, .3f, 1};
            ctx.ui->CanvasRect(a, b, color, 2);
            ctx.ui->CanvasCircle(view_.Screen(point.x, point.y - .7f), view_.tile * .13f, color, 2);
            if (input.pressedButtons & kPrimary) {
                if (!clear) { model_->error = "这里放不下角色，请在区域内的空处点击；也可以选空中"; return; }
                const auto entry = EncodePreviewSpawn(spawn);
                const auto started = Execute(ctx, "play.preview", {{"spawnPointId", entry}, {"userData", room.object.objectId}});
                if (started.ok) { pickingStart_ = false; model_->error.clear(); }
                else {
                    model_->error = started.error.find("OPERATION_NOT_FOUND") != std::string::npos
                        ? "当前引擎尚未更新：请保存并关闭编辑器，完成引擎构建后重新打开，才能从点击位置开始游玩。"
                        : started.error;
                }
            }
            return;
        }
    }
    void DrawProperties(EditorWorkspaceHostContext& ctx);
    // ---- カメラ（区域カメラ・大区域・切り替え演出）の作者編集 ----
    void SetMode(EditorWorkspaceHostContext& ctx, int mode) {
        if (mode == mode_) { return; }
        if (!LeaveSequence(ctx) || !ApplyDraft(ctx)) { return; }
        if (posing_) { CancelPose(ctx); }
        mode_ = mode; layoutStale_ = true; pickingStart_ = false; multi_.clear(); ghost_ = -1; libraryOpen_ = mode == 0;
        if (mode_ == 3) { model_->machinesStale = true; checkClock_ = 10; }
    }
    bool EnsureLayout(EditorWorkspaceHostContext& ctx, bool force = false) {
        layoutClock_ += ctx.unscaledDeltaSeconds;
        // 他の操作（取り消しや標準の Inspector など）の変更も拾うよう、見ている間は時々読み直す。
        if (!force && !layoutStale_ && layoutRevision_ == model_->revision && layoutClock_ < 2.f) { return layoutValid_; }
        layoutClock_ = 0; layoutStale_ = false; layoutRevision_ = model_->revision;
        std::string error;
        layoutValid_ = LoadLayout(ctx, layout_, error);
        if (!layoutValid_) { model_->error = error; return false; }
        // 俯瞰図の切り替えの経路（今のカメラ位置から）。
        planPaths_.clear();
        std::map<std::string, CameraPose, std::less<>> cameras;
        for (const auto& room : layout_.rooms) { if (room.cameraId) { cameras[room.regionId] = CurrentCameraPose(room); } }
        for (const auto& edge : layout_.edges) {
            if (!edge.between || !edge.passable) { continue; }
            const auto plan = PlanTravel(layout_, edge, cameras);
            PlanPath path{edge.a, edge.b, {}, plan.problem};
            for (int i = 0; i <= 24; ++i) { path.points.push_back(SampleTravel(plan.keys, i / 24.0).eye); }
            planPaths_.push_back(std::move(path));
        }
        return true;
    }
    int AreaIndex(std::string_view regionId) const {
        for (size_t i = 0; i < layout_.areas.size(); ++i) {
            const auto& r = layout_.areas[i].regions;
            if (std::find(r.begin(), r.end(), regionId) != r.end()) { return static_cast<int>(i); }
        }
        return -1;
    }
    static EditorWorkspaceColor AreaColor(int index) {
        // 抑えた色数：琥珀・青緑・菫・砂・青・苔。
        constexpr std::array<EditorWorkspaceColor, 6> colors{{{.93f, .72f, .36f, 1}, {.36f, .78f, .74f, 1}, {.68f, .55f, .92f, 1},
                                                              {.82f, .74f, .56f, 1}, {.42f, .62f, .92f, 1}, {.60f, .78f, .45f, 1}}};
        return colors[static_cast<size_t>(index) % colors.size()];
    }
    const LayoutEdge* LayoutEdgeOf(const EdgeSelection& e) const {
        const auto ra = std::find_if(model_->rooms.begin(), model_->rooms.end(), [&](const auto& r) { return r.object.objectId == e.a; });
        const auto rb = std::find_if(model_->rooms.begin(), model_->rooms.end(), [&](const auto& r) { return r.object.objectId == e.b; });
        if (ra == model_->rooms.end() || rb == model_->rooms.end()) { return nullptr; }
        const auto& ia = ra->settings.region.regionId; const auto& ib = rb->settings.region.regionId;
        for (const auto& edge : layout_.edges) { if ((edge.a == ia && edge.b == ib) || (edge.a == ib && edge.b == ia)) { return &edge; } }
        return nullptr;
    }
    void DrawEdgeKind(IEditorWorkspaceUi& ui, const EdgeSelection& e, EditorWorkspacePoint a, EditorWorkspacePoint b) const {
        const auto* edge = LayoutEdgeOf(e);
        if (!edge) { return; }
        if (!edge->passable) { ui.CanvasLine(a, b, {.32f, .36f, .38f, .7f}, 1.0f); return; }   // 壁で隔てた辺（越えられない）
        if (edge->between) {
            // 大区域どうし：演出が流れる。演出がまだ無い辺は赤みの点で知らせる。
            const EditorWorkspaceColor color = edge->sequenceGuid.empty() ? EditorWorkspaceColor{.95f, .45f, .40f, .95f} : EditorWorkspaceColor{.97f, .79f, .39f, .9f};
            ui.CanvasLine(a, b, color, 3.0f);
            ui.CanvasCircle({(a.x + b.x) * .5f, (a.y + b.y) * .5f}, 4.5f, color, 2.0f);
            return;
        }
        // 大区域の中：点線（カメラは自動で追従し、演出は流れない）。
        const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
        for (float t = 0; t < length; t += 10) {
            const float t1 = (std::min)(t + 5, length);
            ui.CanvasLine({a.x + dx * t / length, a.y + dy * t / length}, {a.x + dx * t1 / length, a.y + dy * t1 / length}, {.45f, .70f, .68f, .65f}, 1.5f);
        }
    }
    void ShowReport(const std::string& title, const GenerateReport& report) {
        report_ = title + "：写入 " + std::to_string(report.written) + "，保留手调 " + std::to_string(report.kept) +
                  (report.failed ? "，失败 " + std::to_string(report.failed) : std::string());
        for (size_t i = 0; i < report.notes.size() && i < 6; ++i) { report_ += "\n· " + report.notes[i]; }
    }
    // 生成は途中で layout_ を何度も読み直す。呼び手が layout_ の中を指す名前を渡すことがあるので、先に値で写す。
    // 呼び手も生成の後は layout_ の中を指すものを使わずに戻ること。
    void Generate(EditorWorkspaceHostContext& ctx, bool cameras, bool travels, std::string_view areaName, std::string_view roomId,
                  const LayoutEdge* onlyEdge, bool force) {
        const std::string area(areaName), room(roomId);
        const std::optional<LayoutEdge> edgeCopy = onlyEdge ? std::optional<LayoutEdge>(*onlyEdge) : std::nullopt;
        const LayoutEdge* edge = edgeCopy ? &*edgeCopy : nullptr;
        if (!ApplyDraft(ctx) || !EnsureLayout(ctx, true)) { return; }
        std::string error; GenerateReport cameraReport, travelReport;
        // 画面の寸法はテレビの型から自動で決める。
        if (!SyncScreenGeometry(ctx, layout_, cameraReport, error)) { model_->error = error; return; }
        cameraReport = {};
        if (cameras && !GenerateCameras(ctx, layout_, area, room, force, cameraReport, error)) { model_->error = error; return; }
        if (travels && !GenerateTravels(ctx, layout_, area, edge, force, travelReport, error)) { model_->error = error; return; }
        GenerateReport merged;
        merged.written = cameraReport.written + travelReport.written; merged.kept = cameraReport.kept + travelReport.kept;
        merged.failed = cameraReport.failed + travelReport.failed;
        merged.notes = cameraReport.notes; merged.notes.insert(merged.notes.end(), travelReport.notes.begin(), travelReport.notes.end());
        ShowReport(cameras && travels ? "机位与演出" : cameras ? "机位" : "演出", merged);
        model_->sceneDirty = true; model_->error.clear(); layoutStale_ = true; model_->refresh = true;
    }
    void ToggleArea(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
        if (!EnsureLayout(ctx, true)) { return; }
        const auto focus = std::find_if(layout_.areas.begin(), layout_.areas.end(), [this](const auto& a) { return a.name == areaFocus_; });
        if (focus == layout_.areas.end()) { model_->error = "先在右栏选择一个大区域"; return; }
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
        if (!Transact(ctx, commands, model_->error)) { return; }
        model_->sceneDirty = true; model_->error.clear(); layoutStale_ = true;
    }
    void DrawAreaPanel(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        if (!layoutValid_) { ui.Text("正在读取镜头布局…"); return; }
        ui.Text("镜头大区域");
        for (size_t i = 0; i < layout_.areas.size(); ++i) {
            const auto& area = layout_.areas[i];
            if (ui.Selectable("td2.world.area." + std::to_string(area.objectId), "■ " + ShortChoiceLabel(area.name, 14) + "  " + std::to_string(area.regions.size()) + " 台", area.name == areaFocus_)) {
                areaFocus_ = area.name;
            }
        }
        if (ui.Button("td2.world.area.new", "+ 新建大区域")) {
            std::string error;
            const auto name = "镜头大区域 " + std::to_string(layout_.areas.size() + 1);
            if (CreateArea(ctx, name, {}, error)) { areaFocus_ = name; layoutStale_ = true; model_->sceneDirty = true; } else { model_->error = error; }
        }
        // 大区域に入っていないテレビは、一台だけの大区域として扱う（出入りに演出が流れる）。
        int loose = 0;
        for (const auto& room : layout_.rooms) { if (room.placed && AreaIndex(room.regionId) < 0) { ++loose; } }
        if (loose) { ui.Text(std::to_string(loose) + " 台电视不属于任何大区域"); }
        ui.Separator();
        const auto focus = std::find_if(layout_.areas.begin(), layout_.areas.end(), [this](const auto& a) { return a.name == areaFocus_; });
        if (focus != layout_.areas.end()) {
            ui.Text(focus->name);
            if (ui.Button("td2.world.area.inspect", "全部参数（检视面板）")) {
                const auto selected = Execute(ctx, "scene.object.select", {{"objectId", focus->objectId}});
                if (!selected.ok) { model_->error = selected.error; }
            }
            // カメラの構図：画面の占める高さ・出口側へ斜めに撮る角度・画面より上げる高さ。変えたらカメラの生成で反映する。
            double fill = focus->frameFill, orbit = focus->orbitDegrees, lift = focus->cameraLift;
            bool changed = EditFloat(ui, "td2.world.area.fill", "屏幕占画面高度", fill, .15f, .8f);
            changed |= EditFloat(ui, "td2.world.area.orbit", "向出口侧斜拍（度）", orbit, 0, 40);
            changed |= EditFloat(ui, "td2.world.area.lift", "机位高于屏幕（米）", lift, -1, 2);
            if (changed) { areaDraft_ = {fill, orbit, lift}; areaDraftPending_ = true; areaDraftObject_ = focus->objectId; areaDraftComponent_ = focus->componentId; }
            if (areaDraftPending_ && !ui.GetInteraction().editing) {
                const auto set = [this](std::string_view id, double v) { return PropertyCommand(areaDraftObject_, areaDraftComponent_, id, v); };
                (void)Transact(ctx, Json::array({set("frameFill", areaDraft_[0]), set("orbitDegrees", areaDraft_[1]), set("cameraLift", areaDraft_[2])}), model_->error);
                areaDraftPending_ = false; layoutStale_ = true; model_->sceneDirty = true;
            }
            // 生成の後は layout_ が作り直されて focus が指す先も消えるので、このフレームの残りは描かずに戻る。
            if (ui.Button("td2.world.area.cameras", "按当前布局生成机位")) { Generate(ctx, true, false, focus->name, {}, nullptr, false); return; }
            if (ui.Button("td2.world.area.travels", "生成出入本区域的切换演出")) { Generate(ctx, false, true, focus->name, {}, nullptr, false); return; }
            if (ui.Button("td2.world.area.force", "恢复自动（覆盖本区域的手调）")) { Generate(ctx, true, true, focus->name, {}, nullptr, true); return; }
        }
        ui.Separator();
        int dead = 0;
        for (const auto& edge : layout_.edges) { if ((!edge.between || !edge.passable) && edge.bindingObject) { ++dead; } }
        if (dead && ui.Button("td2.world.area.prune", "清理大区域内的旧演出绑定（" + std::to_string(dead) + "）")) {
            int removed = 0; std::string error;
            if (PruneInAreaBindings(ctx, layout_, removed, error)) { report_ = "已移除 " + std::to_string(removed) + " 个大区域内的旧绑定（演出文件仍保留在磁盘上）"; layoutStale_ = true; model_->sceneDirty = true; }
            else { model_->error = error; }
        }
        if (ui.Button("td2.world.area.all", "全部大区域：生成机位与演出")) { Generate(ctx, true, true, {}, {}, nullptr, false); return; }
    }
    void DrawRoomCamera(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
        auto& ui = *ctx.ui;
        ui.Separator(); ui.Text("区域机位");
        EnsureLayout(ctx);
        const auto* room = layoutValid_ ? layout_.Room(regionId) : nullptr;
        if (room) {
            // 状態：自動（再生成で置き直す）・手で調整済み／作者が置いた（残す）・ロック済み。
            const std::string state = !room->cameraId ? "没有机位" : room->cameraLocked ? "已锁定 · 重新生成时保留"
                : room->cameraAuto.empty() ? "作者放置 · 重新生成时保留" : CameraEdited(*room) ? "已手调 · 重新生成时保留" : "自动";
            ui.Text(state);
            if (const auto* area = layout_.AreaOf(regionId)) { ui.Text("大区域：" + ShortChoiceLabel(area->name, 14)); } else { ui.Text("未加入大区域"); }
        }
        if (ui.Button("td2.world.room.camera", "预览 / 调整机位")) {
            if (model_->Commit(ctx)) {
                const auto* item = model_->Current();
                OpenRegionCamera(ctx, regionId, item ? item->object.name : regionId, model_->error);
                layoutStale_ = true;
            }
        }
        // 生成の後は layout_ が作り直されて room が指す先が消えるので、すぐ戻る（次のフレームで読み直した内容を描く）。
        if (ui.Button("td2.world.room.camera.generate", room && room->cameraId ? "按当前布局重新生成" : "生成机位")) { Generate(ctx, true, false, {}, regionId, nullptr, false); return; }
        if (room && room->cameraId && (room->cameraLocked || CameraEdited(*room))) {
            ui.SameLine(); if (ui.Button("td2.world.room.camera.reset", "恢复自动")) { Generate(ctx, true, false, {}, regionId, nullptr, true); return; }
        }
    }
    void DrawEdgeCamera(EditorWorkspaceHostContext& ctx, const std::string& ra, const std::string& rb) {
        auto& ui = *ctx.ui;
        EnsureLayout(ctx);
        const LayoutEdge* edge = nullptr;
        for (const auto& e : layout_.edges) { if ((e.a == ra && e.b == rb) || (e.a == rb && e.b == ra)) { edge = &e; } }
        if (!edge) { ui.Text("正在读取镜头布局…"); return; }
        if (edgeDraftKey_ != edge->a + "|" + edge->b) { edgeDraft_ = *edge; edgeDraftKey_ = edge->a + "|" + edge->b; }
        if (!edge->passable) {
            ui.Text("这条边被地形隔开，角色无法越过 · 不需要镜头切换");
            return;
        }
        if (!edge->between) {
            ui.Text("同一大区域内 · 镜头自动跟随，无演出");
        } else {
            ui.Text(edge->sequenceGuid.empty() ? "大区域交界 · 尚未生成演出" : edge->keepAuthored ? "大区域交界 · 保留手调" : "大区域交界 · 演出");
            ui.Text(ShortChoiceLabel(edge->a, 12) + " → " + ShortChoiceLabel(edge->b, 12));
            // 様式（選ぶとすぐ反映し、生成の時に演出へ書く）。
            bool changed = false;
            for (const auto& style : TravelStyles()) {
                const bool current = edgeDraft_.style == style.id;
                if (ui.Selectable("td2.world.edge.style." + std::string(style.id), std::string(style.label) + (style.id == std::string_view("auto") ? "（" + ResolveStyleLabel(*edge) + "）" : ""), current)) {
                    edgeDraft_.style = std::string(style.id); changed = true;
                }
            }
            double seconds = edgeDraft_.seconds;
            if (EditFloat(ui, "td2.world.edge.seconds", "全速时长（秒）", seconds, .4f, 10.f)) { edgeDraft_.seconds = seconds; changed = true; }
            changed |= ui.Checkbox("td2.world.edge.keep", "保留手调（重新生成时不覆盖）", &edgeDraft_.keepAuthored);
            if (changed) { edgeDraftPending_ = true; }
            if (edgeDraftPending_ && !ui.GetInteraction().editing) {
                std::string error;
                if (!WriteEdgeSettings(ctx, edgeDraft_, error)) { model_->error = error; }
                edgeDraftPending_ = false; layoutStale_ = true; model_->sceneDirty = true;
            }
            // 生成の後は layout_ が作り直されて edge が指す先が消えるので、すぐ戻る（次のフレームで読み直した内容を描く）。
            if (ui.Button("td2.world.edge.generate", edge->sequenceGuid.empty() ? "生成演出" : "按当前机位重新生成")) { Generate(ctx, false, true, {}, {}, &edgeDraft_, false); return; }
            if (!edge->sequenceGuid.empty()) {
                ui.SameLine(); if (ui.Button("td2.world.edge.force", "恢复自动")) { auto draft = edgeDraft_; draft.keepAuthored = false; Generate(ctx, false, true, {}, {}, &draft, true); return; }
                if (ui.Button("td2.world.edge.edit", "在演出工作区编辑 / 预览")) {
                    RegionSequenceBinding binding; binding.regionA = edge->a; binding.regionB = edge->b;
                    binding.sequenceAssetGuid = edge->sequenceGuid; binding.speed = static_cast<float>(edge->speed);
                    OpenTransitionCamera(ctx, binding, model_->error);
                }
            }
        }
        // 試遊：この辺のどちら側からでも、歩いて越える位置から始める。
        ui.Text("从这条边试走");
        if (ui.Button("td2.world.edge.trial.a", "→ 从 " + ShortChoiceLabel(edge->a, 10) + " 出发")) { TrialFromEdge(ctx, *edge, true); }
        if (ui.Button("td2.world.edge.trial.b", "← 从 " + ShortChoiceLabel(edge->b, 10) + " 出发")) { TrialFromEdge(ctx, *edge, false); }
    }
    std::string ResolveStyleLabel(const LayoutEdge& edge) const {
        auto copy = edge; copy.style = "auto";
        const auto id = ResolveStyle(layout_, copy);
        for (const auto& style : TravelStyles()) { if (style.id == id) { return std::string(style.label); } }
        return id;
    }
    void TrialFromEdge(EditorWorkspaceHostContext& ctx, const LayoutEdge& edge, bool fromA) {
        if (!ApplyDraft(ctx)) { return; }
        double x = 0, y = 0; uint64_t room = 0;
        if (!EdgeTrialSpawn(layout_, edge, fromA, x, y, room)) { model_->error = "这条边附近没有能站立的位置；请用「从此区域开始」手动点选"; return; }
        const auto started = Execute(ctx, "play.preview", {{"spawnPointId", EncodePreviewSpawn({x, y})}, {"userData", room}});
        model_->error = started.ok ? std::string{} : started.error;
    }
    // 俯瞰図：テレビ（色は大区域）・画面の向き・区域カメラ（視錐）・大区域間の経路・障害物を上から見る。
    void DrawPlan(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        const auto input = ui.BeginCanvas("td2.world.plan", 0);
        if (input.size.x > 0 && input.size.y > 0 && layoutValid_) {
            ui.CanvasFill({0, 0}, input.size, {.06f, .08f, .09f, 1});
            // 範囲：テレビとカメラの外接（xz）。
            double x0 = 1e9, z0 = 1e9, x1 = -1e9, z1 = -1e9;
            const auto grow = [&](const Vec3& p) { x0 = (std::min)(x0, p[0]); x1 = (std::max)(x1, p[0]); z0 = (std::min)(z0, p[2]); z1 = (std::max)(z1, p[2]); };
            for (const auto& r : layout_.rooms) { if (r.placed) { grow(r.tv.position); if (r.cameraId) { grow(r.camera.position); } } }
            if (x1 <= x0) { ui.EndCanvas(); return; }
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
                const bool selected = model_->Current() && model_->Current()->settings.region.regionId == r.regionId;
                ui.CanvasLine(corner(-hw, -hd), corner(hw, -hd), color, selected ? 3.f : 1.5f);
                ui.CanvasLine(corner(hw, -hd), corner(hw, hd), color, selected ? 3.f : 1.5f);
                ui.CanvasLine(corner(-hw, hd), corner(-hw, -hd), color, selected ? 3.f : 1.5f);
                ui.CanvasLine(corner(-hw, hd), corner(hw, hd), {.95f, .96f, .92f, 1}, selected ? 4.f : 2.5f);   // 画面の側
                if (r.cameraId) {
                    const auto eye = P(r.camera.position[0], r.camera.position[2]);
                    const double cy = r.camera.rotation[1] * Math::kPi / 180;
                    const auto ray = [&](double offset) {
                        const double a = cy + offset;
                        return P(r.camera.position[0] + std::sin(a) * 1.4, r.camera.position[2] + std::cos(a) * 1.4);
                    };
                    const auto camColor = r.cameraLocked || CameraEdited(r) ? EditorWorkspaceColor{.97f, .79f, .39f, 1} : EditorWorkspaceColor{.55f, .82f, .88f, 1};
                    ui.CanvasLine(eye, ray(-.39), camColor, 1.2f); ui.CanvasLine(eye, ray(.39), camColor, 1.2f);
                    ui.CanvasCircle(eye, selected ? 5.f : 3.5f, camColor, 2);
                    const double dx = input.pointer.x - eye.x, dy = input.pointer.y - eye.y;
                    if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; clicked = r.objectId; }
                }
                const auto center = corner(0, 0);
                const double dx = input.pointer.x - center.x, dy = input.pointer.y - center.y;
                if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; clicked = r.objectId; }
                if (scale > 18) { ui.CanvasText({center.x + 6, center.y - 6}, {.85f, .88f, .88f, .9f}, ShortChoiceLabel(r.regionId, 10)); }
            }
            ui.CanvasText({10, 8}, {.75f, .8f, .8f, 1}, "俯视：白边为屏幕一侧 · 蓝点为自动机位 / 琥珀为已手调 · 线为大区域间的切换路线（红色为有问题）");
            if (input.hovered && (input.pressedButtons & kPrimary) && clicked) {
                for (const auto& room : model_->rooms) { if (room.object.objectId == clicked) { if (LeaveSequence(ctx)) { model_->Select(ctx, clicked); } break; } }
            }
        }
        ui.EndCanvas();
    }
    struct PlanPath { std::string a, b; std::vector<Vec3> points; std::string problem; };
    // ---- 配置：複数選択と平行移動、矩形選択
    bool GroupPress(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void GroupDrag(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    std::set<uint64_t> multi_;
    bool groupMoving_ = false, boxing_ = false;
    EditorWorkspacePoint groupPointer_{}, boxStart_{};
    int groupDx_ = 0, groupDy_ = 0;
    // ---- 预算
    void DrawBudget(EditorWorkspaceHostContext& ctx);
    bool budgetOpen_ = false;
    std::vector<std::string> budget_;
    // ---- 仕掛けの見方
    Machine* SelectedMachine() {
        const auto it = std::find_if(model_->machines.begin(), model_->machines.end(), [this](const Machine& m) { return m.objectId == machineObject_ && m.componentId == machineComponent_; });
        return it == model_->machines.end() ? nullptr : &*it;
    }
    std::optional<RegionPlacement> PlacementOf(const std::string& regionId) const {
        for (const auto& room : model_->rooms) { if (room.settings.region.regionId == regionId) { return Placement(room.settings); } }
        return std::nullopt;
    }
    bool WriteSelected(EditorWorkspaceHostContext& ctx, const Machine& after);
    void ToggleMember(EditorWorkspaceHostContext& ctx, const std::string& regionId);
    void MachinePress(EditorWorkspaceHostContext& ctx, EditorWorkspacePoint p);
    void MachineDrag(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void DrawMachineOverlay(IEditorWorkspaceUi& ui);
    void DrawMachinePanel(EditorWorkspaceHostContext& ctx);
    void DrawMechanism(EditorWorkspaceHostContext& ctx, const Machine m);
    void RefreshCheckInput(EditorWorkspaceHostContext& ctx);
    bool RenameMachine(EditorWorkspaceHostContext& ctx, const Machine& before, const std::string& id);
    bool ReadPoses(EditorWorkspaceHostContext& ctx, std::map<uint64_t, Pose>& poses, std::map<uint64_t, Json>& expected, std::string& sceneId, uint64_t& revision);
    bool BeginPose(EditorWorkspaceHostContext& ctx);
    bool SavePose(EditorWorkspaceHostContext& ctx);
    void CancelPose(EditorWorkspaceHostContext& ctx);
    uint64_t machineObject_ = 0, machineComponent_ = 0;
    bool machineC_ = false, posing_ = false, posingC_ = false, confirmDelete_ = false;
    std::map<uint64_t, Pose> poseA_;
    int ghost_ = -1, ghostDx_ = 0, ghostDy_ = 0, newKind_ = 0;
    EditorWorkspacePoint ghostGrab_{};
    std::optional<Machine> ghostPreview_;
    std::string newMachineId_ = "machine1", machineIdDraft_;
    MachineCheckInput checkInput_;
    std::map<uint64_t, std::string> objectNames_;
    float checkClock_ = 10;
    std::optional<Machine> pendingMachine_;
    int mode_ = 0;
    LayoutSnapshot layout_;
    bool layoutValid_ = false, layoutStale_ = true;
    uint64_t layoutRevision_ = 0;
    float layoutClock_ = 0;
    std::vector<PlanPath> planPaths_;
    std::string areaFocus_, report_, librarySearch_;
    std::array<double, 3> areaDraft_{};
    bool areaDraftPending_ = false;
    uint64_t areaDraftObject_ = 0, areaDraftComponent_ = 0;
    LayoutEdge edgeDraft_;
    std::string edgeDraftKey_;
    bool edgeDraftPending_ = false, edgeAdvanced_ = false;
    void ReadBinding(EditorWorkspaceHostContext& ctx);
    bool WriteBinding(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& next);
    void RefreshAssets(EditorWorkspaceHostContext& ctx);
    bool ReadAssetSlots(EditorWorkspaceHostContext& ctx, SequenceAsset& asset);
    EdgeSelection edge_;
    EditorWorkspacePoint offset_{};
    bool moving_ = false, libraryDrag_ = false, propertyPending_ = false;
    bool pickingStart_ = false;
    bool dragHeldAny_ = false, dragPressedAny_ = false, dragReleasedAny_ = false;
    bool assetsRefresh_ = true, bindingRefresh_ = true;
    bool sequenceAdvanced_ = false;
    float clock_ = 0, lastClickTime_ = -1;
    uint64_t lastClick_ = 0;
    RegionTarget bindingTarget_;
    RegionSequenceBinding binding_;
    RegionSequenceBinding observedBinding_;
    // 初回も保存前にスロットとカメラを選べる。Scene の値とは分けて持つ。
    RegionSequenceBinding pendingBinding_, pendingObserved_;
    RegionTarget pendingTarget_;
    bool sequenceDraft_ = false;
    uint64_t bindingRevision_ = 0;
    bool bindingValid_ = false;
    std::vector<SequenceAsset> assets_;
    std::vector<SceneItem> cameras_;
    int picker_ = 0, assetPage_ = 0, slotPage_ = 0, cameraPage_ = 0, neighborPage_ = 0, roomPage_ = 0;
    std::string assetSearch_, cameraSearch_;
    bool canvasHovered_ = false, libraryOpen_ = true;
};
}
