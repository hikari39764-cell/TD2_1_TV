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
    if (kind == "Laser" || kind == "Pusher") { d.direction = "Right"; }
    if (kind == "Spikes") { d.w = 2; }
    if (kind == "Goal") { d.w = 2; d.h = 3; }
    MapCanvasView v;
    v.tile = (std::min)((size.x - 16) / static_cast<float>(d.w + .5), (size.y - 24) / static_cast<float>(d.h + .5));
    v.tile = std::clamp(v.tile, 6.0f, 26.0f);
    v.origin = {(size.x - static_cast<float>(d.w) * v.tile) * .5f, (size.y - 18 - static_cast<float>(d.h) * v.tile) * .5f};
    DrawDeviceGlyph(ui, d, v);
}

// 地面：二段の塊。
void GroundIcon(IEditorWorkspaceUi& ui, EditorWorkspacePoint size) {
    const float tile = std::clamp((size.y - 26) / 2, 6.0f, 14.0f), w = tile * 3;
    const EditorWorkspacePoint a{(size.x - w) * .5f, (size.y - 18 - tile * 2) * .5f};
    ui.CanvasFill(a, {a.x + w, a.y + tile * 2}, {.36f, .62f, .64f, 1});
    for (int i = 1; i < 3; ++i) { ui.CanvasLine({a.x + tile * i, a.y}, {a.x + tile * i, a.y + tile * 2}, {.06f, .09f, .11f, 1}, 1); }
    ui.CanvasLine({a.x, a.y + tile}, {a.x + w, a.y + tile}, {.06f, .09f, .11f, 1}, 1);
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
    // テレビはシーンに置いたモデルへ「TD2 テレビの区域」を付けると、左の一覧に出る。
    case Tool::Select: ui.Text("テレビ：モデルを置いて「TD2 テレビの区域」を付ける"); break;
    // 「描く」：地面と装置を一つの棚に並べる。地面を選んだ時だけ筆の大きさを出す。
    case Tool::Terrain: case Tool::Device: {
        constexpr size_t columns = 7;
        if (ui.BeginTable("td2.palette.paint", static_cast<uint32_t>(columns))) {
            ui.TableNextRow(); ui.TableNextColumn();
            if (Tile(ui, "td2.palette.ground", "地面", tool_ == Tool::Terrain, [](IEditorWorkspaceUi& u, EditorWorkspacePoint s) { GroundIcon(u, s); })) { ChooseTool(ctx, Tool::Terrain); }
            for (size_t i = 0; i < kEditorDeviceKinds.size(); ++i) {
                if ((i + 1) % columns == 0) { ui.TableNextRow(); }
                ui.TableNextColumn();
                const auto kind = kEditorDeviceKinds[i];
                if (Tile(ui, "td2.palette.device." + std::string(kind), DeviceKindName(kind), tool_ == Tool::Device && kind_ == kind, [kind](IEditorWorkspaceUi& u, EditorWorkspacePoint s) { DeviceIcon(u, kind, s); })) {
                    kind_ = std::string(kind); ChooseTool(ctx, Tool::Device); model_.devices.Select(-1); if (model_.ready) { focus_ = Focus::Room; }
                }
            }
            ui.EndTable();
        }
        if (tool_ == Tool::Terrain) {
            for (int size = 1; size <= 4; ++size) {
                if (size > 1) { ui.SameLine(); }
                if (ui.Button("td2.palette.brush." + std::to_string(size), (brushSize_ == size ? "● " : "") + std::to_string(size))) { brushSize_ = size; }
            }
            ui.SameLine(); ui.Checkbox("td2.palette.rectangle", "矩形", &rectanglePaint_);
        }
        break;
    }
    case Tool::Link: break;
    case Tool::Machine: DrawMachineShelf(ctx); break;
    case Tool::Camera: DrawCameraShelf(ctx); break;
    case Tool::Stage: DrawPropShelf(ctx); break;
    }
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
    const bool under = prop.stand && current && focus_ == Focus::Room && ReadStageObject(ctx, current->object.objectId, tv, error);
    if (under) { pose = tv.pose; pose.position[1] = 0; pose.rotation = {0, tv.pose.rotation[1], 0}; pose.scale = {1, 1, 1}; }
    else if (!PoseAtView(ctx, pose, model_.error)) { return; }
    pose.scale = prop.scale;
    const uint64_t id = CreateStageModel(ctx, prop.path, prop.label, pose, model_.error);
    if (!id) { return; }
    MarkSceneChanged();
    if (under) {
        JoinFolderOf(ctx, id, tv.objectId);
        stand_ = id; standTelevision_ = tv.objectId; standFrames_ = 240;
        SetNote("「" + ShortChoiceLabel(tv.name, 12) + "」の下に「" + std::string(prop.label) + "」を置きました");
    } else {
        if (current) { JoinFolderOf(ctx, id, current->object.objectId); }
        if (Execute(ctx, "scene.object.select", {{"objectId", id}}).ok) { model_.sceneSelection = id; }
        focus_ = Focus::Object;
        SetNote("「" + std::string(prop.label) + "」を置きました：ビューポートで W / E で合わせてください");
    }
}

void LevelEditor::FinishStand(EditorWorkspaceHostContext& ctx) {
    if (!stand_) { return; }
    StageObject stand, tv;
    std::string error;
    if (!ReadStageObject(ctx, stand_, stand, error) || !ReadStageObject(ctx, standTelevision_, tv, error)) { stand_ = 0; model_.error = error; return; }
    if (!stand.hasBounds) {
        if (--standFrames_ <= 0) { stand_ = 0; model_.error = "台の高さを読めません：ビューポートでテレビを台の上へ動かしてください"; }
        return;
    }
    tv.pose.position[1] = stand.hi[1];
    const auto television = standTelevision_;
    stand_ = 0;
    if (!SetStagePose(ctx, television, tv.pose, model_.error)) { return; }
    stageClock_ = 10;
    for (const auto& room : model_.rooms) {
        if (room.object.objectId == television) { const std::string region = room.settings.region.regionId; Generate(ctx, true, false, {}, region, nullptr, false); break; }
    }
    SetNote("テレビを台の上に置き、カメラ位置を作り直しました");
}
}
