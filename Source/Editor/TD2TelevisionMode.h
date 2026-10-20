#pragma once
// レベル編集のテレビ地図：一台のテレビの地形と装置を描いて直す。
#include "TD2LevelModel.h"

namespace TD2::Editor::Level {
// テレビ単位の作者編集。実行時の状態は持たない。
class TelevisionMode final : public MapMode {
public:
    using MapMode::MapMode;
    void OnActivate(EditorWorkspaceHostContext& ctx) override { MapMode::OnActivate(ctx); drag_ = Drag::None; ChooseTool(0); view_.fit = true; }
    void OnDeactivate(EditorWorkspaceHostContext& ctx) override { MapMode::OnDeactivate(ctx); drag_ = Drag::None; linkSource_.clear(); }
    // 左：保存と履歴・今のテレビ・テレビの一覧（畳める）・選んだものの属性。
    void DrawSide(EditorWorkspaceHostContext& ctx) override {
        auto& ui = *ctx.ui;
        HistoryToolbar(ctx);
        if (const auto* room = model_->Current()) {
            ui.Text(ShortChoiceLabel(room->object.name, 16) + "  " + std::to_string(room->settings.region.cols) + "×" + std::to_string(room->settings.region.rows));
            ui.SameLine();
        }
        if (ui.Button("td2.television.gallery", gallery_ ? "收起列表" : "换一台电视")) { gallery_ = !gallery_; }
        if (gallery_ || !model_->ready) { DrawGallery(ctx); ui.Separator(); }
        DrawProperties(ctx);
    }
    // 右：見方の切り替え・道具と仕掛けの板・地図。置くものを選ぶのに欄を切り替えなくてよいよう、全部を地図の上に並べる。
    void DrawMap(EditorWorkspaceHostContext& ctx) override {
        ModeBar(ctx, kTelevisionView);
        // シーンでの選択などでテレビが変わったら、新しいテレビに合わせる。
        if (model_->selected != shown_) { shown_ = model_->selected; view_.fit = true; drag_ = Drag::None; ChooseTool(tool_ == 3 ? 3 : 0); }
        DrawPalette(ctx);
        DrawCanvas(ctx);
    }
private:
    enum class Drag { None, Paint, Move, Resize, Spawn, Box };
    void ChooseTool(int tool) { tool_ = tool; hand_ = false; linkSource_.clear(); groupIds_.clear(); }
    void ChooseKind(size_t index) { kind_ = kEditorDeviceKinds[index]; ChooseTool(3); model_->devices.Select(-1); }
    void DrawGallery(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        if (ui.Button("td2.television.add_selected", "选中电视建图")) { model_->AddSelectedTelevision(ctx); view_.fit = true; ChooseTool(0); }
        ui.SameLine();
        if (ui.Button("td2.television.new", "+ 空白地图")) { model_->Create(ctx); view_.fit = true; ChooseTool(0); }
        EditText(ui, "td2.television.gallery.search", "查找", gallerySearch_);
        const auto rooms = model_->rooms;
        for (size_t i = 0; i < rooms.size(); ++i) {
            const auto& room = rooms[i];
            if (!gallerySearch_.empty() && room.object.name.find(gallerySearch_) == std::string::npos &&
                room.settings.region.regionId.find(gallerySearch_) == std::string::npos) { continue; }
            ui.Text(ShortChoiceLabel(room.object.name) + " " + std::to_string(room.settings.region.cols) + "×" + std::to_string(room.settings.region.rows));
            const auto input = ui.BeginCanvas("td2.television.thumbnail." + std::to_string(room.object.objectId), 60);
            DrawThumbnail(ui, room, input);
            if (input.hovered && (input.pressedButtons & kPrimary)) {
                if (model_->Select(ctx, room.object.objectId)) { view_.fit = true; drag_ = Drag::None; gallery_ = false; ChooseTool(0); }
            }
            ui.EndCanvas();
        }
    }
    // 道具（選ぶ・地形・消す・連線・スポーン位置・範囲選択・貼り付け）と仕掛けの種類を一枚の板にする。仕掛けの種類を押すとそのまま置く道具になる。
    void DrawPalette(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        constexpr std::array<std::pair<int, std::string_view>, 7> tools{{{0, "↖ 选择"}, {1, "▧ 地形"}, {2, "⌫ 擦除"}, {4, "⌁ 连线"}, {5, "◉ 出生点"}, {6, "□ 框选"}, {7, "▣ 粘贴"}}};
        constexpr size_t columns = 4;
        if (ui.BeginTable("td2.television.tools", static_cast<uint32_t>(columns))) {
            for (size_t i = 0; i <= tools.size(); ++i) {
                if (i % columns == 0) { ui.TableNextRow(); }
                ui.TableNextColumn();
                if (i == tools.size()) { ViewControls(ctx); break; }
                const auto [tool, label] = tools[i];
                if (ui.Button("td2.television.tool." + std::to_string(tool), std::string(tool_ == tool ? "● " : "") + std::string(label))) { ChooseTool(tool); }
            }
            ui.EndTable();
        }
        ui.Text("机关");
        if (ui.BeginTable("td2.television.kinds", static_cast<uint32_t>(columns))) {
            for (size_t i = 0; i < kEditorDeviceKinds.size(); ++i) {
                if (i % columns == 0) { ui.TableNextRow(); }
                ui.TableNextColumn();
                const bool current = tool_ == 3 && kind_ == kEditorDeviceKinds[i];
                if (ui.Button("td2.television.kind." + std::to_string(i), std::string(current ? "● " : "") + std::string(kEditorDeviceLabels[i]))) { ChooseKind(i); }
            }
            ui.EndTable();
        }
        // 道具ごとの補助（筆の太さ・範囲選択のコピー）と一行の説明。
        if (tool_ == 1 || tool_ == 2) {
            EditInt(ui, "td2.television.brush.size", "笔刷", brushSize_, 1, 4);
            ui.SameLine(); ui.Checkbox("td2.television.brush.rectangle", "矩形填充", &rectanglePaint_);
        }
        if (tool_ == 6) {
            ui.Text("框选机关：" + std::to_string(groupIds_.size()));
            ui.SameLine();
            if (ui.Button("td2.television.group.copy", "复制框选")) {
                clipboard_.clear(); for (const auto& d : model_->devices.Items()) { if (std::find(groupIds_.begin(), groupIds_.end(), d.id) != groupIds_.end()) { clipboard_.push_back(d); } }
                if (clipboard_.empty()) { model_->error = "先拖框选中机关"; }
                else { ChooseTool(7); model_->error.clear(); }
            }
        }
        ui.Text(Hint());
    }
    // 一行が欄の幅で切れないよう、短い行に分ける。
    std::string Hint() const {
        switch (tool_) {
        case 1: return "左键拖动画地形 · 右键拖动擦除";
        case 2: return "拖动擦除地形和机关";
        case 3: return "点空格放「" + DeviceLabel(kind_) + "」· 点已有的机关改为选中\nShift+点击叠放 · 新放的沿用上次调好的属性";
        case 4: return "先点压板，再点门 / 激光 / 桥\n再点一次断开";
        case 5: return "点击设出生点";
        case 6: return "拖框选中多个机关，再「复制框选」";
        case 7: return clipboard_.empty() ? "先框选并复制机关" : "点击地图粘贴 " + std::to_string(clipboard_.size()) + " 个机关";
        default: return "点机关选中并拖动 · 拖角改大小 · Delete 删除\n右键拖动擦除 · 滚轮缩放 · 中键平移";
        }
    }
    // 種類ごとに最後に直した形（大きさ・向き・射程・追加の項目）を覚え、次に置くものへ写す。
    void RememberTemplate(const DeviceDraft& d) { templates_[d.kind] = d; }
    void ApplyTemplate(DeviceDraft& d, int cols, int rows) const {
        const auto found = templates_.find(d.kind);
        if (found == templates_.end()) { return; }
        const auto& t = found->second;
        d.w = std::clamp(t.w, .125, cols - d.x); d.h = std::clamp(t.h, .125, rows - d.y);
        d.direction = t.direction; d.range = t.range; d.inverted = t.inverted; d.latch = t.latch; d.extra = t.extra;
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
        const auto input = ui.BeginCanvas("td2.television.canvas", 0);
        canvasHovered_ = input.hovered != 0;
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
                        else if (model_->devices.Add(kind_, static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), s.region.cols, s.region.rows)) {
                            if (auto* added = model_->devices.Selected()) { ApplyTemplate(*added, s.region.cols, s.region.rows); }
                            s.devices = model_->devices.Serialize(); model_->Commit(ctx);
                        }
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
                        if (drag_ == Drag::Resize) { if (const auto* d = model_->devices.Selected()) { RememberTemplate(*d); } }
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
                // 置く前の影：指しているマスに、これから置く仕掛けを薄く描く。
                if (tool_ == 3 && drag_ == Drag::None && input.hovered && !view_.panning && !hand_ && inside && (model_->devices.Hit(p.x, p.y) < 0 || input.shift)) {
                    DeviceDraft ghost; ghost.kind = kind_; ghost.x = std::floor(p.x); ghost.y = std::floor(p.y);
                    if (kind_ == "Door" || kind_ == "LockWall") { ghost.h = (std::min)(3.0, s.region.rows - ghost.y); }
                    if (kind_ == "OneWay" || kind_ == "Bridge") { ghost.w = (std::min)(3.0, s.region.cols - ghost.x); }
                    if (kind_ == "Laser") { ghost.direction = "Right"; }
                    ApplyTemplate(ghost, s.region.cols, s.region.rows);
                    DrawDeviceGlyph(ui, ghost, view_);
                    ui.CanvasRect(view_.Screen(static_cast<float>(ghost.x), static_cast<float>(ghost.y)), view_.Screen(static_cast<float>(ghost.x + ghost.w), static_cast<float>(ghost.y + ghost.h)), {.97f, .80f, .40f, .7f}, 1.5f);
                }
            } else { ui.CanvasText({20, 20}, {.85f, .4f, .4f, 1}, valid.message); }
        }
        ui.EndCanvas();
        // Delete は地図の上にマウスがある時だけ（3D ビューポートの Delete は物体を消すので、同じキーで装置まで消さない）。
        if (canvasHovered_ && model_->ready && !model_->gesture && !ui.GetInteraction().editing && ui.GetInteraction().deletePressed) {
            if (model_->devices.RemoveSelected()) { model_->draft.devices = model_->devices.Serialize(); model_->Commit(ctx); }
        }
    }
    void DrawProperties(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui; if (!model_->ready) { return; }
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
            if (d->kind == "Spring") { DrawSpring(ui, *d); }
            if (d->kind == "Decal") { DrawDecal(ui, *d, s.region); }
            if (d->kind == "Laser" || d->kind == "Door" || d->kind == "Bridge") { ui.Checkbox("td2.television.device.invert", "反向激活", &d->inverted); }
            if (d->kind == "Plate") {
                ui.Checkbox("td2.television.device.latch", "踩一下切换（关：踩住时才通电）", &d->latch);
                bool reset = d->Extra("variant") == "reset";
                if (ui.Checkbox("td2.television.device.reset", "圆形复位钮外观（电视塔复位用）", &reset)) { d->SetExtra("variant", reset ? "reset" : ""); }
            }
            if (d->kind == "Plate") {
                ui.Separator(); ui.Text("连接目标（" + std::to_string(d->targets.size()) + "）");
                if (ui.Button("td2.television.device.link", "在地图上连线")) { tool_ = 4; linkSource_ = d->id; hand_ = false; }
                std::vector<int> targets;
                for (size_t i = 0; i < model_->devices.Items().size(); ++i) { const auto& target = model_->devices.Items()[i]; if (target.kind == "Door" || target.kind == "Laser" || target.kind == "Bridge") { targets.push_back(static_cast<int>(i)); } }
                if (targets.empty()) { ui.Text("本屏没有门、激光或桥"); }
                for (size_t i = 0; i < targets.size(); ++i) {
                    const int index = targets[i]; const auto& target = model_->devices.Items()[static_cast<size_t>(index)];
                    bool linked = std::find(d->targets.begin(), d->targets.end(), target.id) != d->targets.end();
                    if (ui.Checkbox("td2.television.device.target." + target.id, DeviceLabel(target.kind) + " " + std::to_string(index + 1), &linked)) { model_->devices.ToggleLink(d->id, index); }
                }
                const std::string plateId = d->id;
                DrawExternalTargets(ctx, plateId);
                d = model_->devices.Selected();
                if (!d) { s.devices = model_->devices.Serialize(); return; }
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
            if (previous.cols != s.region.cols || previous.rows != s.region.rows) { view_.fit = true; }
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
        // 数値のドラッグも草稿に集め、編集の終わりに一回だけ確定する。直した形は次に置く同じ種類へ写す。
        if (s != before) { propertyPending_ = true; if (const auto* d = model_->devices.Selected()) { RememberTemplate(*d); } }
        if (propertyPending_ && !model_->gesture && !ui.GetInteraction().editing) { model_->Commit(ctx); propertyPending_ = false; }
        if (!model_->devices.HasSelection()) { DrawDeviceList(ctx); }
    }
    // 今のテレビの仕掛けの一覧（地図の属性の下）。地図で見つけにくい小さな仕掛けを選ぶのに使う。
    void DrawDeviceList(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        if (model_->devices.Items().empty()) { return; }
        ui.Separator(); ui.Text("现有机关（" + std::to_string(model_->devices.Items().size()) + "）");
        EditText(ui, "td2.television.devices.search", "查找", deviceSearch_);
        std::vector<EditorChoice> choices; std::vector<int> indices;
        for (size_t i = 0; i < model_->devices.Items().size(); ++i) {
            const auto& d = model_->devices.Items()[i];
            const std::string name = DeviceLabel(d.kind) + " " + std::to_string(i + 1);
            if (!deviceSearch_.empty() && name.find(deviceSearch_) == std::string::npos && d.id.find(deviceSearch_) == std::string::npos) { continue; }
            choices.push_back({"td2.television.device.select." + d.id, name, model_->devices.SelectedIndex() == static_cast<int>(i)}); indices.push_back(static_cast<int>(i));
        }
        if (choices.empty()) { ui.Text("没有匹配的机关"); }
        else if (const int selected = DrawChoices(ui, choices); selected >= 0) { model_->devices.Select(indices[static_cast<size_t>(selected)]); ChooseTool(0); }
    }
    // バネ：強さ（高さの目安つき）・立ってから発射までの秒・冷却。既定値は書き出さない。
    static void DrawSpring(IEditorWorkspaceUi& ui, DeviceDraft& d) {
        const auto read = [&d](std::string_view key, double fallback) {
            const auto text = d.Extra(key);
            try { return text.empty() ? fallback : std::stod(text); } catch (...) { return fallback; }
        };
        double power = read("power", 22.5), delay = read("delay", .85), cooldown = read("cooldown", 3);
        if (EditFloat(ui, "td2.television.device.power", "弹力", power, 3, 44)) { d.SetExtra("power", std::abs(power - 22.5) < 1e-6 ? "" : ShortNumber(power)); }
        ui.Text("站着发射约弹起 " + ShortNumber(power * power / 87.5) + " 格（从高处落下会更高）");
        if (EditFloat(ui, "td2.television.device.delay", "站上去多久发射（秒）", delay, .05f, 3)) { d.SetExtra("delay", std::abs(delay - .85) < 1e-6 ? "" : ShortNumber(delay)); }
        if (EditFloat(ui, "td2.television.device.cooldown", "发射后冷却（秒）", cooldown, 3, 30)) { d.SetExtra("cooldown", std::abs(cooldown - 3) < 1e-6 ? "" : ShortNumber(cooldown)); }
    }
    // 貼り絵の図案：按键の説明と出口の矢印、前景（実体の地形の時に玩法の面より手前に出る暗い塊・垂れた線）。
    static void DrawDecal(IEditorWorkspaceUi& ui, DeviceDraft& d, const RegionDefinition& region) {
        constexpr std::array<std::pair<std::string_view, std::string_view>, 9> variants{{{"ad", "A / D 移动"}, {"space", "空格"}, {"spaceHold", "长按空格"},
            {"exit", "出口 →"}, {"exitLeft", "出口 ←"}, {"exitUp", "出口 ↑"}, {"exitDown", "出口 ↓"},
            {"fore", "前景块"}, {"foreCable", "前景线缆"}}};
        ui.Text("图案");
        const auto current = d.Extra("variant");
        for (size_t i = 0; i < variants.size(); ++i) {
            if (ui.Selectable("td2.television.device.decal." + std::string(variants[i].first), std::string(variants[i].second), current == variants[i].first)) {
                d.SetExtra("variant", variants[i].first);
            }
        }
        if (current != "fore" && current != "foreCable") { return; }
        // 前景：人物の立つ面から手前への距離（既定 1 は書き出さない）と、遊べるマスの前に置いた時の注意（置くのは止めない）。
        double depth = 1.0;
        try { if (const auto text = d.Extra("depth"); !text.empty()) { depth = std::stod(text); } } catch (...) {}
        if (EditFloat(ui, "td2.television.device.depth", "在角色前方多远（格）", depth, .25f, 6)) {
            d.SetExtra("depth", std::abs(depth - 1.0) < 1e-6 ? "" : ShortNumber(depth));
        }
        bool covers = false;
        for (int row = static_cast<int>(std::floor(d.y)); row < static_cast<int>(std::ceil(d.y + d.h)) && !covers; ++row)
            for (int col = static_cast<int>(std::floor(d.x)); col < static_cast<int>(std::ceil(d.x + d.w)) && !covers; ++col)
                covers = row >= 0 && col >= 0 && row < region.rows && col < region.cols && region.cells[CellIndex(region, col, row)] != '1';
        ui.Text("只在实体地形（受光 + 透视窗口）时显示；越近越大，也越会遮挡后面。");
        if (covers) { ui.Text("提示：盖住了可以走的格子，可能挡住角色或机关。"); }
    }
    // 圧力板の、別のテレビの装置と画面の外の仕掛けへの接続。今の接続の一覧（見つからない目標は印を付ける）も出す。
    void DrawExternalTargets(EditorWorkspaceHostContext& ctx, const std::string& plateId) {
        auto& ui = *ctx.ui;
        model_->RefreshMachines(ctx);
        const auto* self = model_->Current();
        if (!self || !model_->devices.Selected()) { return; }
        const auto here = Placement(model_->draft);
        const auto linked = [&](const std::string& target) {
            const auto* p = model_->devices.Selected();
            return p && std::find(p->targets.begin(), p->targets.end(), target) != p->targets.end();
        };
        // 別のテレビ：既定では辺を接するテレビだけ（作者が確かめやすい近い所から）。
        ui.Separator(); ui.Text("其他电视的门 / 激光 / 桥");
        ui.Checkbox("td2.television.device.remote.all", "显示所有电视（默认只显示相邻的）", &remoteAll_);
        int shown = 0;
        for (const auto& room : model_->rooms) {
            if (room.object.objectId == self->object.objectId) { continue; }
            const bool adjacent = here.worldPlaced && room.settings.worldPlaced && FindSharedWorldEdge(here, Placement(room.settings)).has_value();
            if (!remoteAll_ && !adjacent) { continue; }
            DeviceEditor other; other.Load(room.settings.devices);
            for (size_t i = 0; i < other.Items().size(); ++i) {
                const auto& device = other.Items()[i];
                if (device.kind != "Door" && device.kind != "Laser" && device.kind != "Bridge") { continue; }
                const std::string target = room.settings.region.regionId + "/" + device.id;
                bool on = linked(target);
                if (ui.Checkbox("td2.television.device.remote." + target, ShortChoiceLabel(room.object.name, 12) + " · " + DeviceLabel(device.kind) + " " + std::to_string(i + 1), &on)) {
                    model_->devices.ToggleTarget(plateId, target);
                }
                ++shown;
            }
        }
        if (!shown) { ui.Text(remoteAll_ ? "其他电视没有门、激光或桥" : "相邻电视没有门、激光或桥"); }
        ui.Text("提示：压板尽量和它控制的东西放在同一块屏幕里");
        // 画面の外の仕掛け。
        ui.Separator(); ui.Text("机关目标（转台、吊车、货架、电视塔、转柱）");
        int machines = 0;
        for (const auto& machine : model_->machines) {
            for (const auto& [target, label] : machine.Receivers()) {
                bool on = linked(target);
                if (ui.Checkbox("td2.television.device.machine." + target, label, &on)) { model_->devices.ToggleTarget(plateId, target); }
                ++machines;
            }
        }
        if (!machines) { ui.Text("场景里还没有机关（在世界地图的「机关」模式新建）"); }
        // 今の接続。どこにも見つからない目標は外せるようにする。
        const auto* current = model_->devices.Selected();
        if (!current || current->targets.empty()) { return; }
        ui.Separator(); ui.Text("当前连接");
        std::string removing;
        for (const auto& target : current->targets) {
            bool found = false;
            const auto kind = ClassifyTarget(target);
            if (kind == TargetKind::Local) {
                found = std::any_of(model_->devices.Items().begin(), model_->devices.Items().end(), [&](const auto& d) { return d.id == target; });
            } else if (kind == TargetKind::Machine) {
                for (const auto& m : model_->machines) for (const auto& r : m.Receivers()) { found = found || r.first == target; }
            } else if (kind == TargetKind::OtherRegion) {
                const auto slash = target.find('/');
                for (const auto& room : model_->rooms) {
                    if (room.settings.region.regionId != target.substr(0, slash)) { continue; }
                    DeviceEditor other; other.Load(room.settings.devices);
                    found = std::any_of(other.Items().begin(), other.Items().end(), [&](const auto& d) { return d.id == target.substr(slash + 1); });
                }
            }
            ui.Text(std::string(found ? "· " : "✕ 找不到：") + target);
            ui.SameLine();
            if (ui.Button("td2.television.device.unlink." + target, "移除")) { removing = target; }
        }
        if (!removing.empty()) { model_->devices.ToggleTarget(plateId, removing); }
    }
    void RefreshScreens(EditorWorkspaceHostContext& ctx) {
        screens_.clear();
        // 初期の候補はモデルを持つ物体に絞る（灯や記録用の物体で保存欄を押し出さない）。
        (void)QueryAll(ctx, "scene.objects.query", {{"detail", true}}, "objects", [this](const Json& object) {
            const auto id = ObjectId(object.value("objectId", Json{}));
            const bool model = object.contains("model") && object["model"].is_object() &&
                (!object["model"].value("assetId", "").empty() || object["model"].value("materialSlotCount", 0) > 0);
            if (id && (allScreens_ || id == model_->draft.screenObject || model)) { screens_.push_back({id, object.value("name", "对象 " + std::to_string(id))}); }
            return true;
        }, model_->error);
    }
    Drag drag_ = Drag::None;
    EditorWorkspacePoint start_{}, last_{};
    DeviceDraft original_;
    int tool_ = 0, handle_ = -1, roomPage_ = 0, devicePage_ = 0, targetPage_ = 0, brushSize_ = 1;
    bool erase_ = false, propertyPending_ = false, screenSettings_ = false, screenPicker_ = false, allScreens_ = false;
    bool gallery_ = false;
    bool rectanglePaint_ = false, remoteAll_ = false;
    std::set<size_t> erasedDeviceCells_;
    std::string kind_ = "Plate", linkSource_, deviceSearch_, gallerySearch_;
    std::string screenSearch_;
    int screenPage_ = 0;
    std::vector<std::string> groupIds_;
    std::vector<DeviceDraft> clipboard_;
    std::vector<SceneItem> screens_;
    std::map<std::string, DeviceDraft, std::less<>> templates_;
    uint64_t shown_ = 0;
    bool canvasHovered_ = false;
};
}
