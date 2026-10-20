#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
namespace {
constexpr float kTileHeight = 58;
constexpr EditorWorkspaceColor kTileBack{.06f, .09f, .11f, 1}, kTileFrame{.22f, .32f, .35f, 1}, kTileOn{.97f, .80f, .40f, 1};

// 装置の記号を棚の枠に合わせて描く（細長い装置は横長・縦長の形のまま）。
void DeviceIcon(IEditorWorkspaceUi& ui, std::string_view kind, EditorWorkspacePoint size) {
    DeviceDraft d; d.kind = kind;
    if (kind == "Door" || kind == "LockWall") { d.h = 2; }
    if (kind == "OneWay" || kind == "Bridge") { d.w = 2; }
    if (kind == "Laser") { d.direction = "Right"; }
    if (kind == "Spikes") { d.w = 2; }
    MapCanvasView v;
    v.tile = (std::min)((size.x - 16) / static_cast<float>(d.w + .5), (size.y - 24) / static_cast<float>(d.h + .5));
    v.tile = std::clamp(v.tile, 6.0f, 26.0f);
    v.origin = {(size.x - static_cast<float>(d.w) * v.tile) * .5f, (size.y - 18 - static_cast<float>(d.h) * v.tile) * .5f};
    DrawDeviceGlyph(ui, d, v);
}

// テレビの型：画面の縦横比の枠と、中の画面。
void TelevisionIcon(IEditorWorkspaceUi& ui, const TelevisionModel& m, EditorWorkspacePoint size) {
    const float w = size.x - 24, h = size.y - 26;
    const float k = (std::min)(w / m.cols, h / m.rows);
    const float bw = m.cols * k, bh = m.rows * k;
    const EditorWorkspacePoint a{(size.x - bw) * .5f, (size.y - 18 - bh) * .5f};
    ui.CanvasFill({a.x - 3, a.y - 3}, {a.x + bw + 3, a.y + bh + 3}, {.20f, .24f, .26f, 1});
    ui.CanvasFill(a, {a.x + bw, a.y + bh}, {.36f, .62f, .64f, 1});
}
}

bool LevelEditor::Tile(IEditorWorkspaceUi& ui, std::string_view id, std::string_view label, bool selected,
                       const std::function<void(IEditorWorkspaceUi&, EditorWorkspacePoint)>& icon) {
    const auto input = ui.BeginCanvas(id, kTileHeight);
    TrackDrag(input);
    bool clicked = false;
    if (input.size.x > 0 && input.size.y > 0) {
        ui.CanvasFill({0, 0}, input.size, kTileBack);
        if (icon) { icon(ui, input.size); }
        ui.CanvasText({6, input.size.y - 17}, {.86f, .9f, .9f, 1}, ShortChoiceLabel(label, 8));
        ui.CanvasRect({1, 1}, {input.size.x - 1, input.size.y - 1}, selected ? kTileOn : input.hovered ? EditorWorkspaceColor{.45f, .62f, .64f, 1} : kTileFrame, selected ? 2.5f : 1.0f);
        clicked = input.hovered && (input.pressedButtons & kPrimary);
    }
    ui.EndCanvas();
    return clicked;
}

void LevelEditor::DrawPalette(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    switch (tool_) {
    case Tool::Select: DrawTelevisionShelf(ctx); break;
    case Tool::Terrain: {
        ui.Text("笔刷");
        for (int size = 1; size <= 4; ++size) {
            ui.SameLine();
            if (ui.Button("td2.palette.brush." + std::to_string(size), (brushSize_ == size ? "● " : "") + std::to_string(size))) { brushSize_ = size; }
        }
        ui.SameLine(); ui.Checkbox("td2.palette.rectangle", "矩形", &rectanglePaint_);
        ui.Text("左键画 · 右键擦");
        break;
    }
    case Tool::Device: {
        constexpr size_t columns = 6;
        if (ui.BeginTable("td2.palette.devices", static_cast<uint32_t>(columns))) {
            for (size_t i = 0; i < kEditorDeviceKinds.size(); ++i) {
                if (i % columns == 0) { ui.TableNextRow(); }
                ui.TableNextColumn();
                const auto kind = kEditorDeviceKinds[i];
                if (Tile(ui, "td2.palette.device." + std::string(kind), DeviceKindName(kind), kind_ == kind, [kind](IEditorWorkspaceUi& u, EditorWorkspacePoint s) { DeviceIcon(u, kind, s); })) {
                    kind_ = std::string(kind); model_.devices.Select(-1); if (model_.ready) { focus_ = Focus::Room; }
                }
            }
            ui.EndTable();
        }
        break;
    }
    case Tool::Link: ui.Text("从压板拖到门、激光、桥或机构 · 再连一次断开"); break;
    case Tool::Machine: DrawMachineShelf(ctx); break;
    case Tool::Camera: DrawCameraShelf(ctx); break;
    case Tool::Stage: DrawTelevisionShelf(ctx); DrawPropShelf(ctx); break;
    }
}

// 新しいテレビ：型を選んで、地図の上の置きたい所を押す。
void LevelEditor::DrawTelevisionShelf(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    constexpr size_t columns = 9;
    if (!ui.BeginTable("td2.palette.televisions", static_cast<uint32_t>(columns))) { return; }
    ui.TableNextRow();
    for (size_t i = 0; i < std::size(kTelevisionModels); ++i) {
        ui.TableNextColumn();
        const auto& m = kTelevisionModels[i];
        if (Tile(ui, "td2.palette.tv." + std::to_string(i), std::string(m.label), placeModel_ == static_cast<int>(i), [&m](IEditorWorkspaceUi& u, EditorWorkspacePoint s) { TelevisionIcon(u, m, s); })) {
            placeModel_ = placeModel_ == static_cast<int>(i) ? -1 : static_cast<int>(i);
            pickingStart_ = false;
        }
    }
    ui.EndTable();
}

void LevelEditor::DrawPropShelf(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    // 組ごとに一行の表（欄が狭くても切れないよう、列の数を決めて並べる）。
    constexpr size_t columns = 8;
    for (size_t g = 0; g < kStagePropGroups.size(); ++g) {
        ui.Text(std::string(kStagePropGroups[g]));
        if (!ui.BeginTable("td2.palette.props." + std::to_string(g), static_cast<uint32_t>(columns))) { continue; }
        size_t n = 0;
        for (size_t i = 0; i < std::size(kStageProps); ++i) {
            const auto& prop = kStageProps[i];
            if (prop.group != static_cast<int>(g)) { continue; }
            if (n++ % columns == 0) { ui.TableNextRow(); }
            ui.TableNextColumn();
            if (ui.Button("td2.palette.prop." + std::to_string(i), std::string(prop.label))) { ui.EndTable(); PlaceProp(ctx, prop); return; }
        }
        ui.EndTable();
    }
}

// 台は今のテレビの真下の床に置き、高さが分かったらテレビを台の上へ載せる（FinishStand）。ほかはビューポートの前に置いて選ぶ。
void LevelEditor::PlaceProp(EditorWorkspaceHostContext& ctx, const StageProp& prop) {
    const auto* current = model_.Current();
    StageObject tv;
    Pose pose;
    std::string error;
    const bool under = prop.stand && current && focus_ == Focus::Room && ReadStageObject(ctx, MapModel::TelevisionObject(*current), tv, error);
    if (under) { pose = tv.pose; pose.position[1] = 0; pose.rotation = {0, tv.pose.rotation[1], 0}; pose.scale = {1, 1, 1}; }
    else if (!PoseAtView(ctx, pose, model_.error)) { return; }
    pose.scale = prop.scale;
    const uint64_t id = CreateStageModel(ctx, prop.path, prop.label, pose, model_.error);
    if (!id) { return; }
    MarkSceneChanged();
    if (under) {
        JoinFolderOf(ctx, id, tv.objectId);
        stand_ = id; standTelevision_ = tv.objectId; standFrames_ = 240;
        SetNote("已在「" + ShortChoiceLabel(tv.name, 12) + "」下面放「" + std::string(prop.label) + "」");
    } else {
        if (current) { JoinFolderOf(ctx, id, MapModel::TelevisionObject(*current)); }
        if (Execute(ctx, "scene.object.select", {{"objectId", id}}).ok) { model_.sceneSelection = id; }
        focus_ = Focus::Object;
        SetNote("已放「" + std::string(prop.label) + "」：在视口里用 W / E 摆好");
    }
}

void LevelEditor::FinishStand(EditorWorkspaceHostContext& ctx) {
    if (!stand_) { return; }
    StageObject stand, tv;
    std::string error;
    if (!ReadStageObject(ctx, stand_, stand, error) || !ReadStageObject(ctx, standTelevision_, tv, error)) { stand_ = 0; model_.error = error; return; }
    if (!stand.hasBounds) {
        if (--standFrames_ <= 0) { stand_ = 0; model_.error = "读不到台的高度：请在视口里把电视移到台面上"; }
        return;
    }
    tv.pose.position[1] = stand.hi[1];
    const auto television = standTelevision_;
    stand_ = 0;
    if (!SetStagePose(ctx, television, tv.pose, model_.error)) { return; }
    stageClock_ = 10;
    for (const auto& room : model_.rooms) {
        if (MapModel::TelevisionObject(room) == television) { const std::string region = room.settings.region.regionId; Generate(ctx, true, false, {}, region, nullptr, false); break; }
    }
    SetNote("电视已放到台面上并重新生成机位");
}
}
