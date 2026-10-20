#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
namespace {
// バネ：強さ（高さの目安つき）・立ってから発射までの秒・冷却。既定値は書き出さない。
void DrawSpring(IEditorWorkspaceUi& ui, DeviceDraft& d) {
    const auto read = [&d](std::string_view key, double fallback) {
        const auto text = d.Extra(key);
        try { return text.empty() ? fallback : std::stod(text); } catch (...) { return fallback; }
    };
    double power = read("power", 22.5), delay = read("delay", .85), cooldown = read("cooldown", 3);
    if (EditFloat(ui, "td2.device.power", "弹力", power, 3, 44)) { d.SetExtra("power", std::abs(power - 22.5) < 1e-6 ? "" : ShortNumber(power)); }
    ui.Text("约弹起 " + ShortNumber(power * power / 87.5) + " 格");
    if (EditFloat(ui, "td2.device.delay", "站上去多久发射（秒）", delay, .05f, 3)) { d.SetExtra("delay", std::abs(delay - .85) < 1e-6 ? "" : ShortNumber(delay)); }
    if (EditFloat(ui, "td2.device.cooldown", "冷却（秒）", cooldown, 3, 30)) { d.SetExtra("cooldown", std::abs(cooldown - 3) < 1e-6 ? "" : ShortNumber(cooldown)); }
}

// 貼り絵の図案：按键の説明・出口の矢印・前景（実体の地形の時に手前に出る暗い塊と垂れた線）。
void DrawDecal(IEditorWorkspaceUi& ui, DeviceDraft& d) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 9> variants{{{"ad", "A / D"}, {"space", "空格"}, {"spaceHold", "长按空格"},
        {"exit", "出口 →"}, {"exitLeft", "出口 ←"}, {"exitUp", "出口 ↑"}, {"exitDown", "出口 ↓"}, {"fore", "前景块"}, {"foreCable", "前景线缆"}}};
    const auto current = d.Extra("variant");
    if (ui.BeginTable("td2.device.decal", 3)) {
        for (size_t i = 0; i < variants.size(); ++i) {
            if (i % 3 == 0) { ui.TableNextRow(); }
            ui.TableNextColumn();
            if (ui.Selectable("td2.device.decal." + std::string(variants[i].first), std::string(variants[i].second), current == variants[i].first)) { d.SetExtra("variant", variants[i].first); }
        }
        ui.EndTable();
    }
    if (current != "fore" && current != "foreCable") { return; }
    double depth = 1.0;
    try { if (const auto text = d.Extra("depth"); !text.empty()) { depth = std::stod(text); } } catch (...) {}
    if (EditFloat(ui, "td2.device.depth", "离角色多远（格）", depth, .25f, 6)) { d.SetExtra("depth", std::abs(depth - 1.0) < 1e-6 ? "" : ShortNumber(depth)); }
}
}

void LevelEditor::CommitPending(EditorWorkspaceHostContext& ctx) {
    // 数値の入力も草稿に集め、入力が終わってから一回だけ確定する。
    if (propertyPending_ && !model_.gesture && !ctx.ui->GetInteraction().editing) { model_.Commit(ctx); propertyPending_ = false; }
}

void LevelEditor::DrawInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (!note_.empty() && noteClock_ < 6) { ui.Text(note_); ui.Separator(); }
    if (pickingStart_) { DrawPlayInspector(ctx); return; }
    if (focus_ == Focus::Machine && !SelectedMachine()) { focus_ = Focus::None; }
    if ((focus_ == Focus::Room || focus_ == Focus::Device) && !model_.ready) { focus_ = Focus::None; }
    if (focus_ == Focus::Device && !model_.devices.HasSelection()) { focus_ = Focus::Room; }
    switch (focus_) {
    case Focus::Room: DrawRoomInspector(ctx); break;
    case Focus::Device: DrawDeviceInspector(ctx); break;
    case Focus::Machine: DrawMachineInspector(ctx); break;
    case Focus::Edge: DrawEdgeInspector(ctx); break;
    case Focus::Object: DrawObjectInspector(ctx); break;
    case Focus::None:
        if (tool_ == Tool::Machine) { DrawMachineInspector(ctx); }
        else if (tool_ == Tool::Camera) { DrawAreaInspector(ctx); }
        else { DrawOverview(ctx); }
        break;
    }
    CommitPending(ctx);
}

// ---------------------------------------------------------------- 一覧（何も選んでいない時）
void LevelEditor::RefreshChapters(EditorWorkspaceHostContext& ctx) {
    chapterClock_ += ctx.unscaledDeltaSeconds;
    if (chapterClock_ < 1 || (chapterPulse_ == model_.documentPulse && chapterClock_ < 30)) { return; }
    chapterClock_ = 0; chapterPulse_ = model_.documentPulse;
    chapterOf_.clear();
    const auto folders = Execute(ctx, "scene.folder.query", Json::object());
    if (!folders.ok) { return; }
    // 「1 玩法/关卡/0 书桌/电视」の三つ目を章の名前にする。
    for (const auto& o : folders.data.value("objects", Json::array())) {
        const auto id = ObjectId(o.value("objectId", Json{}));
        const auto folder = o.value("folder", "");
        std::vector<std::string> parts;
        for (size_t start = 0; start <= folder.size();) {
            const auto slash = folder.find('/', start);
            parts.push_back(folder.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
            if (slash == std::string::npos) { break; }
            start = slash + 1;
        }
        if (parts.size() >= 3 && parts[1] == "关卡") { chapterOf_[id] = parts[2]; }
    }
}

void LevelEditor::DrawOverview(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    RefreshChapters(ctx);
    // 刚才的试玩：解法として残すと、以後の修正で通れなくなった時に分かる。
    if (lastRecord_) {
        double seconds = 0;
        for (const auto& f : lastRecord_->frames) { seconds += f.seconds; }
        ui.Text("刚才的试玩：" + ShortChoiceLabel(RoomName(lastRecord_->entryRegion), 8) + " → " + ShortChoiceLabel(RoomName(lastRecord_->endRegion), 8) + "，" + std::to_string(static_cast<int>(seconds)) + " 秒");
        if (ui.Button("td2.recording.save", "存为解法")) { SaveRecording(ctx); }
        ui.SameLine(); if (ui.Button("td2.recording.drop", "不要")) { lastRecord_.reset(); }
        ui.Separator();
    }
    if (!issues_.empty()) {
        ui.Text("问题（" + std::to_string(issues_.size()) + "）");
        for (size_t i = 0; i < issues_.size() && i < 12; ++i) {
            const auto& issue = issues_[i];
            if (ui.Selectable("td2.issue." + std::to_string(i), (issue.level == IssueLevel::Error ? "⚠ " : "△ ") + ShortChoiceLabel(issue.text, 26))) { JumpToIssue(ctx, i); return; }
        }
        ui.Separator();
    }
    if (!solutions_.empty()) {
        ui.Text("解法");
        for (size_t i = 0; i < solutions_.size(); ++i) {
            const auto& s = solutions_[i];
            if (ui.Selectable("td2.solution." + std::to_string(i), (s.passed ? "✓ " : "✕ ") + ShortChoiceLabel(s.name, 18) + (s.passed ? "" : "  " + ShortChoiceLabel(s.detail, 20)), pathIndex_ == static_cast<int>(i))) {
                pathIndex_ = pathIndex_ == static_cast<int>(i) ? -1 : static_cast<int>(i);
            }
            if (pathIndex_ == static_cast<int>(i)) {
                if (confirmSolution_ != static_cast<int>(i)) { if (ui.Button("td2.solution.delete." + std::to_string(i), "删除这个解法")) { confirmSolution_ = static_cast<int>(i); } }
                else if (ui.Button("td2.solution.confirm." + std::to_string(i), "确认删除")) {
                    std::error_code error;
                    std::filesystem::remove(std::filesystem::path(std::u8string(s.file.begin(), s.file.end())), error);
                    confirmSolution_ = pathIndex_ = -1;
                    RunSolutions(ctx);
                    return;
                }
            }
        }
        ui.Separator();
    }
    EditText(ui, "td2.overview.search", "查找", search_);
    // 章ごとにテレビを並べる。世界にまだ置いていないテレビは小さな地図で出し、地図へドラッグして置く。
    std::map<std::string, std::vector<const RoomItem*>> chapters;
    for (const auto& room : model_.rooms) {
        if (!search_.empty() && room.object.name.find(search_) == std::string::npos && room.settings.region.regionId.find(search_) == std::string::npos) { continue; }
        const auto found = chapterOf_.find(room.object.objectId);
        chapters[found == chapterOf_.end() ? "其他" : found->second].push_back(&room);
    }
    for (const auto& [chapter, rooms] : chapters) {
        ui.Text(chapter + "（" + std::to_string(rooms.size()) + "）");
        for (const auto* room : rooms) {
            const auto id = room->object.objectId;
            if (room->settings.worldPlaced) {
                if (ui.Selectable("td2.overview.room." + std::to_string(id), "   " + ShortChoiceLabel(room->object.name, 16), id == model_.selected)) {
                    if (SelectRoom(ctx, id)) { focus_ = Focus::Room; FocusRoom(id); }
                }
                continue;
            }
            ui.Text("   ○ " + ShortChoiceLabel(room->object.name, 16) + "（拖进地图）");
            const auto input = ui.BeginCanvas("td2.overview.thumbnail." + std::to_string(id), 54);
            TrackDrag(input);
            MapCanvasView thumbnail;
            const auto& s = room->settings;
            thumbnail.tile = (std::max)(.5f, (std::min)((input.size.x - 12) / static_cast<float>(s.region.cols), (input.size.y - 8) / static_cast<float>(s.region.rows)));
            thumbnail.origin = {(input.size.x - s.region.cols * thumbnail.tile) * .5f, (input.size.y - s.region.rows * thumbnail.tile) * .5f};
            DrawRoomMap(ui, s, thumbnail, false, false, false);
            if (input.hovered && (input.pressedButtons & kPrimary) && drag_ == Drag::None && SelectRoom(ctx, id)) {
                drag_ = Drag::Library; model_.gesture = true; focus_ = Focus::Room;
            }
            ui.EndCanvas();
        }
    }
}

// ---------------------------------------------------------------- テレビ
void LevelEditor::DrawRoomInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    const auto* room = model_.Current();
    if (!room) { return; }
    auto& s = model_.draft;
    const auto before = s;
    ui.Text(ShortChoiceLabel(room->object.name, 18) + "  " + std::to_string(s.region.cols) + "×" + std::to_string(s.region.rows));
    if (s.worldPlaced) {
        if (ui.Button("td2.room.play", "▶ 从这屏开始")) { if (model_.Commit(ctx)) { pickingStart_ = true; FitRoom(s); } }
        ui.SameLine();
        if (ui.Button("td2.room.fit", "看这屏")) { FitRoom(s); }
    } else { ui.Text("还没放进世界：从左边列表拖进地图"); }
    ui.Checkbox("td2.room.entry", "开始电视", &s.entryRegion);
    const auto previous = s.region;
    EditInt(ui, "td2.room.cols", "列", s.region.cols, 1, 64);
    EditInt(ui, "td2.room.rows", "行", s.region.rows, 1, 64);
    if (!ResizeCells(previous, s.region)) { s.region = previous; model_.error = "尺寸变化失败"; }
    if (s != before) { propertyPending_ = true; }
    ui.Separator();
    DrawRoomStage(ctx, *room);
    ui.Separator();
    DrawRoomCamera(ctx, s.region.regionId);
    ui.Separator();
    if (ui.Checkbox("td2.room.advanced", "高级", &screenSettings_) && screenSettings_) { RefreshScreens(ctx); }
    if (screenSettings_) { DrawScreenBinding(ctx); }
    if (s.worldPlaced && ui.Button("td2.room.remove", "移出世界")) { s.worldPlaced = false; model_.Commit(ctx); }
}

void LevelEditor::DrawScreenBinding(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    auto& s = model_.draft;
    const auto before = s;
    ui.Text("区域 ID：" + s.region.regionId);
    double brightness = s.screenBrightness;
    if (EditFloat(ui, "td2.room.brightness", "屏幕亮度", brightness, .5f, 4.0f)) { s.screenBrightness = static_cast<float>(brightness); }
    const auto current = std::find_if(screens_.begin(), screens_.end(), [&s](const auto& item) { return item.objectId == s.screenObject; });
    ui.Text("屏幕对象：" + (s.screenObject == 0 ? std::string("未绑定") : current == screens_.end() ? std::to_string(s.screenObject) : ShortChoiceLabel(current->name)));
    if (ui.Button("td2.room.screen.pick", screenPicker_ ? "收起" : "换屏幕对象")) { screenPicker_ = !screenPicker_; }
    if (screenPicker_) {
        EditText(ui, "td2.room.screen.search", "查找", screenSearch_);
        if (ui.Checkbox("td2.room.screen.all", "包括无模型对象", &allScreens_)) { RefreshScreens(ctx); }
        std::vector<EditorChoice> choices{{"td2.room.screen.none", "未绑定", s.screenObject == 0}};
        std::vector<uint64_t> ids{0};
        for (const auto& screen : screens_) {
            if (!screenSearch_.empty() && screen.name.find(screenSearch_) == std::string::npos) { continue; }
            choices.push_back({"td2.room.screen." + std::to_string(screen.objectId), screen.name, s.screenObject == screen.objectId}); ids.push_back(screen.objectId);
        }
        if (const int chosen = DrawChoices(ui, choices); chosen >= 0) { s.screenObject = ids[static_cast<size_t>(chosen)]; screenPicker_ = false; }
    }
    EditText(ui, "td2.room.screen.slot", "材质槽", s.screenSlot);
    if (!model_.screenWarning.empty()) { ui.Text(model_.screenWarning); }
    if (s != before) { propertyPending_ = true; }
}

void LevelEditor::RefreshScreens(EditorWorkspaceHostContext& ctx) {
    screens_.clear();
    (void)QueryAll(ctx, "scene.objects.query", {{"detail", true}}, "objects", [this](const Json& object) {
        const auto id = ObjectId(object.value("objectId", Json{}));
        const bool model = object.contains("model") && object["model"].is_object() &&
            (!object["model"].value("assetId", "").empty() || object["model"].value("materialSlotCount", 0) > 0);
        if (id && (allScreens_ || id == model_.draft.screenObject || model)) { screens_.push_back({id, object.value("name", "对象 " + std::to_string(id))}); }
        return true;
    }, model_.error);
}

void LevelEditor::DrawRoomCamera(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
    auto& ui = *ctx.ui;
    EnsureLayout(ctx);
    const auto* room = layoutValid_ ? layout_.Room(regionId) : nullptr;
    std::string state = "机位";
    if (room) {
        state += !room->cameraId ? "：还没有" : room->cameraLocked ? "：已锁定" : room->cameraAuto.empty() ? "：作者放置" : CameraEdited(*room) ? "：已手调" : "：自动";
        if (const auto* area = layout_.AreaOf(regionId)) { state += " · " + ShortChoiceLabel(area->name, 10); }
    }
    ui.Text(state);
    if (ui.Button("td2.room.camera.view", "预览机位")) {
        if (model_.Commit(ctx)) { const auto* item = model_.Current(); OpenRegionCamera(ctx, regionId, item ? item->object.name : regionId, model_.error); layoutStale_ = true; }
    }
    ui.SameLine();
    // 生成の後は layout_ が作り直されて room が指す先が消えるので、すぐ戻る。
    if (ui.Button("td2.room.camera.generate", room && room->cameraId ? "按位置重新生成" : "生成机位")) { Generate(ctx, true, false, {}, regionId, nullptr, false); return; }
    if (room && room->cameraId && (room->cameraLocked || CameraEdited(*room))) {
        if (ui.Button("td2.room.camera.reset", "恢复自动机位")) { Generate(ctx, true, false, {}, regionId, nullptr, true); return; }
    }
}

// ---------------------------------------------------------------- 装置
void LevelEditor::DrawDeviceInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    auto* d = model_.devices.Selected();
    if (!d) { return; }
    auto& s = model_.draft;
    const auto before = s;
    ui.Text(DeviceName(*d));
    if (ui.Button("td2.device.back", "← 这台电视")) { model_.devices.Select(-1); focus_ = Focus::Room; return; }
    ui.SameLine(); if (ui.Button("td2.device.duplicate", "复制")) { model_.devices.DuplicateSelected(s.region.cols, s.region.rows); }
    ui.SameLine(); if (ui.Button("td2.device.delete", "删除")) { model_.devices.RemoveSelected(); s.devices = model_.devices.Serialize(); model_.Commit(ctx); focus_ = Focus::Room; return; }
    d = model_.devices.Selected();
    if (!d) { return; }
    if (d->kind == "Laser" || d->kind == "Spikes" || d->kind == "OneWay") {
        constexpr std::array<std::string_view, 4> directions{"Up", "Right", "Down", "Left"};
        constexpr std::array<std::string_view, 4> names{"↑", "→", "↓", "←"};
        for (size_t i = 0; i < directions.size(); ++i) {
            if (i) { ui.SameLine(); }
            if (ui.Button("td2.device.direction." + std::to_string(i), d->direction == directions[i] ? std::string(names[i]) + " ●" : std::string(names[i]))) { d->direction = directions[i]; }
        }
    }
    if (d->kind == "Laser") { EditFloat(ui, "td2.device.range", "射程（格）", d->range, .125f, 128); }
    if (d->kind == "Spring") { DrawSpring(ui, *d); }
    if (d->kind == "Decal") { DrawDecal(ui, *d); }
    if (d->kind == "Laser" || d->kind == "Door" || d->kind == "Bridge") { ui.Checkbox("td2.device.invert", "通电时关闭（反向）", &d->inverted); }
    if (d->kind == "Plate") {
        if (ui.Selectable("td2.device.latch.off", "踩住才通电", !d->latch)) { d->latch = false; }
        if (ui.Selectable("td2.device.latch.on", "踩一下切换", d->latch)) { d->latch = true; }
        bool reset = d->Extra("variant") == "reset";
        if (ui.Checkbox("td2.device.reset", "圆形复位钮外观", &reset)) { d->SetExtra("variant", reset ? "reset" : ""); }
        s.devices = model_.devices.Serialize();
        if (s != before) { propertyPending_ = true; RememberTemplate(*d); }
        DrawPlateLinks(ctx);
        return;
    }
    ui.Checkbox("td2.device.advanced", "位置与大小", &deviceAdvanced_);
    if (deviceAdvanced_) {
        EditFloat(ui, "td2.device.x", "X", d->x, 0, static_cast<float>((std::max)(0.0, s.region.cols - d->w)));
        EditFloat(ui, "td2.device.y", "Y", d->y, 0, static_cast<float>((std::max)(0.0, s.region.rows - d->h)));
        EditFloat(ui, "td2.device.w", "宽", d->w, .125f, static_cast<float>(s.region.cols - d->x));
        EditFloat(ui, "td2.device.h", "高", d->h, .125f, static_cast<float>(s.region.rows - d->y));
    }
    s.devices = model_.devices.Serialize();
    if (s != before) { propertyPending_ = true; RememberTemplate(*d); }
}

// 圧力板のつながり：今の相手の一覧（外せる）。つなぐのは地図の「连线」で板から相手へ引く。
void LevelEditor::DrawPlateLinks(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    RefreshMachines(ctx);
    const auto* d = model_.devices.Selected();
    if (!d) { return; }
    ui.Separator();
    ui.Text(d->targets.empty() ? "还没连接：用「连线」从这块板拖到门、激光、桥或机构" : "连接");
    const std::string region = model_.draft.region.regionId;
    std::string removing;
    for (const auto& target : d->targets) {
        std::string label = target;
        bool found = false;
        const auto kind = ClassifyTarget(target);
        if (kind == TargetKind::Local) {
            for (const auto& t : model_.devices.Items()) { if (t.id == target) { label = DeviceName(t); found = true; } }
        } else if (kind == TargetKind::OtherRegion) {
            const auto slash = target.find('/');
            for (const auto& room : model_.rooms) {
                if (room.settings.region.regionId != target.substr(0, slash)) { continue; }
                DeviceEditor other; other.Load(room.settings.devices);
                for (const auto& t : other.Items()) { if (t.id == target.substr(slash + 1)) { label = ShortChoiceLabel(room.object.name, 8) + " · " + DeviceName(t); found = true; } }
            }
        } else if (kind == TargetKind::Machine) {
            for (const auto& m : model_.machines) for (const auto& [receiver, text] : m.Receivers()) { if (receiver == target) { label = text; found = true; } }
        }
        ui.Text((found ? "→ " : "✕ 找不到 ") + ShortChoiceLabel(label, 22));
        ui.SameLine();
        if (ui.Button("td2.device.unlink." + target, "断开")) { removing = target; }
    }
    if (!removing.empty()) { TogglePlateTarget(ctx, region + "/" + d->id, removing); }
}

// ---------------------------------------------------------------- 3D の物体（布景で選んだテレビ以外の物）
void LevelEditor::RefreshStageObject(EditorWorkspaceHostContext& ctx) {
    stageClock_ += ctx.unscaledDeltaSeconds;
    const uint64_t id = model_.sceneSelection;
    // 選んだ物が替わったか、シーンの目印が変わった時（ギズモで動かした等）だけ読む。
    if (stagePending_ || (id == stageObject_ && stagePulse_ == model_.Pulses() && stageClock_ < 5.f)) { return; }
    const bool changed = id != stageObject_;
    stageClock_ = 0; stageObject_ = id; stageReadOk_ = false; stagePulse_ = model_.Pulses();
    std::string error;
    if (id) { stageReadOk_ = ReadStageObject(ctx, id, stageRead_, error); }
    // 布景の道具でテレビ以外を選んだら、その物体の属性を出す。
    if (changed && stageReadOk_ && tool_ == Tool::Stage) {
        const bool room = std::any_of(model_.rooms.begin(), model_.rooms.end(), [id](const RoomItem& r) { return MapModel::TelevisionObject(r) == id; });
        if (!room) { focus_ = Focus::Object; }
    }
}

// 選んだ物体の位置・向き・縮尺。動かすのは主にビューポートのギズモ（W / E / R）で、ここは数値と一押しの向き直しだけ。
static bool DrawPoseEditor(IEditorWorkspaceUi& ui, Pose& pose, const StageObject& read) {
    bool changed = false;
    changed |= EditFloat(ui, "td2.stage.x", "X（米）", pose.position[0], -1000, 1000);
    changed |= EditFloat(ui, "td2.stage.y", "高度（米）", pose.position[1], -1000, 1000);
    changed |= EditFloat(ui, "td2.stage.z", "Z（米）", pose.position[2], -1000, 1000);
    changed |= EditFloat(ui, "td2.stage.yaw", "朝向（度）", pose.rotation[1], -360, 360);
    const bool uniform = std::abs(pose.scale[0] - pose.scale[1]) < 1e-4 && std::abs(pose.scale[0] - pose.scale[2]) < 1e-4;
    if (uniform) {
        double s = pose.scale[0];
        if (EditFloat(ui, "td2.stage.scale", "缩放", s, .05f, 20)) { pose.scale = {s, s, s}; changed = true; }
    }
    constexpr std::array<std::pair<double, std::string_view>, 4> turns{{{-90, "↺ 90°"}, {-15, "↺ 15°"}, {15, "↻ 15°"}, {90, "↻ 90°"}}};
    for (size_t i = 0; i < turns.size(); ++i) {
        if (i) { ui.SameLine(); }
        if (ui.Button("td2.stage.turn." + std::to_string(i), turns[i].second)) { pose.rotation[1] = std::fmod(pose.rotation[1] + turns[i].first + 540.0, 360.0) - 180; changed = true; }
    }
    if (ui.Button("td2.stage.floor", "落到地面") && read.hasBounds) { pose.position[1] -= read.lo[1]; changed = true; }
    return changed;
}

void LevelEditor::DrawObjectInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (!stageObject_ || !stageReadOk_) { ui.Text("在视口里点选一个物体"); return; }
    ui.Text(ShortChoiceLabel(stageRead_.name, 20));
    if (stageRead_.television) {
        ui.Text(std::string(stageRead_.television->label) + "（还不是关卡电视）");
        if (ui.Button("td2.stage.adopt", "建成关卡电视")) {
            const auto model3d = *stageRead_.television;
            if (model_.AdoptTelevision(ctx, stageObject_, model3d, std::nullopt)) { focus_ = Focus::Room; SetNote("已建成，从左边列表拖进地图"); }
            return;
        }
    }
    if (!stagePending_) { stageEditing_ = stageRead_.pose; }
    if (DrawPoseEditor(ui, stageEditing_, stageRead_)) { stagePending_ = true; }
    if (stagePending_ && !ui.GetInteraction().editing) {
        stagePending_ = false;
        if (SetStagePose(ctx, stageObject_, stageEditing_, model_.error)) { MarkSceneChanged(); }
        stageClock_ = 10;
    }
}

void LevelEditor::DrawRoomStage(EditorWorkspaceHostContext& ctx, const RoomItem& room) {
    auto& ui = *ctx.ui;
    const auto tv = MapModel::TelevisionObject(room);
    const auto& s = model_.draft;
    // 3D の付き方：地図の隣のテレビに付けてあるか、自由に置いたか。
    if (!s.stageAnchor.empty()) {
        ui.Text("3D：贴在「" + ShortChoiceLabel(RoomName(s.stageAnchor), 10) + "」旁");
        ui.SameLine();
        if (ui.Button("td2.room.stage.free", "解开")) { model_.draft.stageAnchor.clear(); model_.Commit(ctx); }
    } else { ui.Text("3D：自由摆放"); }
    if (s.worldPlaced) {
        const auto here = Placement(s);
        for (const auto& other : model_.rooms) {
            if (other.object.objectId == room.object.objectId || !other.settings.worldPlaced) { continue; }
            const auto edge = FindSharedWorldEdge(Placement(other.settings), here);
            if (!edge) { continue; }
            const std::string where = edge->sideA == WorldSide::Right ? "右边" : edge->sideA == WorldSide::Left ? "左边" : edge->sideA == WorldSide::Top ? "上面" : "下面";
            if (ui.Button("td2.room.stage.attach." + std::to_string(other.object.objectId), "贴到「" + ShortChoiceLabel(other.object.name, 14) + "」" + where)) {
                const auto region = s.region.regionId, anchor = other.settings.region.regionId;
                AttachRoom(ctx, region, anchor);
                return;
            }
        }
    }
    // テレビの姿勢（数値）は畳んでおく。
    ui.Checkbox("td2.room.stage.pose", "3D 位置", &stagePoseOpen_);
    if (!stagePoseOpen_) { return; }
    if (tv != stageObject_ || !stageReadOk_) { ui.Text("读取中…"); return; }
    if (!stagePending_) { stageEditing_ = stageRead_.pose; }
    if (DrawPoseEditor(ui, stageEditing_, stageRead_)) { stagePending_ = true; }
    if (stagePending_ && !ui.GetInteraction().editing) {
        stagePending_ = false;
        if (SetStagePose(ctx, tv, stageEditing_, model_.error)) { MarkSceneChanged(); }
        stageClock_ = 10;
    }
}
}
