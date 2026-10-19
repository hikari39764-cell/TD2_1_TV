#pragma once
#include "TD2WorldMode.h"

namespace TD2::Editor::Level {
// ---------------------------------------------------------------- 3D の舞台（下の欄）
// ビューポートの操作の案内、選んだ物体の位置と向き、テレビの新規作成、台や道具の配置、地図の隣に合わせた並べ方。
// 物体を動かすのは標準のビューポートのギズモ（W / E / R）が主で、ここは数値と「一押しで済む」並べ方だけを受け持つ。
class StagePanel {
public:
    void Draw(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world) {
        auto& ui = *ctx.ui;
        ui.Text("视口：左键点选 · 按住右键拖动看四周（同时 WASD 移动、Q / E 降升、滚轮调速度）· 滚轮前后推 · F 聚焦选中\n"
                "手柄：W 移动 / E 旋转 / R 缩放 / Q 收起 · X 切换世界 / 本地坐标 · Ctrl+D 复制 · Delete 删除");
        if (!error_.empty()) { ui.Text("⚠ " + error_); } else if (!note_.empty()) { ui.Text(note_); }
        Refresh(ctx, model);
        FinishStand(ctx, model, world);
        if (ui.BeginTable("td2.stage.columns", 3)) {
            ui.TableNextRow();
            ui.TableNextColumn(); DrawSelection(ctx, model, world);
            ui.TableNextColumn(); DrawTelevisions(ctx, model, world);
            ui.TableNextColumn(); DrawProps(ctx, model, world);
            ui.EndTable();
        }
    }
private:
    // 選んだ物体の姿勢を読む（選択が変わった時と、時々）。数値を入力している間は読み直さない。
    void Refresh(EditorWorkspaceHostContext& ctx, MapModel& model) {
        clock_ += ctx.unscaledDeltaSeconds;
        const uint64_t id = model.sceneSelection;
        if (pending_ || (id == object_ && clock_ < .5f)) { return; }
        clock_ = 0; object_ = id; readOk_ = false;
        std::string error;
        if (id) { readOk_ = ReadStageObject(ctx, id, read_, error); }
    }
    const RoomItem* RoomOf(const MapModel& model, uint64_t object) const {
        for (const auto& room : model.rooms) { if (object && MapModel::TelevisionObject(room) == object) { return &room; } }
        return nullptr;
    }
    // テレビを動かした後、そのカメラを今の配置で作り直す（作者が手で直したカメラは残る）。
    void Recamera(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world, uint64_t television) {
        if (const auto* room = RoomOf(model, television)) { const std::string region = room->settings.region.regionId; world.SetUpNewRoom(ctx, region, {}); }
    }
    void DrawSelection(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world) {
        auto& ui = *ctx.ui;
        if (!object_ || !readOk_) { ui.Text("在视口里点选一个物体"); return; }
        const auto* room = RoomOf(model, object_);
        ui.Text("选中：" + ShortChoiceLabel(read_.name, 18) + (read_.television ? "（" + std::string(read_.television->label) + "）" : ""));
        if (room) { ui.Text("区域 " + ShortChoiceLabel(room->settings.region.regionId, 14) + " · 左栏和右侧地图编辑它"); }
        else if (read_.television && ui.Button("td2.stage.adopt", "把这台电视建成区域")) {
            const auto model3d = *read_.television;
            if (model.AdoptTelevision(ctx, object_, model3d, std::nullopt)) { const auto* fresh = model.Current(); if (fresh) { world.SetUpNewRoom(ctx, fresh->settings.region.regionId, {}); } note_ = "已建成区域，请在世界布局里把它拖进世界"; error_.clear(); }
            else { error_ = model.error; }
            return;
        }
        if (!pending_) { editing_ = read_.pose; }
        bool changed = false;
        changed |= EditFloat(ui, "td2.stage.x", "X（米）", editing_.position[0], -1000, 1000);
        changed |= EditFloat(ui, "td2.stage.y", "Y（米，高度）", editing_.position[1], -1000, 1000);
        changed |= EditFloat(ui, "td2.stage.z", "Z（米）", editing_.position[2], -1000, 1000);
        changed |= EditFloat(ui, "td2.stage.yaw", "朝向（度，绕竖轴）", editing_.rotation[1], -360, 360);
        const bool uniform = std::abs(editing_.scale[0] - editing_.scale[1]) < 1e-4 && std::abs(editing_.scale[0] - editing_.scale[2]) < 1e-4;
        if (uniform) {
            double s = editing_.scale[0];
            if (EditFloat(ui, "td2.stage.scale", "缩放", s, .05f, 20)) { editing_.scale = {s, s, s}; changed = true; }
        }
        // よく使う向きの直し：15° と 90° ずつ。
        constexpr std::array<std::pair<double, std::string_view>, 4> turns{{{-90, "↺ 90°"}, {-15, "↺ 15°"}, {15, "↻ 15°"}, {90, "↻ 90°"}}};
        for (size_t i = 0; i < turns.size(); ++i) {
            if (i) { ui.SameLine(); }
            if (ui.Button("td2.stage.turn." + std::to_string(i), turns[i].second)) { editing_.rotation[1] = std::fmod(editing_.rotation[1] + turns[i].first + 540.0, 360.0) - 180; changed = true; }
        }
        if (ui.Button("td2.stage.floor", "落到地面") && read_.hasBounds) { editing_.position[1] -= read_.lo[1]; changed = true; }
        if (room) { ui.SameLine(); if (ui.Button("td2.stage.recamera", "按现在的位置重新生成机位")) { Recamera(ctx, model, world, object_); note_ = "已重新生成机位（手调过的机位保留）"; } }
        if (changed) { pending_ = true; }
        if (pending_ && !ui.GetInteraction().editing) {
            pending_ = false;
            std::string error;
            if (SetStagePose(ctx, object_, editing_, error)) { error_.clear(); model.sceneDirty = true; } else { error_ = error; }
            clock_ = 1;
        }
    }
    void DrawTelevisions(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world) {
        auto& ui = *ctx.ui;
        const auto* current = model.Current();
        ui.Text(current ? "新建电视（放在「" + ShortChoiceLabel(current->object.name, 10) + "」右边，地图接在它右侧）" : "新建电视（放在视口前方的地面）");
        constexpr size_t columns = 3;
        if (ui.BeginTable("td2.stage.televisions", static_cast<uint32_t>(columns))) {
            for (size_t i = 0; i < std::size(kTelevisionModels); ++i) {
                if (i % columns == 0) { ui.TableNextRow(); }
                ui.TableNextColumn();
                const auto& m = kTelevisionModels[i];
                if (ui.Button("td2.stage.tv." + std::to_string(i), std::string(m.label) + " " + std::to_string(m.cols) + "×" + std::to_string(m.rows))) { CreateTelevision(ctx, model, world, m); return; }
            }
            ui.EndTable();
        }
        // 地図で隣り合うテレビの横・上・下へ、今のテレビを 3D でも寄せる。
        if (!current || !current->settings.worldPlaced) { return; }
        const auto here = Placement(current->settings);
        bool any = false;
        for (const auto& other : model.rooms) {
            if (other.object.objectId == current->object.objectId || !other.settings.worldPlaced) { continue; }
            const auto edge = FindSharedWorldEdge(Placement(other.settings), here);
            if (!edge) { continue; }
            const int side = edge->sideA == WorldSide::Right ? 1 : edge->sideA == WorldSide::Left ? -1 : edge->sideA == WorldSide::Top ? 2 : -2;
            const std::string where = side == 1 ? "右边" : side == -1 ? "左边" : side == 2 ? "上面" : "下面";
            if (!any) { ui.Text("按世界地图贴到邻居旁（3D）"); any = true; }
            if (ui.Button("td2.stage.snap." + std::to_string(other.object.objectId), "贴到「" + ShortChoiceLabel(other.object.name, 10) + "」" + where)) {
                SnapBeside(ctx, model, world, MapModel::TelevisionObject(*current), MapModel::TelevisionObject(other), side);
                return;
            }
        }
    }
    void CreateTelevision(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world, const TelevisionModel& m) {
        error_.clear(); note_.clear();
        const auto* current = model.Current();
        // 区域の一覧は作り直されるので、隣の情報は先に値で写す。
        const std::optional<RegionSettings> beside = current ? std::optional<RegionSettings>(current->settings) : std::nullopt;
        const std::string besideRegion = current ? current->settings.region.regionId : std::string{};
        StageObject anchor;
        std::string error;
        const bool hasAnchor = current && ReadStageObject(ctx, MapModel::TelevisionObject(*current), anchor, error);
        Pose pose;
        if (hasAnchor) { pose = PoseBeside(anchor, m.width * m.scale, m.height * m.scale, 1); }
        else if (!PoseAtView(ctx, pose, error_)) { return; }
        pose.scale = {m.scale, m.scale, m.scale};
        const std::string name = "电视 " + std::string(m.label);
        const uint64_t id = CreateStageModel(ctx, m.path, name, pose, error_);
        if (!id) { return; }
        model.sceneDirty = true;
        if (hasAnchor) { JoinFolderOf(ctx, id, anchor.objectId); }
        if (!model.AdoptTelevision(ctx, id, m, beside)) { error_ = model.error; return; }
        const auto* fresh = model.Current();
        if (fresh) { const std::string region = fresh->settings.region.regionId; world.SetUpNewRoom(ctx, region, besideRegion); }
        if (Execute(ctx, "scene.object.select", {{"objectId", id}}).ok) { model.sceneSelection = id; }
        fresh = model.Current();
        note_ = "已新建「" + name + "」：地图 " + std::to_string(m.cols) + "×" + std::to_string(m.rows) +
                (fresh && fresh->settings.worldPlaced ? "，已接到世界地图里并生成机位" : "，已生成机位") + " · 在右侧地图画地形";
        if (!model.error.empty()) { note_ += " · " + model.error; }
    }
    void SnapBeside(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world, uint64_t television, uint64_t neighbor, int side) {
        StageObject self, other;
        if (!ReadStageObject(ctx, television, self, error_) || !ReadStageObject(ctx, neighbor, other, error_)) { return; }
        auto pose = PoseBeside(other, StageWidth(self), StageHeight(self), side);
        pose.scale = self.pose.scale;
        if (!SetStagePose(ctx, television, pose, error_)) { return; }
        model.sceneDirty = true; error_.clear(); clock_ = 1;
        Recamera(ctx, model, world, television);
        note_ = "已贴到「" + ShortChoiceLabel(other.name, 12) + "」旁边并重新生成机位";
    }
    void DrawProps(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode&) {
        auto& ui = *ctx.ui;
        const auto* current = model.Current();
        ui.Text(current ? "布景（台类垫在「" + ShortChoiceLabel(current->object.name, 10) + "」下面，其余放在视口前方）" : "布景（放在视口前方）");
        constexpr size_t columns = 4;
        if (ui.BeginTable("td2.stage.props", static_cast<uint32_t>(columns))) {
            for (size_t i = 0; i < std::size(kStageProps); ++i) {
                if (i % columns == 0) { ui.TableNextRow(); }
                ui.TableNextColumn();
                const auto& prop = kStageProps[i];
                if (ui.Button("td2.stage.prop." + std::to_string(i), std::string(prop.stand && current ? "▁ " : "") + std::string(prop.label))) { PlaceProp(ctx, model, prop); return; }
            }
            ui.EndTable();
        }
    }
    // 台はテレビの真下の床に置き、モデルが読み込まれて高さが分かったらテレビを台の上へ載せる（FinishStand）。
    void PlaceProp(EditorWorkspaceHostContext& ctx, MapModel& model, const StageProp& prop) {
        error_.clear(); note_.clear();
        const auto* current = model.Current();
        StageObject tv;
        Pose pose;
        const bool under = prop.stand && current && ReadStageObject(ctx, MapModel::TelevisionObject(*current), tv, error_);
        // 台は床に立てる：テレビの真下、向きはテレビの縦軸の向きだけ写す。
        if (under) { pose = tv.pose; pose.position[1] = 0; pose.rotation = {0, tv.pose.rotation[1], 0}; pose.scale = {1, 1, 1}; }
        else if (!PoseAtView(ctx, pose, error_)) { return; }
        const uint64_t id = CreateStageModel(ctx, prop.path, prop.label, pose, error_);
        if (!id) { return; }
        model.sceneDirty = true;
        if (under) {
            JoinFolderOf(ctx, id, tv.objectId);
            stand_ = id; standTelevision_ = tv.objectId; standFrames_ = 240;
            note_ = "已在「" + ShortChoiceLabel(tv.name, 12) + "」下面放「" + std::string(prop.label) + "」，读出台高后把电视放上去…";
        } else {
            if (Execute(ctx, "scene.object.select", {{"objectId", id}}).ok) { model.sceneSelection = id; }
            note_ = "已放「" + std::string(prop.label) + "」：用 W / E 移动、旋转它";
        }
    }
    void FinishStand(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world) {
        if (!stand_) { return; }
        StageObject stand, tv;
        std::string error;
        if (!ReadStageObject(ctx, stand_, stand, error) || !ReadStageObject(ctx, standTelevision_, tv, error)) { stand_ = 0; error_ = error; return; }
        if (!stand.hasBounds) {
            if (--standFrames_ <= 0) { stand_ = 0; error_ = "读不到台的高度：请用 W 手动把电视移到台面上"; }
            return;
        }
        tv.pose.position[1] = stand.hi[1];
        const auto television = standTelevision_;
        stand_ = 0;
        if (!SetStagePose(ctx, television, tv.pose, error_)) { return; }
        clock_ = 1;
        Recamera(ctx, model, world, television);
        note_ = "电视已放到台面上（高 " + std::to_string(static_cast<int>(std::round(stand.hi[1] * 100))) + " 厘米）并重新生成机位";
    }
    float clock_ = 1;
    uint64_t object_ = 0;
    StageObject read_;
    bool readOk_ = false, pending_ = false;
    Pose editing_;
    std::string error_, note_;
    uint64_t stand_ = 0, standTelevision_ = 0;
    int standFrames_ = 0;
};
}
