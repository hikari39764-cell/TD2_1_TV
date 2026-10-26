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
    if (EditFloat(ui, "td2.device.power", "バネの強さ", power, 3, 44)) { d.SetExtra("power", std::abs(power - 22.5) < 1e-6 ? "" : ShortNumber(power)); }
    ui.Text("約 " + ShortNumber(power * power / 87.5) + " マス跳ぶ");
    if (EditFloat(ui, "td2.device.delay", "乗ってから跳ぶまでの秒数", delay, .05f, 3)) { d.SetExtra("delay", std::abs(delay - .85) < 1e-6 ? "" : ShortNumber(delay)); }
    if (EditFloat(ui, "td2.device.cooldown", "次に使えるまでの秒数", cooldown, 3, 30)) { d.SetExtra("cooldown", std::abs(cooldown - 3) < 1e-6 ? "" : ShortNumber(cooldown)); }
}

// 押し刺：伸びるマス数と秒数、動き方（板で伸びる／往復）。往復は端で止まる秒数と始まりのずれ。
void DrawPusher(IEditorWorkspaceUi& ui, DeviceDraft& d) {
    const auto read = [&d](std::string_view key, double fallback) {
        const auto text = d.Extra(key);
        try { return text.empty() ? fallback : std::stod(text); } catch (...) { return fallback; }
    };
    double travel = read("travel", 3), seconds = read("seconds", .5), dwell = read("dwell", 0), phase = read("phase", 0);
    if (EditFloat(ui, "td2.device.travel", "伸びるマス数", travel, .25f, 30)) { d.SetExtra("travel", std::abs(travel - 3) < 1e-6 ? "" : ShortNumber(travel)); }
    if (EditFloat(ui, "td2.device.seconds", "伸びる秒数", seconds, .05f, 10)) { d.SetExtra("seconds", std::abs(seconds - .5) < 1e-6 ? "" : ShortNumber(seconds)); }
    const bool timed = dwell > 0;
    if (ui.Selectable("td2.device.pusher.plate", "板で伸びる", !timed)) { d.SetExtra("dwell", ""); d.SetExtra("phase", ""); }
    if (ui.Selectable("td2.device.pusher.timed", "往復する", timed) && !timed) { d.SetExtra("dwell", "1"); }
    if (!timed) { return; }
    if (EditFloat(ui, "td2.device.dwell", "端で止まる秒数", dwell, .05f, 30)) { d.SetExtra("dwell", ShortNumber(dwell)); }
    if (EditFloat(ui, "td2.device.phase", "始まりのずれ", phase, 0, 1)) { d.SetExtra("phase", phase < 1e-6 ? "" : ShortNumber(phase)); }
}

// 貼り絵の図案：キーの説明・出口の矢印・前景（実体の地形の時に手前に出る暗い塊と垂れた線）。
void DrawDecal(IEditorWorkspaceUi& ui, DeviceDraft& d) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 9> variants{{{"ad", "A / D"}, {"space", "スペース"}, {"spaceHold", "スペース長押し"},
        {"exit", "出口 →"}, {"exitLeft", "出口 ←"}, {"exitUp", "出口 ↑"}, {"exitDown", "出口 ↓"}, {"fore", "手前の塊"}, {"foreCable", "手前のケーブル"}}};
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
    if (EditFloat(ui, "td2.device.depth", "人物からの距離", depth, .25f, 6)) { d.SetExtra("depth", std::abs(depth - 1.0) < 1e-6 ? "" : ShortNumber(depth)); }
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
    // 今の試遊：解法として残すと、以後の修正で通れなくなった時に分かる。
    if (lastRecord_) {
        double seconds = 0;
        for (const auto& f : lastRecord_->frames) { seconds += f.seconds; }
        ui.Text("今の試遊：" + ShortChoiceLabel(RoomName(lastRecord_->entryRegion), 8) + " → " + ShortChoiceLabel(RoomName(lastRecord_->endRegion), 8) + "、" + std::to_string(static_cast<int>(seconds)) + " 秒");
        if (ui.Button("td2.recording.save", "解法として保存")) { SaveRecording(ctx); }
        ui.SameLine(); if (ui.Button("td2.recording.drop", "捨てる")) { lastRecord_.reset(); }
        ui.Separator();
    }
    if (!issues_.empty()) {
        ui.Text("問題（" + std::to_string(issues_.size()) + "）");
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
                if (confirmSolution_ != static_cast<int>(i)) { if (ui.Button("td2.solution.delete." + std::to_string(i), "この解法を削除")) { confirmSolution_ = static_cast<int>(i); } }
                else if (ui.Button("td2.solution.confirm." + std::to_string(i), "削除する")) {
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
    if (!lastRecord_ && issues_.empty() && solutions_.empty()) { ui.Text("地図でテレビを選ぶと、ここに設定が出ます"); }
}

// ---------------------------------------------------------------- テレビ
void LevelEditor::DrawRoomInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    const auto* room = model_.Current();
    if (!room) { return; }
    auto& s = model_.draft;
    const auto before = s;
    // いつも出すのは名前・大きさ・試遊・開始テレビ・向き・カメラだけ。3D の付き方と画面は「詳細」に畳む。
    ui.Text(ShortChoiceLabel(room->object.name, 18) + "  " + std::to_string(s.region.cols) + "×" + std::to_string(s.region.rows));
    if (s.worldPlaced) {
        if (ui.Button("td2.room.play", "▶ この画面から")) { if (model_.Commit(ctx)) { pickingStart_ = true; FitRoom(s); } }
    } else { ui.Text("地図の右の「未配置」から地図へドラッグ"); }
    ui.Checkbox("td2.room.entry", "開始テレビ", &s.entryRegion);
    const auto previous = s.region;
    EditInt(ui, "td2.room.cols", "列", s.region.cols, 1, 64);
    EditInt(ui, "td2.room.rows", "行", s.region.rows, 1, 64);
    if (!ResizeCells(previous, s.region)) { s.region = previous; model_.error = "大きさを変えられません"; }
    if (s != before) { propertyPending_ = true; }
    ui.Separator();
    const auto regionId = s.region.regionId;
    DrawRoomFacing(ctx, *room);
    DrawRoomCamera(ctx, regionId);
    if (model_.Current() != room) { return; }   // 裏返し・カメラの作り直しで区域を読み直した
    ui.Separator();
    ui.Checkbox("td2.room.advanced", "詳細", &screenSettings_);
    if (!screenSettings_) { return; }
    DrawRoomStage(ctx, *room);
    ui.Separator();
    DrawScreenBinding(ctx);
    if (s.worldPlaced && ui.Button("td2.room.remove", "世界から外す")) { s.worldPlaced = false; model_.Commit(ctx); }
}

// 画面の位置と材質スロットはテレビの型で決まるので、ここは ID と明るさだけ。
void LevelEditor::DrawScreenBinding(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    auto& s = model_.draft;
    ui.Text("区域 ID：" + s.region.regionId);
    double brightness = s.screenBrightness;
    if (EditFloat(ui, "td2.room.brightness", "画面の明るさ", brightness, .5f, 4.0f)) { s.screenBrightness = static_cast<float>(brightness); propertyPending_ = true; }
}

// カメラ：世界に置いたテレビには、初めて置いた時に自動のカメラが付く。「選ぶ」でそのカメラを選び、ビューポートの
// 「カメラを操作」で動かすと、その画がこの区域のカメラになる（作者が動かしたカメラは作り直しても残る）。
void LevelEditor::DrawRoomCamera(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
    auto& ui = *ctx.ui;
    EnsureLayout(ctx);
    const auto* room = layoutValid_ ? layout_.Room(regionId) : nullptr;
    if (!room || !room->placed) { return; }
    if (!room->cameraId) {
        ui.Text("カメラ：なし");
        ui.SameLine();
        if (ui.Button("td2.room.camera.generate", "作る")) { Generate(ctx, true, false, {}, regionId, nullptr, false); }
        return;
    }
    const bool edited = room->cameraLocked || CameraEdited(*room);
    std::string state = std::string("カメラ：") + (room->cameraLocked ? "固定" : room->cameraAuto.empty() ? "手置き" : edited ? "手直し" : "自動");
    if (const auto* area = layout_.AreaOf(regionId)) { state += " · " + ShortChoiceLabel(area->name, 8); }
    ui.Text(state);
    const uint64_t camera = room->cameraId;
    if (ui.Button("td2.room.camera.select", "選ぶ")) {
        if (Execute(ctx, "scene.object.select", {{"objectId", camera}}).ok) {
            model_.sceneSelection = camera;
            SetNote("ビューポートの上の「カメラを操作」で動かすと、その画がこのテレビのカメラになります");
        }
    }
    ui.SameLine();
    // 生成の後は layout_ が作り直されて room が指す先が消えるので、すぐ戻る。
    if (edited) { if (ui.Button("td2.room.camera.reset", "自動に戻す")) { Generate(ctx, true, false, {}, regionId, nullptr, true); } }
    else if (ui.Button("td2.room.camera.generate", "作り直す")) { Generate(ctx, true, false, {}, regionId, nullptr, false); }
}

void LevelEditor::DrawRoomFacing(EditorWorkspaceHostContext& ctx, const RoomItem& room) {
    auto& ui = *ctx.ui;
    EnsureLayout(ctx);
    if (!layoutValid_ || !layout_.Room(room.settings.region.regionId)) { return; }
    const bool back = BackFacing(room);
    const auto regionId = room.settings.region.regionId;
    ui.Text("向き");
    ui.SameLine();
    if (ui.Button("td2.room.facing.front", back ? "表" : "● 表") && back) { FlipRoom(ctx, regionId); return; }
    ui.SameLine();
    if (ui.Button("td2.room.facing.back", back ? "● 裏" : "裏") && !back) { FlipRoom(ctx, regionId); return; }
}

void LevelEditor::FlipRoom(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
    if (!model_.Commit(ctx) || !EnsureLayout(ctx, true)) { return; }
    if (!FlipTelevision(ctx, layout_, regionId, model_.error)) { return; }
    // 3D で隣に付けていたテレビは、裏返すと付き方が変わるので自由配置にする。
    if (const auto* item = model_.Current(); item && item->settings.region.regionId == regionId && !model_.draft.stageAnchor.empty()) {
        model_.draft.stageAnchor.clear(); model_.Commit(ctx);
    }
    ++layoutVersion_; MarkSceneChanged(); followResync_ = true;
    SetNote("3D でテレビとカメラを裏返しました");
}

// ---------------------------------------------------------------- 装置
void LevelEditor::DrawDeviceInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    auto* d = model_.devices.Selected();
    if (!d) { return; }
    auto& s = model_.draft;
    const auto before = s;
    ui.Text(DeviceName(*d));
    if (ui.Button("td2.device.back", "← このテレビ")) { model_.devices.Select(-1); focus_ = Focus::Room; return; }
    ui.SameLine(); if (ui.Button("td2.device.duplicate", "複製")) { model_.devices.DuplicateSelected(s.region.cols, s.region.rows); }
    ui.SameLine(); if (ui.Button("td2.device.delete", "削除")) { model_.devices.RemoveSelected(); s.devices = model_.devices.Serialize(); model_.Commit(ctx); focus_ = Focus::Room; return; }
    d = model_.devices.Selected();
    if (!d) { return; }
    // 一方通行・バネ・板は上向きか天井向き（↓）だけ。
    if (d->kind == "Laser" || d->kind == "Spikes" || d->kind == "OneWay" || d->kind == "Spring" || d->kind == "Plate" || d->kind == "Pusher") {
        constexpr std::array<std::string_view, 4> directions{"Up", "Right", "Down", "Left"};
        constexpr std::array<std::string_view, 4> names{"↑", "→", "↓", "←"};
        const bool vertical = d->kind == "OneWay" || d->kind == "Spring" || d->kind == "Plate";
        for (size_t i = 0; i < directions.size(); ++i) {
            if (vertical && (i % 2) == 1) { continue; }
            if (i) { ui.SameLine(); }
            if (ui.Button("td2.device.direction." + std::to_string(i), d->direction == directions[i] ? std::string(names[i]) + " ●" : std::string(names[i]))) { d->direction = directions[i]; }
        }
    }
    if (d->kind == "Laser") {
        EditFloat(ui, "td2.device.range", "射程", d->range, .125f, 128);
        // 並べて光の帘にする時は太くする（細い光の隙間は体がすり抜けられる）。
        bool wide = !d->Extra("width").empty();
        if (ui.Checkbox("td2.device.wide", "太い光", &wide)) { d->SetExtra("width", wide ? "0.4" : ""); }
    }
    if (d->kind == "Spring") { DrawSpring(ui, *d); }
    if (d->kind == "Pusher") { DrawPusher(ui, *d); }
    if (d->kind == "Decal") { DrawDecal(ui, *d); }
    if (d->kind == "Laser" || d->kind == "Door" || d->kind == "Bridge" || d->kind == "Goal" || (d->kind == "Pusher" && d->Extra("dwell").empty())) {
        ui.Checkbox("td2.device.invert", "通電で閉じる（逆）", &d->inverted);
        // つながる板が二枚以上ある時の動き方（全部で動けば、二か所を同時に押さえる謎になる）。
        bool all = d->Extra("all") == "1";
        if (ui.Checkbox("td2.device.all", "つながる板の全部で動く", &all)) { d->SetExtra("all", all ? "1" : ""); }
    }
    if (d->kind == "Plate") {
        if (ui.Selectable("td2.device.latch.off", "踏んでいる間だけ通電", !d->latch)) { d->latch = false; }
        if (ui.Selectable("td2.device.latch.on", "踏むたびに切り替え", d->latch)) { d->latch = true; }
        bool reset = d->Extra("variant") == "reset";
        if (ui.Checkbox("td2.device.reset", "丸いリセットボタンの見た目", &reset)) { d->SetExtra("variant", reset ? "reset" : ""); }
        s.devices = model_.devices.Serialize();
        if (s != before) { propertyPending_ = true; RememberTemplate(*d); }
        DrawPlateLinks(ctx);
        return;
    }
    ui.Checkbox("td2.device.advanced", "位置と大きさ", &deviceAdvanced_);
    if (deviceAdvanced_) {
        EditFloat(ui, "td2.device.x", "X", d->x, 0, static_cast<float>((std::max)(0.0, s.region.cols - d->w)));
        EditFloat(ui, "td2.device.y", "Y", d->y, 0, static_cast<float>((std::max)(0.0, s.region.rows - d->h)));
        EditFloat(ui, "td2.device.w", "幅", d->w, .125f, static_cast<float>(s.region.cols - d->x));
        EditFloat(ui, "td2.device.h", "高さ", d->h, .125f, static_cast<float>(s.region.rows - d->y));
    }
    s.devices = model_.devices.Serialize();
    if (s != before) { propertyPending_ = true; RememberTemplate(*d); }
}

// 圧力板のつながり：今の相手の一覧（外せる）。つなぐのは地図の「配線」で板から相手へ引く。
void LevelEditor::DrawPlateLinks(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    RefreshMachines(ctx);
    const auto* d = model_.devices.Selected();
    if (!d) { return; }
    ui.Separator();
    ui.Text(d->targets.empty() ? "まだつないでいません：「配線」でこの板から扉・レーザー・橋・ゴール・押し刺・仕掛けへドラッグ" : "つなぎ先");
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
        ui.Text((found ? "→ " : "✕ 見つからない ") + ShortChoiceLabel(label, 22));
        ui.SameLine();
        if (ui.Button("td2.device.unlink." + target, "外す")) { removing = target; }
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
        const bool room = std::any_of(model_.rooms.begin(), model_.rooms.end(), [id](const RoomItem& r) { return r.object.objectId == id; });
        if (!room) { focus_ = Focus::Object; }
    }
}

// 選んだ物体の位置・向き・縮尺。動かすのは主にビューポートのギズモ（W / E / R）で、ここは数値と一押しの向き直しだけ。
static bool DrawPoseEditor(IEditorWorkspaceUi& ui, Pose& pose, const StageObject& read) {
    bool changed = false;
    changed |= EditFloat(ui, "td2.stage.x", "X", pose.position[0], -1000, 1000);
    changed |= EditFloat(ui, "td2.stage.y", "高さ", pose.position[1], -1000, 1000);
    changed |= EditFloat(ui, "td2.stage.z", "Z", pose.position[2], -1000, 1000);
    changed |= EditFloat(ui, "td2.stage.yaw", "向き", pose.rotation[1], -360, 360);
    const bool uniform = std::abs(pose.scale[0] - pose.scale[1]) < 1e-4 && std::abs(pose.scale[0] - pose.scale[2]) < 1e-4;
    if (uniform) {
        double s = pose.scale[0];
        if (EditFloat(ui, "td2.stage.scale", "拡大", s, .05f, 20)) { pose.scale = {s, s, s}; changed = true; }
    }
    constexpr std::array<std::pair<double, std::string_view>, 4> turns{{{-90, "↺ 90°"}, {-15, "↺ 15°"}, {15, "↻ 15°"}, {90, "↻ 90°"}}};
    for (size_t i = 0; i < turns.size(); ++i) {
        if (i) { ui.SameLine(); }
        if (ui.Button("td2.stage.turn." + std::to_string(i), turns[i].second)) { pose.rotation[1] = std::fmod(pose.rotation[1] + turns[i].first + 540.0, 360.0) - 180; changed = true; }
    }
    if (ui.Button("td2.stage.floor", "床に下ろす") && read.hasBounds) { pose.position[1] -= read.lo[1]; changed = true; }
    return changed;
}

void LevelEditor::DrawObjectInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (!stageObject_ || !stageReadOk_) { ui.Text("ビューポートで物体を一つ選んでください"); return; }
    ui.Text(ShortChoiceLabel(stageRead_.name, 20));
    // テレビのモデルでも区域が無ければ飾り。インスペクターで区域を付けると、ここの一覧に出てくる。
    if (stageRead_.television) { ui.Text(std::string(stageRead_.television->label) + "（インスペクターで「TD2 テレビの区域」を付けるとレベルのテレビになる）"); }
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
    const auto tv = room.object.objectId;
    const auto& s = model_.draft;
    // 3D の付き方：地図の隣のテレビに付けてあるか、自由に置いたか。
    if (!s.stageAnchor.empty()) {
        ui.Text("3D：「" + ShortChoiceLabel(RoomName(s.stageAnchor), 10) + "」の隣");
        ui.SameLine();
        if (ui.Button("td2.room.stage.free", "外す")) { model_.draft.stageAnchor.clear(); model_.Commit(ctx); }
    } else { ui.Text("3D：自由配置"); }
    if (s.worldPlaced) {
        const auto here = Placement(s);
        for (const auto& other : model_.rooms) {
            if (other.object.objectId == room.object.objectId || !other.settings.worldPlaced) { continue; }
            const auto edge = FindSharedWorldEdge(Placement(other.settings), here);
            if (!edge) { continue; }
            const std::string where = edge->sideA == WorldSide::Right ? "右" : edge->sideA == WorldSide::Left ? "左" : edge->sideA == WorldSide::Top ? "上" : "下";
            if (ui.Button("td2.room.stage.attach." + std::to_string(other.object.objectId), "「" + ShortChoiceLabel(other.object.name, 14) + "」の" + where + "に付ける")) {
                const auto region = s.region.regionId, anchor = other.settings.region.regionId;
                AttachRoom(ctx, region, anchor);
                return;
            }
        }
    }
    // テレビの姿勢（数値）は畳んでおく。
    ui.Checkbox("td2.room.stage.pose", "3D の位置", &stagePoseOpen_);
    if (!stagePoseOpen_) { return; }
    if (tv != stageObject_ || !stageReadOk_) { ui.Text("読み込み中…"); return; }
    if (!stagePending_) { stageEditing_ = stageRead_.pose; }
    if (DrawPoseEditor(ui, stageEditing_, stageRead_)) { stagePending_ = true; }
    if (stagePending_ && !ui.GetInteraction().editing) {
        stagePending_ = false;
        if (SetStagePose(ctx, tv, stageEditing_, model_.error)) { MarkSceneChanged(); }
        stageClock_ = 10;
    }
}
}
