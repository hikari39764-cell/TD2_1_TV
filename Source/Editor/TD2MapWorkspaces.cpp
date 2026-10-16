#include "TD2MapWorkspaces.h"
#include "TD2MapCanvas.h"
#include "TD2EditorChoices.h"
#include "TD2CameraAuthoring.h"
#include "TD2CameraLayout.h"
#include "../Common/TD2WorldLayout.h"
#include "../Common/TD2PreviewEntry.h"
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <set>

namespace TD2::Editor {
namespace {
constexpr uint32_t kPrimary = static_cast<uint32_t>(EditorWorkspacePointerButton::Primary);
constexpr uint32_t kSecondary = static_cast<uint32_t>(EditorWorkspacePointerButton::Secondary);
constexpr const char* kTelevisionWorkspace = "td2.television_region";
constexpr const char* kWorldWorkspace = "td2.world_map";

struct RoomItem {
    SceneItem object;
    RegionTarget target;
    RegionSettings settings;
};

RegionPlacement Placement(const RegionSettings& s) {
    return {s.region.regionId, s.region.cols, s.region.rows, s.worldPlaced, s.worldX, s.worldY};
}

// Scene の作者データを共有する。二つのワークスペースは同じ選択と草稿を見る。
class MapModel {
public:
    std::vector<RoomItem> rooms;
    uint64_t selected = 0;
    RegionSettings draft;
    DeviceEditor devices;
    bool ready = false, gesture = false, refresh = true;
    float elapsed = 0;
    uint64_t revision = 0;
    bool sceneDirty = false, savedOnce = false;
    std::string error, published, screenWarning;

    RoomItem* Current() {
        const auto it = std::find_if(rooms.begin(), rooms.end(), [this](const auto& r) { return r.object.objectId == selected; });
        return it == rooms.end() ? nullptr : &*it;
    }
    const RoomItem* Current() const { return const_cast<MapModel*>(this)->Current(); }
    bool Dirty() const { const auto* item = Current(); return ready && item && draft != item->settings; }
    void Discard() { if (const auto* item = Current()) { draft = item->settings; devices.Load(draft.devices); } gesture = false; error.clear(); screenWarning.clear(); }
    void Poll(EditorWorkspaceHostContext& ctx, bool force = false) {
        elapsed += ctx.unscaledDeltaSeconds;
        if (gesture || Dirty() || (!force && !refresh && elapsed < .75f)) { return; }
        elapsed = 0; refresh = false;
        std::vector<SceneItem> objects;
        if (!QueryObjects(ctx, kRegionComponentType, objects, error)) { return; }
        std::vector<RoomItem> fresh;
        for (const auto& object : objects) {
            RoomItem item; item.object = object;
            if (!ReadRegion(ctx, object.objectId, item.target, item.settings, error)) { return; }
            fresh.push_back(std::move(item));
        }
        rooms = std::move(fresh);
        ++revision;
        if (!Current()) { selected = rooms.empty() ? 0 : rooms.front().object.objectId; }
        ready = Current() != nullptr;
        if (ready) { draft = Current()->settings; devices.Load(draft.devices); }
        // 他のパネルが作るカメラ参照やプレビューの結びも保存状態に含める。
        const auto state = Execute(ctx, "editor.state", Json::object());
        if (state.ok) {
            const auto scene = state.data.find("scene");
            if (scene != state.data.end() && scene->is_object()) {
                const auto dirty = scene->find("dirty");
                if (dirty != scene->end() && dirty->is_boolean()) { sceneDirty = dirty->get<bool>(); }
            }
        }
    }
    bool Select(EditorWorkspaceHostContext& ctx, uint64_t objectId) {
        if (objectId == selected && ready) { return true; }
        if (Dirty() && !Commit(ctx)) { return false; }
        selected = objectId; ready = Current() != nullptr;
        if (ready) { draft = Current()->settings; devices.Select(-1); devices.Load(draft.devices); screenWarning.clear(); }
        return ready;
    }
    std::vector<RegionPlacement> Layout(const RegionSettings* replacement = nullptr) const {
        std::vector<RegionPlacement> result;
        for (const auto& r : rooms) { result.push_back(Placement(replacement && r.object.objectId == selected ? *replacement : r.settings)); }
        return result;
    }
    std::string Validate(const RegionSettings& s) const {
        if (!std::isfinite(s.screenBrightness) || s.screenBrightness < .5f || s.screenBrightness > 4.0f) { return "屏幕亮度范围为 0.5～4"; }
        const auto region = ValidateRegion(s.region);
        if (!region.valid) { return std::string(region.message); }
        if (s.region.cols > 64 || s.region.rows > 64 || s.region.cells.size() > 4096) { return "电视地图最多 64 × 64 格"; }
        DeviceEditor d; d.Load(s.devices);
        if (const auto problem = d.Validate(s.region.cols, s.region.rows); !problem.empty()) { return problem; }
        if (!std::isfinite(s.spawnX) || !std::isfinite(s.spawnY) || s.spawnX < 0 || s.spawnY < 0 ||
            s.spawnX + .625f > static_cast<float>(s.region.cols) || s.spawnY + .875f > static_cast<float>(s.region.rows)) { return "出生位置超出电视地图"; }
        const auto layout = Layout(&s);
        const auto valid = ValidateWorldLayout(layout);
        return valid.valid ? std::string{} : valid.message + "：" + valid.regionA + " / " + valid.regionB;
    }
    bool Commit(EditorWorkspaceHostContext& ctx) {
        auto* item = Current(); if (!ready || !item || !Dirty()) { return true; }
        // 保存やワークスペース切り替えの時も、画面の index は実際のモデルから取り直す。
        if (draft.screenObject != item->settings.screenObject || draft.screenSlot != item->settings.screenSlot) { ResolveScreenMaterial(ctx, draft, screenWarning); }
        if (const auto problem = Validate(draft); !problem.empty()) { error = problem; return false; }
        RegionTarget target; RegionSettings fresh;
        if (!ReadRegion(ctx, selected, target, fresh, error)) { return false; }
        if (target != item->target || fresh != item->settings) { error = "地图已被其他编辑更新；请撤回草稿并刷新"; return false; }
        Json changes = Json::array(); AppendRegionChanges(changes, target, fresh, draft);
        std::vector<RoomItem*> otherEntries;
        // 開始テレビの変更も同じ取り消しに含め、開始区域を二つ残さない。
        if (draft.entryRegion) for (auto& other : rooms) {
            if (other.object.objectId == selected || !other.settings.entryRegion) { continue; }
            RegionTarget otherTarget; RegionSettings otherFresh;
            if (!ReadRegion(ctx, other.object.objectId, otherTarget, otherFresh, error)) { return false; }
            if (otherTarget != other.target || otherFresh != other.settings) { error = "开始电视已被其他编辑更新"; return false; }
            changes.push_back({{"objectId", otherTarget.objectId}, {"componentId", otherTarget.componentId}, {"propertyId", "entryRegion"}, {"value", false}});
            otherEntries.push_back(&other);
        }
        const auto written = Execute(ctx, "authoring.transaction", {{"commands", changes}});
        if (!written.ok) { error = written.error; return false; }
        sceneDirty = true;
        for (auto* other : otherEntries) {
            RegionTarget observedTarget; RegionSettings observed;
            if (!ReadRegion(ctx, other->object.objectId, observedTarget, observed, error)) { return false; }
            if (observedTarget != other->target || observed.entryRegion) { error = "开始电视写入回读不一致"; return false; }
            other->settings = observed;
        }
        RegionSettings confirmed; RegionTarget confirmedTarget;
        if (!ReadRegion(ctx, selected, confirmedTarget, confirmed, error)) { return false; }
        item->settings = confirmed; item->target = confirmedTarget;
        if (confirmedTarget != target || confirmed != draft) { error = "地图写入回读不一致；草稿保留"; return false; }
        draft = confirmed; devices.Load(draft.devices); error.clear(); refresh = true;
        return true;
    }
    // 一筆・一回のドラッグを一つの authoring.transaction にまとめる。
    bool Finish(EditorWorkspaceHostContext& ctx, bool rejectInvalid = false) {
        gesture = false;
        if (Commit(ctx)) { return true; }
        if (rejectInvalid) { const auto message = error; Discard(); error = message; }
        return false;
    }
    void History(EditorWorkspaceHostContext& ctx, bool redo) {
        if (gesture) { return; }
        if (Dirty()) { error = "先应用或撤回当前草稿"; return; }
        const auto result = Execute(ctx, redo ? "scene.redo" : "scene.undo", Json::object());
        if (!result.ok) { error = result.error; return; }
        sceneDirty = true;
        refresh = true; Poll(ctx, true);
    }
    void Create(EditorWorkspaceHostContext& ctx) {
        if (Dirty() && !Commit(ctx)) { return; }
        const auto created = Execute(ctx, "scene.create_empty_object", {{"name", "电视地图"}});
        if (!created.ok) { error = created.error; return; }
        sceneDirty = true;
        const auto id = ObjectId(created.data.value("objectId", Json{}));
        if (!id || !AddRegion(ctx, id, false, error)) { return; }
        refresh = true; Poll(ctx, true); Select(ctx, id);
    }
    void AddSelectedTelevision(EditorWorkspaceHostContext& ctx) {
        if (Dirty() && !Commit(ctx)) { return; }
        const auto id = SelectedObject(ctx, error);
        if (!id || !AddRegion(ctx, id, true, error)) { return; }
        sceneDirty = true;
        refresh = true; Poll(ctx, true);
        if (!Select(ctx, id)) { return; }
        ResolveScreenMaterial(ctx, draft, screenWarning);
        if (Commit(ctx)) { error.clear(); }
    }
    void Publish(EditorWorkspaceHostContext& ctx) {
        std::string message = !error.empty() ? error : screenWarning;
        if (message.empty() && !rooms.empty()) {
            const auto entries = std::count_if(rooms.begin(), rooms.end(), [](const auto& r) { return r.settings.worldPlaced && r.settings.entryRegion; });
            if (entries == 0) { message = "请在世界地图放置开始电视"; }
            else if (entries > 1) { message = "世界地图需要唯一的开始电视"; }
        }
        if (message == published) { return; }
        ctx.commands->PublishStatus(message.empty() ? EditorWorkspaceStatusSeverity::Info : EditorWorkspaceStatusSeverity::Warning, message);
        published = message;
    }
};

class MapWorkspace : public IEditorWorkspace {
public:
    explicit MapWorkspace(std::shared_ptr<MapModel> model) : model_(std::move(model)) {}
    std::string_view GetDocumentIdentity() const override { return "td2.scene.maps"; }
    void OnActivate(EditorWorkspaceHostContext& ctx) override {
        model_->Poll(ctx, true);
        if (!centerCreated_) { centerWarmupFrames_ = 2; }
    }
    void OnDeactivate(EditorWorkspaceHostContext& ctx) override { if (model_->gesture) { model_->Finish(ctx, true); } }
    bool IsDirty() const override { return model_->Dirty(); }
    EditorWorkspaceSaveResult Save(EditorWorkspaceHostContext& ctx) override {
        if (!ApplyDraft(ctx)) { return EditorWorkspaceSaveResult::Failed; }
        const auto saved = Execute(ctx, "scene.save", Json::object());
        if (!saved.ok) { model_->error = saved.error; return EditorWorkspaceSaveResult::Failed; }
        model_->sceneDirty = false; model_->savedOnce = true; model_->error.clear();
        return EditorWorkspaceSaveResult::Saved;
    }
    void DiscardChanges(EditorWorkspaceHostContext&) override { model_->Discard(); }
    EditorWorkspaceCloseDisposition QueryClose(EditorWorkspaceCloseReason) const override { return IsDirty() ? EditorWorkspaceCloseDisposition::RequiresSave : EditorWorkspaceCloseDisposition::Allow; }
protected:
    // ワークスペースの切り替えはメモリへ適用するだけ。ディスクへの保存とは別。
    virtual bool ApplyDraft(EditorWorkspaceHostContext& ctx) { return model_->Commit(ctx); }
    bool MapPanelReady() {
        // 初回だけ標準のビューポートを先に作る。以後は作者のタブ選択を変えない。
        if (centerWarmupFrames_ > 0) { --centerWarmupFrames_; return false; }
        centerCreated_ = true; return true;
    }
    void SaveControls(EditorWorkspaceHostContext& ctx, std::string_view controlId = "td2.map.save") {
        auto& ui = *ctx.ui;
        // 作者が保存した時だけ Scene をディスクへ書く。ブラシやドラッグでは書かない。
        if (ui.Button(controlId, "保存场景")) { Save(ctx); }
        ui.SameLine(); ui.Text(IsDirty() ? "草稿未应用" : model_->sceneDirty ? "已应用 · 未保存" : model_->savedOnce ? "已保存" : "已载入");
    }
    void HistoryToolbar(EditorWorkspaceHostContext& ctx, std::string_view otherId, std::string_view otherName) {
        auto& ui = *ctx.ui;
        if (ui.Button("td2.map.switch", otherName)) { if (ApplyDraft(ctx)) { ctx.commands->RequestActivateWorkspace(otherId); } }
        ui.SameLine();
        // 取り消し・やり直しの前に草稿を適用する。
        if (ui.Button("td2.map.undo", "撤销")) { if (!IsDirty() || ApplyDraft(ctx)) { model_->History(ctx, false); } }
        ui.SameLine();
        if (ui.Button("td2.map.redo", "重做")) { if (!IsDirty() || ApplyDraft(ctx)) { model_->History(ctx, true); } }
        ui.SameLine();
        if (ui.Button("td2.map.fit", "适应")) { view_.fit = true; }
        ui.SameLine();
        if (ui.Button("td2.map.pan", hand_ ? "平移 ●" : "平移")) { hand_ = !hand_; }
        ui.SameLine();
        if (ui.Button("td2.map.refresh", "刷新")) { model_->Poll(ctx, true); }
        if (model_->Dirty() && !model_->gesture) {
            ui.SameLine(); if (ui.Button("td2.map.apply", "应用")) { model_->Commit(ctx); }
            ui.SameLine(); if (ui.Button("td2.map.discard", "撤回草稿")) { model_->Discard(); }
        }
        const auto interaction = ui.GetInteraction();
        if (!interaction.editing && interaction.undoPressed && (!IsDirty() || ApplyDraft(ctx))) { model_->History(ctx, false); }
        if (!interaction.editing && interaction.redoPressed && (!IsDirty() || ApplyDraft(ctx))) { model_->History(ctx, true); }
    }
    void DrawThumbnail(IEditorWorkspaceUi& ui, const RoomItem& room, const EditorWorkspaceCanvasInput& canvas) {
        const auto& settings = room.object.objectId == model_->selected && model_->ready ? model_->draft : room.settings;
        MapCanvasView thumbnail;
        thumbnail.tile = (std::min)((canvas.size.x - 12) / static_cast<float>(settings.region.cols), (canvas.size.y - 12) / static_cast<float>(settings.region.rows));
        thumbnail.tile = (std::max)(.5f, thumbnail.tile);
        thumbnail.origin = {(canvas.size.x - static_cast<float>(settings.region.cols) * thumbnail.tile) * .5f, (canvas.size.y - static_cast<float>(settings.region.rows) * thumbnail.tile) * .5f};
        DrawRoomMap(ui, settings, thumbnail, false, false);
        ui.CanvasRect({1, 1}, {canvas.size.x - 1, canvas.size.y - 1}, room.object.objectId == model_->selected ? EditorWorkspaceColor{.77f, .68f, .39f, 1} : EditorWorkspaceColor{.24f, .38f, .43f, 1});
    }
    std::shared_ptr<MapModel> model_;
    MapCanvasView view_;
    bool hand_ = false;
    bool centerCreated_ = false;
    int centerWarmupFrames_ = 0;
};

// テレビ単位の作者編集。実行時の状態は持たない。
class TelevisionMapWorkspace final : public MapWorkspace {
public:
    using MapWorkspace::MapWorkspace;
    EditorWorkspaceDescriptor GetDescriptor() const override {
        EditorWorkspaceDescriptor d; d.workspaceId = kTelevisionWorkspace; d.displayName = "电视地图";
        d.options = static_cast<uint32_t>(EditorWorkspaceOption::RequestSceneViewport); return d;
    }
    void OnActivate(EditorWorkspaceHostContext& ctx) override { MapWorkspace::OnActivate(ctx); drag_ = Drag::None; ChooseTool(0); }
    void OnDeactivate(EditorWorkspaceHostContext& ctx) override { MapWorkspace::OnDeactivate(ctx); drag_ = Drag::None; linkSource_.clear(); }
    void Draw(EditorWorkspaceHostContext& ctx) override {
        if (!ctx.ui || !ctx.commands) { return; }
        model_->Poll(ctx);
        auto& ui = *ctx.ui;
        DrawList(ctx);
        if (MapPanelReady()) {
            if (ui.BeginPanel("td2.television.canvas.panel.v5", "电视地图", nullptr, EditorWorkspaceDockRegion::Center)) {
                // ツールバーのドックは既定のレイアウトで隠れるので、操作はメインキャンバスの上に置く。
                HistoryToolbar(ctx, kWorldWorkspace, "世界地图");
                ui.SameLine(); SaveControls(ctx);
                ui.Separator();
                if (tool_ == 3) { ui.Text("放置：" + DeviceLabel(kind_) + " · 点空格放置，按住 Shift 可叠放"); }
                else if (tool_ == 4) { ui.Text("连线：点压板，再点门或激光"); }
                DrawCanvas(ctx);
            } ui.EndPanel();
        }
        if (ui.BeginPanel("td2.television.properties", "选中属性", nullptr, EditorWorkspaceDockRegion::Right)) { DrawProperties(ctx); } ui.EndPanel();
        model_->Publish(ctx);
    }
private:
    enum class Drag { None, Paint, Move, Resize, Spawn, Box };
    void ChooseTool(int tool) { tool_ = tool; hand_ = false; linkSource_.clear(); groupIds_.clear(); }
    void ChooseKind(size_t index) { kind_ = kEditorDeviceKinds[index]; ChooseTool(3); model_->devices.Select(-1); }
    void DrawList(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        if (ui.BeginPanel("td2.television.library", "工具与机关", nullptr, EditorWorkspaceDockRegion::Left)) {
            SaveControls(ctx, "td2.map.save.backup"); ui.Separator();
            if (const auto* room = model_->Current()) { ui.Text(ShortChoiceLabel(room->object.name)); }
            if (ui.Button("td2.television.gallery", gallery_ ? "收起电视列表" : "切换电视")) { gallery_ = !gallery_; }
            if (gallery_ || !model_->ready) {
                if (ui.Button("td2.television.add_selected", "选中电视建图")) { model_->AddSelectedTelevision(ctx); view_.fit = true; ChooseTool(0); }
                if (ui.Button("td2.television.new", "+ 空白地图")) { model_->Create(ctx); view_.fit = true; ChooseTool(0); }
                EditText(ui, "td2.television.gallery.search", "查找", gallerySearch_);
                const auto rooms = model_->rooms;
                for (size_t i = 0; i < rooms.size(); ++i) {
                    const auto& room = rooms[i];
                    if (!gallerySearch_.empty() && room.object.name.find(gallerySearch_) == std::string::npos &&
                        room.settings.region.regionId.find(gallerySearch_) == std::string::npos) { continue; }
                    ui.Text(ShortChoiceLabel(room.object.name) + " " + std::to_string(room.settings.region.cols) + "×" + std::to_string(room.settings.region.rows));
                    const auto input = ui.BeginCanvas("td2.television.thumbnail." + std::to_string(room.object.objectId), 72);
                    DrawThumbnail(ui, room, input);
                    if (input.hovered && (input.pressedButtons & kPrimary)) {
                        if (model_->Select(ctx, room.object.objectId)) { view_.fit = true; drag_ = Drag::None; gallery_ = false; ChooseTool(0); }
                    }
                    ui.EndCanvas();
                }
            }
            ui.Separator(); ui.Text("工具 / 地形");
            constexpr std::array<std::string_view, 8> tools{"↖ 选择", "▧ 地形", "⌫ 擦除", "+ 放机关", "⌁ 连线", "◉ 出生点", "□ 框选", "▣ 粘贴"};
            const bool toolsTable = ui.BeginTable("td2.television.tools", 4);
            for (size_t i = 0; i < tools.size(); ++i) {
                if (toolsTable) { if (i % 4 == 0) { ui.TableNextRow(); } ui.TableNextColumn(); }
                if (ui.Button("td2.television.tool." + std::to_string(i), std::string(tools[i]) + (tool_ == static_cast<int>(i) ? " ●" : ""))) { ChooseTool(static_cast<int>(i)); }
            }
            if (toolsTable) { ui.EndTable(); }
            if (tool_ == 6) {
                ui.Text("框选机关：" + std::to_string(groupIds_.size()));
                if (ui.Button("td2.television.group.copy", "复制框选")) {
                    clipboard_.clear(); for (const auto& d : model_->devices.Items()) { if (std::find(groupIds_.begin(), groupIds_.end(), d.id) != groupIds_.end()) { clipboard_.push_back(d); } }
                    if (clipboard_.empty()) { model_->error = "先拖框选中机关"; }
                    else { ChooseTool(7); model_->error.clear(); }
                }
            }
            if (tool_ == 7) { ui.Text(clipboard_.empty() ? "先框选并复制机关" : "点击地图粘贴 " + std::to_string(clipboard_.size()) + " 个机关"); }
            if (tool_ == 1 || tool_ == 2) {
                EditInt(ui, "td2.television.brush.size", "笔刷", brushSize_, 1, 4);
                ui.Checkbox("td2.television.brush.rectangle", "矩形填充", &rectanglePaint_);
            }
            ui.Separator(); ui.Text("机关类型");
            const bool kindsTable = ui.BeginTable("td2.television.kinds", 2);
            for (size_t i = 0; i < kEditorDeviceKinds.size(); ++i) {
                if (kindsTable) { if (i % 2 == 0) { ui.TableNextRow(); } ui.TableNextColumn(); }
                if (ui.Button("td2.television.kind." + std::to_string(i), std::string(kEditorDeviceLabels[i]) + (tool_ == 3 && kind_ == kEditorDeviceKinds[i] ? " ●" : ""))) { ChooseKind(i); }
            }
            if (kindsTable) { ui.EndTable(); }
            ui.Separator(); ui.Text("现有机关");
            EditText(ui, "td2.television.devices.search", "查找", deviceSearch_);
            std::vector<EditorChoice> choices; std::vector<int> indices;
            for (size_t i = 0; i < model_->devices.Items().size(); ++i) {
                const auto& d = model_->devices.Items()[i];
                const std::string name = DeviceLabel(d.kind) + " " + std::to_string(i + 1);
                if (!deviceSearch_.empty() && name.find(deviceSearch_) == std::string::npos && d.id.find(deviceSearch_) == std::string::npos) { continue; }
                choices.push_back({"td2.television.device.select." + d.id, name, model_->devices.SelectedIndex() == static_cast<int>(i)}); indices.push_back(static_cast<int>(i));
            }
            if (choices.empty()) { ui.Text(model_->devices.Items().empty() ? "点击类型，再点地图放置" : "没有匹配的机关"); }
            else if (const int selected = DrawChoices(ui, choices); selected >= 0) { model_->devices.Select(indices[static_cast<size_t>(selected)]); ChooseTool(0); }
        } ui.EndPanel();
    }
    void PaintLine(EditorWorkspacePoint a, EditorWorkspacePoint b, bool erase) {
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
    void PaintCell(int x, int y, bool erase) {
        auto& region = model_->draft.region;
        if (x < 0 || y < 0 || x >= region.cols || y >= region.rows) { return; }
        const auto index = CellIndex(region, x, y);
        if (erase) {
            if (erasedDeviceCells_.contains(index)) { return; }
            const int hit = model_->devices.Hit(x + .5, y + .5);
            if (hit >= 0) {
                model_->devices.Select(hit); model_->devices.RemoveSelected(); model_->draft.devices = model_->devices.Serialize();
                erasedDeviceCells_.insert(index); return;
            }
        }
        region.cells[index] = erase ? '0' : '1';
    }
    int HandleAt(EditorWorkspacePoint screen) const {
        const auto* d = model_->devices.Selected(); if (!d) { return -1; }
        const auto a = view_.Screen(static_cast<float>(d->x), static_cast<float>(d->y));
        const auto b = view_.Screen(static_cast<float>(d->x + d->w), static_cast<float>(d->y + d->h));
        const auto points = Handles(a, b);
        // 縮小した薄い装置ではハンドルが近いので、一番近いものを選んで向きを安定させる。
        int hit = -1; float closest = 81;
        for (size_t i = 0; i < points.size(); ++i) {
            const float dx = screen.x - points[i].x, dy = screen.y - points[i].y, distance = dx * dx + dy * dy;
            if (distance <= closest) { closest = distance; hit = static_cast<int>(i); }
        }
        return hit;
    }
    static std::array<EditorWorkspacePoint, 8> Handles(EditorWorkspacePoint a, EditorWorkspacePoint b) {
        const float x = (a.x + b.x) * .5f, y = (a.y + b.y) * .5f;
        return {a, EditorWorkspacePoint{x, a.y}, EditorWorkspacePoint{b.x, a.y}, EditorWorkspacePoint{b.x, y}, b,
                EditorWorkspacePoint{x, b.y}, EditorWorkspacePoint{a.x, b.y}, EditorWorkspacePoint{a.x, y}};
    }
    void DrawSelection(IEditorWorkspaceUi& ui) {
        const auto* d = model_->devices.Selected(); if (!d) { return; }
        const auto a = view_.Screen(static_cast<float>(d->x), static_cast<float>(d->y));
        const auto b = view_.Screen(static_cast<float>(d->x + d->w), static_cast<float>(d->y + d->h));
        ui.CanvasRect(a, b, {.97f, .80f, .40f, 1}, 2);
        for (const auto& p : Handles(a, b)) {
            ui.CanvasFill({p.x - 4, p.y - 4}, {p.x + 4, p.y + 4}, {.97f, .80f, .40f, 1});
        }
    }
    void DrawCanvas(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        const auto input = ui.BeginCanvas("td2.television.canvas", 640);
        view_.ObserveSize(input.size);
        if (model_->ready && input.size.x > 0 && input.size.y > 0) {
            auto& s = model_->draft;
            if (view_.fit && (!model_->gesture || !view_.hasFit)) { view_.Fit(input.size, 0, 0, static_cast<float>(s.region.cols), static_cast<float>(s.region.rows)); }
            view_.Navigate(input, hand_, drag_ != Drag::None);
            DrawCanvasGrid(ui, view_, input.size);
            const auto valid = ValidateRegion(s.region);
            if (valid.valid) {
                const auto p = view_.World(input.pointer);
                const bool inside = p.x >= 0 && p.y >= 0 && p.x < static_cast<float>(s.region.cols) && p.y < static_cast<float>(s.region.rows);
                if (!hand_ && !view_.panning && input.hovered && (input.pressedButtons & (kPrimary | kSecondary)) && (inside || (tool_ == 0 && HandleAt(input.pointer) >= 0))) {
                    if (model_->Dirty() && !model_->Commit(ctx)) { ui.EndCanvas(); return; }
                    start_ = last_ = p; erase_ = tool_ == 2 || (input.pressedButtons & kSecondary);
                    erasedDeviceCells_.clear();
                    if (erase_) {
                        const int hit = model_->devices.Hit(p.x, p.y);
                        if (hit >= 0) {
                            model_->devices.Select(hit); model_->devices.RemoveSelected(); s.devices = model_->devices.Serialize();
                            erasedDeviceCells_.insert(CellIndex(s.region, static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y))));
                        }
                    }
                    if (tool_ == 6 && !erase_) { groupIds_.clear(); model_->devices.Select(-1); drag_ = Drag::Box; }
                    else if (tool_ == 7 && !erase_) {
                        if (model_->devices.Paste(clipboard_, std::floor(p.x), std::floor(p.y), s.region.cols, s.region.rows, groupIds_)) { s.devices = model_->devices.Serialize(); model_->Commit(ctx); }
                        else { model_->error = clipboard_.empty() ? "先框选并复制机关" : "复制的机关组超出当前电视"; }
                    }
                    else if (tool_ == 0 && !erase_) {
                        groupIds_.clear();
                        handle_ = HandleAt(input.pointer);
                        if (handle_ < 0) { model_->devices.Select(model_->devices.Hit(p.x, p.y)); }
                        if (const auto* d = model_->devices.Selected()) { original_ = *d; drag_ = handle_ < 0 ? Drag::Move : Drag::Resize; }
                    } else if (tool_ == 4 && !erase_) {
                        const int hit = model_->devices.Hit(p.x, p.y);
                        if (hit >= 0) {
                            model_->devices.Select(hit);
                            const auto* d = model_->devices.Selected();
                            if (d->kind == "Plate") { linkSource_ = d->id; }
                            else if (model_->devices.ToggleLink(linkSource_, hit)) {
                                s.devices = model_->devices.Serialize(); model_->Commit(ctx);
                                for (size_t i = 0; i < model_->devices.Items().size(); ++i) { if (model_->devices.Items()[i].id == linkSource_) { model_->devices.Select(static_cast<int>(i)); break; } }
                            }
                        }
                    } else if (tool_ == 3 && !erase_) {
                        const int hit = model_->devices.Hit(p.x, p.y);
                        if (hit >= 0 && !input.shift) { model_->devices.Select(hit); ChooseTool(0); }
                        else if (model_->devices.Add(kind_, static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), s.region.cols, s.region.rows)) { s.devices = model_->devices.Serialize(); model_->Commit(ctx); }
                    } else if (tool_ == 5 && !erase_) { drag_ = Drag::Spawn; }
                    else { drag_ = Drag::Paint; }
                    model_->gesture = drag_ != Drag::None;
                }
                if (drag_ != Drag::None) {
                    const auto clamped = EditorWorkspacePoint{std::clamp(p.x, 0.0f, static_cast<float>(s.region.cols) - .001f), std::clamp(p.y, 0.0f, static_cast<float>(s.region.rows) - .001f)};
                    if (drag_ == Drag::Paint) {
                        if (!rectanglePaint_) { PaintLine(last_, clamped, erase_); }
                        last_ = clamped;
                    }
                    if (drag_ == Drag::Box) { last_ = clamped; }
                    if (drag_ == Drag::Spawn) {
                        s.spawnX = (std::min)(std::floor(clamped.x) + .1875f, static_cast<float>(s.region.cols) - .625f);
                        s.spawnY = (std::min)(std::floor(clamped.y) + .125f, static_cast<float>(s.region.rows) - .875f);
                    }
                    if (auto* d = model_->devices.Selected(); d && (drag_ == Drag::Move || drag_ == Drag::Resize)) {
                        const double dx = std::round(p.x - start_.x), dy = std::round(p.y - start_.y);
                        *d = original_;
                        if (drag_ == Drag::Move) {
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
                        s.devices = model_->devices.Serialize();
                    }
                    if ((input.releasedButtons & (kPrimary | kSecondary)) || (!(input.heldButtons & (kPrimary | kSecondary)) && !(input.pressedButtons & (kPrimary | kSecondary)))) {
                        if (drag_ == Drag::Paint && rectanglePaint_) {
                            for (int y = static_cast<int>(std::floor((std::min)(start_.y, last_.y))); y <= static_cast<int>(std::floor((std::max)(start_.y, last_.y))); ++y)
                                for (int x = static_cast<int>(std::floor((std::min)(start_.x, last_.x))); x <= static_cast<int>(std::floor((std::max)(start_.x, last_.x))); ++x) { PaintCell(x, y, erase_); }
                        }
                        if (drag_ == Drag::Box) {
                            const double left = (std::min)(start_.x, last_.x), right = (std::max)(start_.x, last_.x);
                            const double top = (std::min)(start_.y, last_.y), bottom = (std::max)(start_.y, last_.y);
                            for (const auto& d : model_->devices.Items()) { if (d.x < right && d.x + d.w > left && d.y < bottom && d.y + d.h > top) { groupIds_.push_back(d.id); } }
                        }
                        drag_ = Drag::None; model_->Finish(ctx);
                    }
                }
                DrawRoomMap(ui, s, view_, true, true); DrawSelection(ui);
                if ((drag_ == Drag::Paint && rectanglePaint_) || drag_ == Drag::Box) {
                    const float x0 = (std::min)(start_.x, last_.x), y0 = (std::min)(start_.y, last_.y), x1 = (std::max)(start_.x, last_.x), y1 = (std::max)(start_.y, last_.y);
                    ui.CanvasRect(view_.Screen(drag_ == Drag::Box ? x0 : std::floor(x0), drag_ == Drag::Box ? y0 : std::floor(y0)),
                                  view_.Screen(drag_ == Drag::Box ? x1 : std::floor(x1) + 1, drag_ == Drag::Box ? y1 : std::floor(y1) + 1), {.95f, .78f, .4f, 1}, 2);
                }
                for (const auto& d : model_->devices.Items()) { if (std::find(groupIds_.begin(), groupIds_.end(), d.id) != groupIds_.end()) { ui.CanvasRect(view_.Screen(static_cast<float>(d.x), static_cast<float>(d.y)), view_.Screen(static_cast<float>(d.x + d.w), static_cast<float>(d.y + d.h)), {.95f, .78f, .4f, 1}, 2); } }
                ui.CanvasRect(view_.Screen(0, 0), view_.Screen(static_cast<float>(s.region.cols), static_cast<float>(s.region.rows)), {.45f, .66f, .69f, 1}, 2);
                if (!linkSource_.empty() && tool_ == 4) {
                    const auto source = std::find_if(model_->devices.Items().begin(), model_->devices.Items().end(), [this](const auto& d) { return d.id == linkSource_; });
                    if (source != model_->devices.Items().end()) { ui.CanvasLine(view_.Screen(static_cast<float>(source->x + source->w * .5), static_cast<float>(source->y + source->h * .5)), input.pointer, {.93f, .76f, .36f, .75f}, 2); }
                }
            } else { ui.CanvasText({20, 20}, {.85f, .4f, .4f, 1}, valid.message); }
        }
        ui.EndCanvas();
        if (model_->ready && !model_->gesture && !ui.GetInteraction().editing && ui.GetInteraction().deletePressed) {
            if (model_->devices.RemoveSelected()) { model_->draft.devices = model_->devices.Serialize(); model_->Commit(ctx); }
        }
    }
    void DrawProperties(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui; if (!model_->ready) { return; }
        if (!model_->error.empty()) { ui.Text(model_->error); }
        auto& s = model_->draft;
        const auto before = s;
        if (tool_ == 3 && !model_->devices.HasSelection()) { ui.Text("放置 " + DeviceLabel(kind_)); ui.Text("点空格放置；点现有机关选中"); }
        if (auto* d = model_->devices.Selected(); d && tool_ != 5) {
            ui.Text(DeviceLabel(d->kind) + " " + std::to_string(model_->devices.SelectedIndex() + 1));
            if (ui.Button("td2.television.device.deselect", "返回地图属性")) { model_->devices.Select(-1); return; }
            EditFloat(ui, "td2.television.device.x", "X / 格", d->x, 0, static_cast<float>((std::max)(0.0, s.region.cols - d->w)));
            EditFloat(ui, "td2.television.device.y", "Y / 格", d->y, 0, static_cast<float>((std::max)(0.0, s.region.rows - d->h)));
            EditFloat(ui, "td2.television.device.w", "宽 / 格", d->w, .125f, static_cast<float>(s.region.cols - d->x));
            EditFloat(ui, "td2.television.device.h", "高 / 格", d->h, .125f, static_cast<float>(s.region.rows - d->y));
            if (d->kind == "Laser" || d->kind == "Spikes" || d->kind == "OneWay") {
                constexpr std::array<std::string_view, 4> directions{"Up", "Right", "Down", "Left"};
                constexpr std::array<std::string_view, 4> names{"↑", "→", "↓", "←"};
                for (size_t i = 0; i < directions.size(); ++i) { if (i) { ui.SameLine(); } if (ui.Button("td2.television.device.direction." + std::to_string(i), d->direction == directions[i] ? std::string(names[i]) + " ●" : std::string(names[i]))) { d->direction = directions[i]; } }
            }
            if (d->kind == "Laser") { EditFloat(ui, "td2.television.device.range", "射程", d->range, .125f, 128); }
            if (d->kind == "Laser" || d->kind == "Door" || d->kind == "Bridge") { ui.Checkbox("td2.television.device.invert", "反向激活", &d->inverted); }
            if (d->kind == "Plate") { ui.Checkbox("td2.television.device.latch", "踩一下切换（关：踩住时才通电）", &d->latch); }
            if (d->kind == "Plate") {
                ui.Separator(); ui.Text("连接目标（" + std::to_string(d->targets.size()) + "）");
                if (ui.Button("td2.television.device.link", "在地图上连线")) { tool_ = 4; linkSource_ = d->id; hand_ = false; }
                std::vector<int> targets;
                for (size_t i = 0; i < model_->devices.Items().size(); ++i) { const auto& target = model_->devices.Items()[i]; if (target.kind == "Door" || target.kind == "Laser" || target.kind == "Bridge") { targets.push_back(static_cast<int>(i)); } }
                if (targets.empty()) { ui.Text("先放置门、激光或桥"); }
                for (size_t i = 0; i < targets.size(); ++i) {
                    const int index = targets[i]; const auto& target = model_->devices.Items()[static_cast<size_t>(index)];
                    bool linked = std::find(d->targets.begin(), d->targets.end(), target.id) != d->targets.end();
                    if (ui.Checkbox("td2.television.device.target." + target.id, DeviceLabel(target.kind) + " " + std::to_string(index + 1), &linked)) { model_->devices.ToggleLink(d->id, index); }
                }
            }
            if (ui.Button("td2.television.device.duplicate", "复制")) { model_->devices.DuplicateSelected(s.region.cols, s.region.rows); }
            ui.SameLine(); if (ui.Button("td2.television.device.delete", "删除")) { model_->devices.RemoveSelected(); }
            s.devices = model_->devices.Serialize();
        } else {
            if (const auto* item = model_->Current()) { ui.Text(item->object.name); }
            const auto previous = s.region;
            EditInt(ui, "td2.television.cols", "列", s.region.cols, 1, 64);
            EditInt(ui, "td2.television.rows", "行", s.region.rows, 1, 64);
            if (!ResizeCells(previous, s.region)) { s.region = previous; model_->error = "尺寸变化失败"; }
            if (previous.cols != s.region.cols || previous.rows != s.region.rows) {
                view_.fit = true;
                const float scale = (std::min)(1.0f, 2048.0f / static_cast<float>((std::max)(s.region.cols, s.region.rows) * s.region.tilePixels));
                s.outputWidth = (std::max)(32, static_cast<int>(std::round(static_cast<float>(s.region.cols * s.region.tilePixels) * scale)));
                s.outputHeight = (std::max)(32, static_cast<int>(std::round(static_cast<float>(s.region.rows * s.region.tilePixels) * scale)));
            }
            ui.Checkbox("td2.television.entry", "开始电视", &s.entryRegion);
            if (tool_ == 5) {
                double x = s.spawnX, y = s.spawnY;
                if (EditFloat(ui, "td2.television.spawn.x", "X", x, 0, static_cast<float>(s.region.cols) - .625f)) { s.spawnX = static_cast<float>(x); }
                if (EditFloat(ui, "td2.television.spawn.y", "Y", y, 0, static_cast<float>(s.region.rows) - .875f)) { s.spawnY = static_cast<float>(y); }
            }
            if (ui.Checkbox("td2.television.screen.advanced", "屏幕绑定", &screenSettings_) && screenSettings_) { RefreshScreens(ctx); }
            if (screenSettings_) {
                const auto current = std::find_if(screens_.begin(), screens_.end(), [&s](const auto& item) { return item.objectId == s.screenObject; });
                ui.Text("当前：" + (s.screenObject == 0 ? std::string("未绑定") : current == screens_.end() ? "对象 " + std::to_string(s.screenObject) : ShortChoiceLabel(current->name)));
                double brightness = s.screenBrightness;
                if (EditFloat(ui, "td2.television.screen.brightness", "屏幕亮度", brightness, .5f, 4.0f)) { s.screenBrightness = static_cast<float>(brightness); }
                if (ui.Button("td2.television.screen.style", "屏幕风格（原生属性）")) {
                    // 標準の Inspector への入口だけを置く。共有の設定や演出カメラは複製しない。
                    std::vector<SceneItem> styles;
                    if (QueryObjects(ctx, "TD2.ScreenStyle", styles, model_->error)) {
                        const auto own = std::find_if(styles.begin(), styles.end(), [&](const auto& item) { return item.objectId == model_->selected || item.objectId == s.screenObject; });
                        const uint64_t styleObject = own != styles.end() ? own->objectId : styles.size() == 1 ? styles.front().objectId : 0;
                        if (styleObject) {
                            const auto selected = Execute(ctx, "scene.object.select", {{"objectId", styleObject}});
                            model_->error = selected.ok ? std::string{} : selected.error;
                        } else { model_->error = styles.empty() ? "尚未配置屏幕风格组件" : "有多个共享屏幕风格，请在场景中选择要调整的组件"; }
                    }
                }
                if (ui.Button("td2.television.screen.pick", screenPicker_ ? "收起屏幕列表" : "选择屏幕对象")) { screenPicker_ = !screenPicker_; }
                if (screenPicker_) {
                    EditText(ui, "td2.television.screen.search", "查找", screenSearch_);
                    if (ui.Checkbox("td2.television.screen.all", "包括无模型对象", &allScreens_)) { RefreshScreens(ctx); }
                    std::vector<EditorChoice> choices{{"td2.television.screen.none", "未绑定", s.screenObject == 0}};
                    std::vector<uint64_t> ids{0};
                    for (const auto& screen : screens_) {
                        if (!screenSearch_.empty() && screen.name.find(screenSearch_) == std::string::npos) { continue; }
                        choices.push_back({"td2.television.screen." + std::to_string(screen.objectId), screen.name, s.screenObject == screen.objectId}); ids.push_back(screen.objectId);
                    }
                    if (const int chosen = DrawChoices(ui, choices); chosen >= 0) { s.screenObject = ids[static_cast<size_t>(chosen)]; screenPicker_ = false; }
                }
                EditText(ui, "td2.television.screen.slot", "材质槽", s.screenSlot);
                if (!model_->screenWarning.empty()) { ui.Text(model_->screenWarning); }
            }
        }
        // 数値のドラッグも草稿に集め、編集の終わりに一回だけ確定する。
        if (s != before) { propertyPending_ = true; }
        if (propertyPending_ && !model_->gesture && !ui.GetInteraction().editing) { model_->Commit(ctx); propertyPending_ = false; }
    }
    void RefreshScreens(EditorWorkspaceHostContext& ctx) {
        screens_.clear(); uint64_t offset = 0;
        // 初期の候補はモデルを持つ物体に絞る（灯や記録用の物体で保存欄を押し出さない）。
        for (int page = 0; page < 256; ++page) {
            const auto result = Execute(ctx, "scene.objects.query", {{"detail", true}, {"offset", offset}, {"limit", 128}});
            if (!result.ok) { model_->error = result.error; return; }
            for (const auto& object : result.data.value("objects", Json::array())) {
                const auto id = ObjectId(object.value("objectId", Json{}));
                const bool model = object.contains("model") && object["model"].is_object() &&
                    (!object["model"].value("assetId", "").empty() || object["model"].value("materialSlotCount", 0) > 0);
                if (!id || (!allScreens_ && id != model_->draft.screenObject && !model)) { continue; }
                screens_.push_back({id, object.value("name", "对象 " + std::to_string(id))});
            }
            const auto next = ObjectId(result.data.value("nextOffset", Json{}));
            if (!next || next <= offset) { break; } offset = next;
        }
    }
    Drag drag_ = Drag::None;
    EditorWorkspacePoint start_{}, last_{};
    DeviceDraft original_;
    int tool_ = 0, handle_ = -1, roomPage_ = 0, devicePage_ = 0, targetPage_ = 0, brushSize_ = 1;
    bool erase_ = false, propertyPending_ = false, screenSettings_ = false, screenPicker_ = false, allScreens_ = false;
    bool gallery_ = false;
    bool rectanglePaint_ = false;
    std::set<size_t> erasedDeviceCells_;
    std::string kind_ = "Plate", linkSource_, deviceSearch_, gallerySearch_;
    std::string screenSearch_;
    int screenPage_ = 0;
    std::vector<std::string> groupIds_;
    std::vector<DeviceDraft> clipboard_;
    std::vector<SceneItem> screens_;
};

struct EdgeSelection { uint64_t a = 0, b = 0; SharedWorldEdge edge; };
struct SequenceAsset { std::string guid, name; std::vector<std::string> slots; bool slotsKnown = false; };

// 世界地図は既存テレビの整数原点だけを編集する。
class WorldMapWorkspace final : public MapWorkspace {
public:
    using MapWorkspace::MapWorkspace;
    bool IsDirty() const override { return MapWorkspace::IsDirty() || (sequenceDraft_ && pendingBinding_ != observedBinding_); }
    void DiscardChanges(EditorWorkspaceHostContext& ctx) override { sequenceDraft_ = false; bindingRefresh_ = true; MapWorkspace::DiscardChanges(ctx); }
    EditorWorkspaceDescriptor GetDescriptor() const override {
        EditorWorkspaceDescriptor d; d.workspaceId = kWorldWorkspace; d.displayName = "世界地图";
        d.options = static_cast<uint32_t>(EditorWorkspaceOption::RequestSceneViewport); return d;
    }
    void OnActivate(EditorWorkspaceHostContext& ctx) override { MapWorkspace::OnActivate(ctx); assetsRefresh_ = true; bindingRefresh_ = true; }
    void OnDeactivate(EditorWorkspaceHostContext& ctx) override { if (libraryDrag_) { model_->Discard(); } else { MapWorkspace::OnDeactivate(ctx); } moving_ = false; libraryDrag_ = false; pickingStart_ = false; }
    void Draw(EditorWorkspaceHostContext& ctx) override {
        if (!ctx.ui || !ctx.commands) { return; }
        model_->Poll(ctx); clock_ += ctx.unscaledDeltaSeconds;
        dragHeldAny_ = dragPressedAny_ = dragReleasedAny_ = false;
        auto& ui = *ctx.ui;
        DrawLibrary(ctx);
        if (MapPanelReady()) {
            if (ui.BeginPanel("td2.world.canvas.panel.v5", "世界地图", nullptr, EditorWorkspaceDockRegion::Center)) {
                // 一覧→メインキャンバスの順でドラッグの入力をまとめる。操作ボタンも見える中央に置く。
                HistoryToolbar(ctx, kTelevisionWorkspace, "编辑电视");
                ui.SameLine(); SaveControls(ctx);
                // 三つの見方：配置（テレビを世界に並べる）・カメラ大区域（大区域を塗り分ける）・俯瞰カメラ（カメラと経路を上から見る）。
                constexpr std::array<std::string_view, 3> modes{"布局", "镜头大区域", "俯视镜头"};
                for (size_t i = 0; i < modes.size(); ++i) {
                    ui.SameLine();
                    if (ui.Button("td2.world.mode." + std::to_string(i), (mode_ == static_cast<int>(i) ? std::string("● ") : std::string()) + std::string(modes[i]))) { SetMode(ctx, static_cast<int>(i)); }
                }
                ui.Separator();
                if (pickingStart_) {
                    ui.Text("点击角色脚下的位置开始游玩 · 空中也可选 · 右键取消");
                    if (ui.Button("td2.world.spawn.cancel", "取消选择出生位置")) { pickingStart_ = false; }
                } else if (mode_ == 1) {
                    ui.Text(areaFocus_.empty() ? "在右栏选择或新建一个大区域，再点击电视加入 / 移出" : "点击电视加入或移出「" + areaFocus_ + "」");
                }
                // 試遊できない理由は右欄だけでなく、クリックしたキャンバスにも出す。
                if (!model_->error.empty()) { ui.Text(model_->error); }
                if (mode_ != 0) { EnsureLayout(ctx); }
                if (mode_ == 2) { DrawPlan(ctx); } else { DrawCanvas(ctx); }
            } ui.EndPanel();
        }
        if (ui.BeginPanel("td2.world.properties", "选中属性", nullptr, EditorWorkspaceDockRegion::Right)) { DrawProperties(ctx); } ui.EndPanel();
        model_->Publish(ctx);
    }
private:
    bool ApplyDraft(EditorWorkspaceHostContext& ctx) override {
        if (sequenceDraft_ && pendingBinding_ != observedBinding_ && !WriteBinding(ctx, pendingBinding_)) { return false; }
        sequenceDraft_ = false; return MapWorkspace::ApplyDraft(ctx);
    }
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
    void OpenTelevision(EditorWorkspaceHostContext& ctx, uint64_t id) { if (LeaveSequence(ctx) && model_->Select(ctx, id) && model_->Commit(ctx)) { ctx.commands->RequestActivateWorkspace(kTelevisionWorkspace); } }
    void DrawLibrary(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        if (ui.BeginPanel("td2.world.library", "电视地图", nullptr, EditorWorkspaceDockRegion::Left)) {
            SaveControls(ctx, "td2.map.save.backup"); ui.Separator();
            if (ui.Button("td2.world.add_selected", "选中电视建图") && LeaveSequence(ctx)) { model_->AddSelectedTelevision(ctx); OpenTelevision(ctx, model_->selected); }
            if (ui.Button("td2.world.new", "+ 空白地图") && LeaveSequence(ctx)) { model_->Create(ctx); OpenTelevision(ctx, model_->selected); }
            EditText(ui, "td2.world.rooms.search", "查找", librarySearch_);
            const auto rooms = model_->rooms;
            for (size_t i = 0; i < rooms.size(); ++i) {
                const auto& room = rooms[i];
                if (!librarySearch_.empty() && room.object.name.find(librarySearch_) == std::string::npos &&
                    room.settings.region.regionId.find(librarySearch_) == std::string::npos) { continue; }
                ui.Text(ShortChoiceLabel(room.object.name) + "  " + std::to_string(room.settings.region.cols) + "×" + std::to_string(room.settings.region.rows));
                const auto input = ui.BeginCanvas("td2.world.thumbnail." + std::to_string(room.object.objectId), 92);
                dragHeldAny_ |= (input.heldButtons & kPrimary) != 0;
                dragPressedAny_ |= (input.pressedButtons & kPrimary) != 0;
                dragReleasedAny_ |= (input.releasedButtons & kPrimary) != 0;
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
        } ui.EndPanel();
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
        const auto input = ui.BeginCanvas("td2.world.canvas", 640);
        view_.ObserveSize(input.size);
        dragHeldAny_ |= (input.heldButtons & kPrimary) != 0;
        dragPressedAny_ |= (input.pressedButtons & kPrimary) != 0;
        dragReleasedAny_ |= (input.releasedButtons & kPrimary) != 0;
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
            if (mode_ == 0 && !pickingStart_ && !moving_ && !libraryDrag_ && !hand_ && !view_.panning && input.hovered && (input.pressedButtons & kPrimary)) {
                bool hitEdge = false;
                for (const auto& e : Edges()) {
                    const auto [a, b] = EdgePoints(e.edge);
                    const float dx = b.x - a.x, dy = b.y - a.y;
                    const float t = std::clamp(((input.pointer.x - a.x) * dx + (input.pointer.y - a.y) * dy) / (std::max)(.001f, dx * dx + dy * dy), 0.0f, 1.0f);
                    if (std::hypot(input.pointer.x - a.x - t * dx, input.pointer.y - a.y - t * dy) < 6) { ChooseEdge(ctx, e); hitEdge = true; break; }
                }
                if (!hitEdge) {
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
                const auto& s = room.object.objectId == model_->selected && model_->ready ? model_->draft : room.settings;
                if (!s.worldPlaced) { continue; }
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
                const bool selected = room.object.objectId == model_->selected && edge_.a == 0;
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
            DrawSpawnPicker(ctx, input, p);
        }
        ui.EndCanvas();
        if (!pickingStart_ && !model_->gesture && !ui.GetInteraction().editing && ui.GetInteraction().deletePressed && edge_.a == 0 && model_->ready) { model_->draft.worldPlaced = false; model_->Commit(ctx); }
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
        mode_ = mode; layoutStale_ = true; pickingStart_ = false;
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
            std::string joined;
            for (const auto& r : regions) { if (!joined.empty()) { joined += ","; } joined += r; }
            commands.push_back({{"objectId", area.objectId}, {"componentId", area.componentId}, {"propertyId", "regions"}, {"value", joined}});
        }
        const auto written = Execute(ctx, "authoring.transaction", {{"commands", commands}});
        if (!written.ok) { model_->error = written.error; return; }
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
                const auto set = [this](std::string_view id, double v) { return Json{{"objectId", areaDraftObject_}, {"componentId", areaDraftComponent_}, {"propertyId", id}, {"value", v}}; };
                const auto written = Execute(ctx, "authoring.transaction", {{"commands", {set("frameFill", areaDraft_[0]), set("orbitDegrees", areaDraft_[1]), set("cameraLift", areaDraft_[2])}}});
                if (!written.ok) { model_->error = written.error; }
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
        const auto input = ui.BeginCanvas("td2.world.plan", 640);
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
                const double yaw = r.tv.rotation[1] * 3.14159265358979 / 180;
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
                    const double cy = r.camera.rotation[1] * 3.14159265358979 / 180;
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
};

void WorldMapWorkspace::RefreshAssets(EditorWorkspaceHostContext& ctx) {
    assetsRefresh_ = false; assets_.clear(); cameras_.clear();
    uint64_t offset = 0;
    // インポーターの公開の索引を使う。選んだ資産のスロットは標準の文書から取る。
    for (;;) {
        const auto result = Execute(ctx, "asset.query", {{"type", "Sequence"}, {"offset", offset}, {"limit", 128}});
        if (!result.ok) { model_->error = result.error; return; }
        const auto entries = result.data.value("assets", Json::array());
        if (!entries.is_array()) { model_->error = "Sequence 资产索引未返回数组"; return; }
        for (const auto& item : entries) {
            const auto guid = item.value("guid", ""), name = item.value("name", item.value("path", "Sequence"));
            if (!guid.empty()) { assets_.push_back({guid, name, {}, false}); }
        }
        if (!result.data.contains("nextOffset")) { break; }
        const auto next = ObjectId(result.data["nextOffset"]);
        if (next <= offset || entries.empty()) { model_->error = "Sequence 资产索引分页无效"; return; }
        offset = next;
    }
    std::sort(assets_.begin(), assets_.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    QueryObjects(ctx, "CameraComponent", cameras_, model_->error);
}

bool WorldMapWorkspace::ReadAssetSlots(EditorWorkspaceHostContext& ctx, SequenceAsset& asset) {
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

void WorldMapWorkspace::ReadBinding(EditorWorkspaceHostContext& ctx) {
    bindingRefresh_ = false; bindingTarget_ = {}; binding_ = {}; bindingValid_ = false; bindingRevision_ = model_->revision;
    if (!edge_.a || !edge_.b) { return; }
    const auto a = std::find_if(model_->rooms.begin(), model_->rooms.end(), [this](const auto& r) { return r.object.objectId == edge_.a; });
    const auto b = std::find_if(model_->rooms.begin(), model_->rooms.end(), [this](const auto& r) { return r.object.objectId == edge_.b; });
    if (a == model_->rooms.end() || b == model_->rooms.end()) { return; }
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

bool WorldMapWorkspace::WriteBinding(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& requested) {
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
    const auto add = [&commands, &target](std::string_view id, const auto& value) { commands.push_back({{"objectId", target.objectId}, {"componentId", target.componentId}, {"propertyId", id}, {"value", value}}); };
    add("regionA", next.regionA); add("regionB", next.regionB); add("sequenceAssetGuid", next.sequenceAssetGuid); add("sequenceSlotName", next.sequenceSlotName); add("cameraObject", next.cameraObject);
    const auto result = Execute(ctx, "authoring.transaction", {{"commands", commands}});
    if (!result.ok) { if (createdRecord) { Execute(ctx, "scene.delete_object", {{"objectId", target.objectId}}); } model_->error = result.error; return false; }
    model_->sceneDirty = true;
    bindingRefresh_ = true; ReadBinding(ctx);
    if (bindingTarget_ != target || binding_ != next) { model_->error = "Sequence 写入回读不一致"; return false; }
    sequenceDraft_ = false; model_->error.clear(); return true;
}

void WorldMapWorkspace::DrawProperties(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (!model_->error.empty()) { ui.Text(model_->error); }
    if (!report_.empty()) { ui.Text(report_); }
    if (mode_ == 1) { DrawAreaPanel(ctx); return; }
    if (pickingStart_) {
        ui.Text("在世界地图中选择出生位置");
        ui.Text("绿色轮廓是角色位置；点击立即开始");
        if (ui.Button("td2.world.spawn.cancel.properties", "取消")) { pickingStart_ = false; }
        return;
    }
    if (edge_.a) {
        if (assetsRefresh_) { RefreshAssets(ctx); }
        if (bindingRefresh_ || bindingRevision_ != model_->revision) { ReadBinding(ctx); }
        const auto a = std::find_if(model_->rooms.begin(), model_->rooms.end(), [this](const auto& r) { return r.object.objectId == edge_.a; });
        const auto b = std::find_if(model_->rooms.begin(), model_->rooms.end(), [this](const auto& r) { return r.object.objectId == edge_.b; });
        if (a == model_->rooms.end() || b == model_->rooms.end()) { return; }
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

std::vector<std::unique_ptr<IEditorWorkspace>> MakeMapWorkspaces() {
    auto model = std::make_shared<MapModel>();
    std::vector<std::unique_ptr<IEditorWorkspace>> workspaces;
    workspaces.push_back(std::make_unique<TelevisionMapWorkspace>(model));
    workspaces.push_back(std::make_unique<WorldMapWorkspace>(model));
    return workspaces;
}

}
