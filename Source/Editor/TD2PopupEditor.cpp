#include "TD2PopupEditor.h"
#include "TD2EditorChoices.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace TD2::Editor::Popup {
namespace {
// ---- 表示の名前（並びは TD2PopupText.h の名前と同じ）
constexpr std::array<std::string_view, 4> kTriggerLabels{"時々", "様子", "出来事", "部屋"};
constexpr std::array<std::string_view, 4> kTierLabels{"前", "中", "後", "いつでも"};
constexpr std::array<std::string_view, 4> kMoodLabels{"止まったまま", "死んだすぐ後", "長居", "残影"};
constexpr std::array<std::string_view, 10> kEventLabels{"チェックポイント", "宝石", "機構", "大区域", "扉", "何度も死ぬ",
                                                        "移動の案内", "ダッシュの案内", "入れ替えの案内", "入れ替えた"};
constexpr std::array<std::string_view, 5> kIconLabels{"自動", "無し", "？", "！", "手紙"};
constexpr std::array<std::string_view, 6> kFilterIds{"all", "random", "mood", "event", "room", "egg"};
constexpr std::array<std::string_view, 6> kFilterLabels{"全部", "時々", "様子", "出来事", "部屋", "おまけ"};

// ---- 設定の欄（ID と名前は TD2.PopupScript の欄と同じ）。section は右の欄の見出し。
constexpr std::array<std::string_view, 3> kSections{"時々", "様子", "出来事"};
struct FloatSetting { std::string_view id, label; float PopupSettings::*member; float minimum, maximum, step; size_t section; };
struct IntSetting { std::string_view id, label; int PopupSettings::*member; int minimum, maximum; size_t section; };
struct BoolSetting { std::string_view id, label; bool PopupSettings::*member; size_t section; };
constexpr std::array<FloatSetting, 10> kFloatSettings{{
    {"firstDelay", "最初まで", &PopupSettings::firstDelay, 0, 600, 1, 0},
    {"intervalMax", "間隔の最長", &PopupSettings::intervalMax, 1, 600, 1, 0},
    {"intervalMin", "間隔の最短", &PopupSettings::intervalMin, 1, 600, 1, 0},
    {"intervalPerArea", "大区域ごとに縮める", &PopupSettings::intervalPerArea, 0, 120, 1, 0},
    {"moodChance", "様子の確率", &PopupSettings::moodChance, 0, 1, .05f, 1},
    {"moodCooldown", "様子の間", &PopupSettings::moodCooldown, 0, 1800, 10, 1},
    {"idleSeconds", "止まったまま", &PopupSettings::idleSeconds, 1, 120, 1, 1},
    {"diedSeconds", "死んだすぐ後", &PopupSettings::diedSeconds, 1, 120, 1, 1},
    {"stuckSeconds", "長居", &PopupSettings::stuckSeconds, 5, 1800, 5, 1},
    {"eventGap", "出来事の間", &PopupSettings::eventGap, 0, 600, 1, 2},
}};
constexpr std::array<IntSetting, 2> kIntSettings{{
    {"history", "重ねない数", &PopupSettings::history, 0, 60, 0},
    {"deathsToWorry", "心配する死の数", &PopupSettings::deathsToWorry, 1, 99, 2},
}};
constexpr std::array<BoolSetting, 3> kBoolSettings{{
    {"echoPerArea", "残影の様子は大区域ごと", &PopupSettings::echoPerArea, 1},
    {"checkpointPerArea", "チェックポイントは大区域ごと", &PopupSettings::checkpointPerArea, 2},
    {"mechanismPerArea", "機構は大区域ごと", &PopupSettings::mechanismPerArea, 2},
}};

// 見本の色：ゲームの弾窓に似せた明るい灰の窓、暗い題の帯、黒い字。
constexpr EditorWorkspaceColor kWindow{.84f, .84f, .85f, 1}, kTitleBar{.17f, .19f, .24f, 1}, kTitleInk{.93f, .94f, .95f, 1};
constexpr EditorWorkspaceColor kInk{.10f, .10f, .12f, 1}, kFaint{.48f, .48f, .52f, 1}, kEdge{.38f, .38f, .42f, 1}, kShadow{0, 0, 0, .3f};

// 台本の部品で読む欄。
Json PropertyIds() {
    Json ids = Json::array();
    ids.push_back("entries");
    for (const auto& f : kFloatSettings) { ids.push_back(std::string(f.id)); }
    for (const auto& f : kIntSettings) { ids.push_back(std::string(f.id)); }
    for (const auto& f : kBoolSettings) { ids.push_back(std::string(f.id)); }
    return ids;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
std::string_view TrimSpace(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) { s.remove_prefix(1); }
    while (!s.empty() && IsSpace(s.back())) { s.remove_suffix(1); }
    return s;
}
// 文：改行は \n だけにし、前後の空白と改行を落とす（書式の往復で変わらない形）。
std::string CleanText(std::string_view text) {
    std::string out;
    for (const char c : text) { if (c != '\r') { out += c; } }
    return std::string(TrimSpace(out));
}
std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    for (;;) {
        const auto cut = text.find('\n');
        lines.push_back(text.substr(0, cut));
        if (cut == std::string_view::npos) { break; }
        text.remove_prefix(cut + 1);
    }
    return lines;
}
// 一行の字の数（UTF-8 の一字を 1、半角の空白は半分）。
double LineLength(std::string_view line) {
    double n = 0;
    for (const char c : line) {
        const auto u = static_cast<unsigned char>(c);
        if ((u & 0xc0u) == 0x80u) { continue; }
        n += u == ' ' ? .5 : 1;
    }
    return n;
}

// 書く前に整える：空白を落とし、種類に関係ない項目は既定に戻す（書式に出ない値を草稿に残さない）。
void Normalize(PopupEntry& e) {
    e.text = CleanText(e.text);
    std::string title;
    for (const char c : e.title) { if (!IsSpace(c) && c != '|') { title += c; } }
    e.title = std::move(title);
    e.id = std::string(TrimSpace(e.id));
    e.room = std::string(TrimSpace(e.room));
    if (e.trigger != PopupTrigger::Random) { e.tier = PopupTier::Early; }
    if (e.trigger != PopupTrigger::Mood) { e.mood = PopupMood::Idle; }
    if (e.trigger != PopupTrigger::Event) { e.event = PopupEvent::Checkpoint; }
    const bool chanceKind = e.trigger == PopupTrigger::Random || e.trigger == PopupTrigger::Mood;
    e.chance = chanceKind && std::isfinite(e.chance) ? std::clamp(e.chance, 0.0, 1.0) : 0.0;
}

// 書けない理由（整えた後の一件）。ID の重なりは別に見る。
std::string EntryProblem(const PopupEntry& e) {
    if (e.id.empty()) { return "ID が要ります"; }
    if (!PopupText::StableId(e.id)) { return "ID は英数字と . _ - だけ"; }
    if (!e.room.empty() && !PopupText::StableId(e.room)) { return "部屋の ID は英数字と . _ - だけ"; }
    if (e.trigger == PopupTrigger::Room && e.room.empty()) { return "部屋が要ります"; }
    if (e.text.empty()) { return "文が要ります"; }
    return {};
}

bool Matches(const PopupEntry& e, PopupFilter filter) {
    switch (filter) {
    case PopupFilter::All: return true;
    case PopupFilter::Random: return e.trigger == PopupTrigger::Random;
    case PopupFilter::Mood: return e.trigger == PopupTrigger::Mood;
    case PopupFilter::Event: return e.trigger == PopupTrigger::Event;
    case PopupFilter::Room: return e.trigger == PopupTrigger::Room;
    case PopupFilter::Egg: return e.chance > 0;
    }
    return false;
}

// 一覧の札：種類の一字（時々は段）と文の一行目。
std::string ItemLabel(const PopupEntry& e, bool problem) {
    constexpr std::array<std::string_view, 4> tiers{"前", "中", "後", "常"};
    std::string tag;
    switch (e.trigger) {
    case PopupTrigger::Random: tag = tiers[static_cast<size_t>(e.tier)]; break;
    case PopupTrigger::Mood: tag = "様"; break;
    case PopupTrigger::Event: tag = "出"; break;
    case PopupTrigger::Room: tag = "部"; break;
    }
    const std::string_view text = e.text;
    const auto cut = text.find('\n');
    std::string line = ShortChoiceLabel(text.substr(0, cut), 14);
    if (cut != std::string_view::npos && !line.ends_with("…")) { line += "…"; }
    return (problem ? "⚠ " : "") + tag + (e.chance > 0 ? " おまけ " : " ") + line;
}

// 選択肢の一行（表の一列目に名前）。perRow ごとに行を変える。
template <class E, size_t N>
bool ChoiceRow(IEditorWorkspaceUi& ui, std::string_view id, std::string_view label, const std::array<std::string_view, N>& names,
               const std::array<std::string_view, N>& labels, E& value, size_t perRow) {
    if (!ui.BeginTable(id, static_cast<uint32_t>(perRow + 1))) { return false; }
    bool changed = false;
    for (size_t i = 0; i < N; ++i) {
        if (i % perRow == 0) {
            ui.TableNextRow(); ui.TableNextColumn();
            if (i == 0) { ui.Text(label); }
        }
        ui.TableNextColumn();
        const bool on = static_cast<size_t>(value) == i;
        if (ui.Selectable(std::string(id) + "." + std::string(names[i]), labels[i], on) && !on) { value = static_cast<E>(i); changed = true; }
    }
    ui.EndTable();
    return changed;
}

bool EditLines(IEditorWorkspaceUi& ui, std::string_view id, std::string_view label, std::string& value, float height) {
    std::vector<char> buffer((std::max)(size_t{1024}, value.size() + 256), '\0');
    std::memcpy(buffer.data(), value.data(), value.size());
    if (!ui.InputTextMultiline(id, label, buffer.data(), buffer.size(), height)) { return false; }
    value = buffer.data();
    return true;
}

bool EditSetting(IEditorWorkspaceUi& ui, std::string_view id, std::string_view label, float& value, float minimum, float maximum, float step) {
    float number = value;
    EditorWorkspaceFloatInputOptions options{};
    options.step = step; options.minimum = minimum; options.maximum = maximum; options.clampToRange = 1;
    if (!ui.InputFloat(id, label, &number, options) || !std::isfinite(number)) { return false; }
    number = std::clamp(number, minimum, maximum);
    if (number == value) { return false; }
    value = number;
    return true;
}

bool EditSetting(IEditorWorkspaceUi& ui, std::string_view id, std::string_view label, int& value, int minimum, int maximum) {
    int32_t number = value;
    EditorWorkspaceIntInputOptions options{};
    options.minimum = minimum; options.maximum = maximum; options.clampToRange = 1;
    if (!ui.InputInt(id, label, &number, options)) { return false; }
    number = std::clamp(number, minimum, maximum);
    if (number == value) { return false; }
    value = number;
    return true;
}

void AppendSettings(Json& commands, const RegionTarget& t, const PopupSettings& before, const PopupSettings& after) {
    for (const auto& f : kFloatSettings) {
        if (before.*f.member != after.*f.member) { commands.push_back(PropertyCommand(t.objectId, t.componentId, f.id, after.*f.member)); }
    }
    for (const auto& f : kIntSettings) {
        if (before.*f.member != after.*f.member) { commands.push_back(PropertyCommand(t.objectId, t.componentId, f.id, after.*f.member)); }
    }
    for (const auto& f : kBoolSettings) {
        if (before.*f.member != after.*f.member) { commands.push_back(PropertyCommand(t.objectId, t.componentId, f.id, after.*f.member)); }
    }
}

// 見本の絵：？ と ！ は色の札に一字、手紙は封筒の線。
void DrawIcon(IEditorWorkspaceUi& ui, PopupIcon icon, EditorWorkspacePoint at, float size) {
    const EditorWorkspacePoint end{at.x + size, at.y + size};
    if (icon == PopupIcon::Mail) {
        ui.CanvasFill(at, end, {.97f, .97f, .95f, 1});
        const float left = at.x + size * .14f, right = at.x + size * .86f, top = at.y + size * .24f, bottom = at.y + size * .76f;
        const EditorWorkspacePoint fold{at.x + size * .5f, at.y + size * .54f};
        ui.CanvasRect({left, top}, {right, bottom}, kInk, 1.5f);
        ui.CanvasLine({left, top}, fold, kInk, 1.5f);
        ui.CanvasLine({right, top}, fold, kInk, 1.5f);
        return;
    }
    const bool question = icon == PopupIcon::Question;
    ui.CanvasFill(at, end, question ? EditorWorkspaceColor{.27f, .46f, .80f, 1} : EditorWorkspaceColor{.95f, .74f, .22f, 1});
    const float glyph = size * .62f;
    ui.CanvasStyledText({at.x + (size - glyph) * .5f, at.y + (size - glyph) * .5f}, glyph, size, question ? kTitleInk : kInk, question ? "？" : "！");
}

// 選んだ弾窓の見本（題の帯・絵・文の行）。
void DrawPreview(IEditorWorkspaceUi& ui, const PopupEntry& entry) {
    auto shown = entry;
    Normalize(shown);
    const bool empty = shown.text.empty();
    const auto lines = SplitLines(empty ? std::string_view{"…"} : std::string_view{shown.text});
    const bool icon = shown.icon == PopupIcon::Question || shown.icon == PopupIcon::Warning || shown.icon == PopupIcon::Mail;
    constexpr float kTextSize = 16, kLine = 21, kTitle = 22, kPad = 10, kIcon = 36;
    float longest = 0;
    for (const auto line : lines) { longest = (std::max)(longest, TextWidth(line) * kTextSize / 13.f); }
    const float body = (std::max)(icon ? kIcon : 0.f, static_cast<float>(lines.size()) * kLine);
    const float height = kTitle + kPad * 2 + body;
    const auto input = ui.BeginCanvas("td2.popup.preview", (std::max)(110.f, height + 16));
    if (input.size.x > 40 && input.size.y > 0) {
        const float want = longest + (icon ? kIcon + kPad : 0.f) + kPad * 2 + 4;
        const float width = (std::min)((std::max)(want, 200.f), input.size.x - 16);
        const float x0 = (input.size.x - width) * .5f, y0 = 8, x1 = x0 + width, y1 = y0 + height;
        ui.CanvasFill({x0 + 4, y0 + 4}, {x1 + 4, y1 + 4}, kShadow);
        ui.CanvasFill({x0, y0}, {x1, y1}, kWindow);
        ui.CanvasFill({x0, y0}, {x1, y0 + kTitle}, kTitleBar);
        // 帯の下の角は四角にする。
        ui.CanvasLine({x0, y0 + kTitle - 3}, {x1, y0 + kTitle - 3}, kTitleBar, 6);
        ui.CanvasRect({x0, y0}, {x1, y1}, kEdge);
        const float box = kTitle - 8, bx = x1 - box - 5, by = y0 + 4;
        ui.CanvasRect({bx, by}, {bx + box, by + box}, kTitleInk);
        ui.CanvasLine({bx + 3, by + 3}, {bx + box - 3, by + box - 3}, kTitleInk, 1.5f);
        ui.CanvasLine({bx + box - 3, by + 3}, {bx + 3, by + box - 3}, kTitleInk, 1.5f);
        ui.CanvasStyledText({x0 + 8, y0 + 4}, 14, (std::max)(10.f, width - box - 24), kTitleInk,
                            shown.title.empty() ? std::string_view{"？？？"} : std::string_view{shown.title});
        float tx = x0 + kPad;
        const float top = y0 + kTitle + kPad;
        if (icon) { DrawIcon(ui, shown.icon, {tx, top + (body - kIcon) * .5f}, kIcon); tx += kIcon + kPad; }
        float ty = top + (body - static_cast<float>(lines.size()) * kLine) * .5f + (kLine - kTextSize) * .5f;
        for (const auto line : lines) {
            ui.CanvasStyledText({tx, ty}, kTextSize, (std::max)(10.f, x1 - kPad - tx), empty ? kFaint : kInk, line);
            ty += kLine;
        }
    }
    ui.EndCanvas();
}
}

EditorWorkspaceDescriptor PopupEditor::GetDescriptor() const {
    EditorWorkspaceDescriptor d; d.workspaceId = kPopupWorkspace; d.displayName = "弾窓";
    return d;
}

void PopupEditor::OnActivate(EditorWorkspaceHostContext&) {
    stale_ = true; clock_ = 1; historyFrames_ = 0; regionsLoaded_ = false; confirmDelete_ = false;
}

void PopupEditor::OnDeactivate(EditorWorkspaceHostContext& ctx) {
    // 書ける草稿は書いておく（書けない草稿は残し、戻った時に直せるようにする）。
    if (ctx.commands && IsDirty() && Commit(ctx)) { pending_ = false; }
    confirmDelete_ = false;
}

bool PopupEditor::IsDirty() const {
    if (!loaded_ || !read_.target.objectId) { return false; }
    if (settings_ != read_.settings) { return true; }
    return read_.parseError.empty() ? entries_ != read_.entries : raw_ != read_.text;
}

EditorWorkspaceSaveResult PopupEditor::Save(EditorWorkspaceHostContext& ctx) {
    if (!ctx.commands || !Commit(ctx)) { return EditorWorkspaceSaveResult::Failed; }
    pending_ = false;
    const auto saved = Execute(ctx, "scene.save", Json::object());
    if (!saved.ok) { error_ = saved.error; return EditorWorkspaceSaveResult::Failed; }
    error_.clear();
    return EditorWorkspaceSaveResult::Saved;
}

void PopupEditor::Draw(EditorWorkspaceHostContext& ctx) {
    if (!ctx.ui || !ctx.commands) { return; }
    auto& ui = *ctx.ui;
    clock_ += ctx.unscaledDeltaSeconds; elapsed_ += ctx.unscaledDeltaSeconds;
    // Ctrl+Z / Ctrl+Y はシーンの履歴へ行く。ここでは草稿を捨てて、数フレーム読み直す。
    const auto interaction = ui.GetInteraction();
    if (!interaction.editing && (interaction.undoPressed || interaction.redoPressed)) { historyFrames_ = 3; }
    if (historyFrames_ > 0) { --historyFrames_; DropDraft(); stale_ = true; }
    if (Pulse(ctx, false)) { stale_ = true; }
    Poll(ctx);
    if (!regionsLoaded_ && read_.target.objectId) { LoadRegions(ctx); }
    Validate();
    if (ui.BeginPanel("td2.popup.list", "一覧", nullptr, EditorWorkspaceDockRegion::Left)) { DrawList(ctx); } ui.EndPanel();
    if (ui.BeginPanel("td2.popup.form", "弾窓", nullptr, EditorWorkspaceDockRegion::Center)) { DrawEntry(ctx); } ui.EndPanel();
    if (ui.BeginPanel("td2.popup.settings", "設定", nullptr, EditorWorkspaceDockRegion::Right)) { DrawSettings(ctx); } ui.EndPanel();
    // 入力が終わってから一回だけ書く（一字ごとに取り消しを積まない）。書けない草稿は直るまで待つ。
    if (pending_ && !ui.GetInteraction().editing) {
        NormalizeDraft(); Validate();
        if (problem_.empty()) { pending_ = false; (void)Commit(ctx); }
    }
    Publish(ctx);
}

// editor.state の目印（シーンと Play）が変わったら true。操作は一回ごとにログへ残るので 0.3 秒に一回まで。
bool PopupEditor::Pulse(EditorWorkspaceHostContext& ctx, bool force) {
    if (!force && clock_ < .3f) { return false; }
    clock_ = 0;
    const auto state = Execute(ctx, "editor.state", Json::object());
    if (!state.ok) { return false; }
    auto mark = state.data.value("scene", Json::object()).dump() + state.data.value("play", Json::object()).dump();
    if (mark == mark_) { return false; }
    mark_ = std::move(mark);
    return true;
}

// 目印が変わった時と 30 秒ごとに読み直す。草稿がある間と入力中は読まない。
void PopupEditor::Poll(EditorWorkspaceHostContext& ctx) {
    if (elapsed_ >= 30.f) { stale_ = true; }
    if (!stale_ || pending_ || IsDirty() || ctx.ui->GetInteraction().editing) { return; }
    stale_ = false; elapsed_ = 0;
    ScriptSnapshot fresh;
    if (!Read(ctx, fresh, readError_)) { return; }
    readError_.clear();
    Adopt(std::move(fresh));
}

// シーンで最初の台本を読む。台本が無ければ target が 0 のまま true。
bool PopupEditor::Read(EditorWorkspaceHostContext& ctx, ScriptSnapshot& out, std::string& error) {
    out = {};
    std::vector<SceneItem> objects;
    if (!QueryObjects(ctx, kPopupScriptComponentType, objects, error)) { return false; }
    out.count = objects.size();
    if (objects.empty()) { return true; }
    const uint64_t objectId = objects.front().objectId;
    const auto instances = Execute(ctx, "component.instances.query", {{"objectId", objectId}});
    if (!instances.ok) { error = instances.error; return false; }
    uint64_t componentId = 0;
    for (const auto& c : instances.data.value("components", Json::array())) {
        if (c.is_object() && c.value("type", "") == kPopupScriptComponentType) { componentId = ObjectId(c.value("componentId", Json{})); break; }
    }
    if (!componentId) { error = "台本の部品が見つかりません"; return false; }
    const auto described = Execute(ctx, "component.describe", {{"objectId", objectId}, {"componentId", componentId}, {"propertyIds", PropertyIds()}});
    if (!described.ok) { error = described.error; return false; }
    const auto fields = described.data.value("fields", Json::array());
    const auto* entries = FieldValue(fields, "entries");
    if (!entries || !entries->is_string()) { error = "台本の「弾窓」欄が読めません"; return false; }
    out.target = {objectId, componentId};
    out.text = entries->get<std::string>();
    // 無い欄は既定値のまま（部品の既定と同じ）。
    for (const auto& f : kFloatSettings) { if (const auto* v = FieldValue(fields, f.id); v && v->is_number()) { out.settings.*f.member = v->get<float>(); } }
    for (const auto& f : kIntSettings) { if (const auto* v = FieldValue(fields, f.id); v && v->is_number_integer()) { out.settings.*f.member = v->get<int>(); } }
    for (const auto& f : kBoolSettings) { if (const auto* v = FieldValue(fields, f.id); v && v->is_boolean()) { out.settings.*f.member = v->get<bool>(); } }
    if (!PopupText::Parse(out.text, out.entries, out.parseError) && out.parseError.empty()) { out.parseError = "台本が読めません"; }
    return true;
}

// 読んだ姿を草稿にする。選んでいた弾窓は ID で選び直す。
void PopupEditor::Adopt(ScriptSnapshot fresh) {
    const std::string keep = HasSelection() ? entries_[static_cast<size_t>(selected_)].id : std::string{};
    read_ = std::move(fresh);
    entries_ = read_.entries; settings_ = read_.settings; raw_ = read_.text;
    loaded_ = true; pending_ = false; ++version_;
    int index = -1;
    if (!keep.empty()) {
        const auto found = std::find_if(entries_.begin(), entries_.end(), [&keep](const PopupEntry& e) { return e.id == keep; });
        if (found != entries_.end()) { index = static_cast<int>(found - entries_.begin()); }
    }
    Select(index);
}

void PopupEditor::DropDraft() {
    if (loaded_) { Adopt(read_); } else { pending_ = false; }
}

// 部屋の ID の確かめ用に、シーンの区域 ID を一度だけ集める。
void PopupEditor::LoadRegions(EditorWorkspaceHostContext& ctx) {
    regionsLoaded_ = true; regionsOk_ = false; regions_.clear();
    std::vector<SceneItem> objects;
    std::string error;
    if (!QueryObjects(ctx, kRegionComponentType, objects, error)) { return; }
    Json ids = Json::array();
    ids.push_back("regionId");
    for (const auto& object : objects) {
        const auto instances = Execute(ctx, "component.instances.query", {{"objectId", object.objectId}});
        if (!instances.ok) { return; }
        for (const auto& c : instances.data.value("components", Json::array())) {
            if (!c.is_object() || c.value("type", "") != kRegionComponentType) { continue; }
            const auto described = Execute(ctx, "component.describe", {{"objectId", object.objectId}, {"componentId", ObjectId(c.value("componentId", Json{}))}, {"propertyIds", ids}});
            if (!described.ok) { return; }
            const auto fields = described.data.value("fields", Json::array());
            if (const auto* v = FieldValue(fields, "regionId"); v && v->is_string()) { regions_.insert(v->get<std::string>()); }
        }
    }
    regionsOk_ = true;
}

// 読んだ時から変わっていなければ一回の取り消しで書き、読み直して確かめる。
bool PopupEditor::Commit(EditorWorkspaceHostContext& ctx) {
    if (!ctx.commands || !IsDirty()) { return true; }
    NormalizeDraft(); Validate();
    if (!problem_.empty()) { error_ = problem_; return false; }
    if (!IsDirty()) { return true; }
    ScriptSnapshot now;
    if (!Read(ctx, now, error_)) { return false; }
    if (now != read_) { Adopt(std::move(now)); error_ = "台本がほかの編集で変わりました"; return false; }
    const auto target = read_.target;
    const bool parsed = read_.parseError.empty();
    Json commands = Json::array();
    if (parsed && entries_ != read_.entries) { commands.push_back(PropertyCommand(target.objectId, target.componentId, "entries", PopupText::Format(entries_))); }
    if (!parsed && raw_ != read_.text) { commands.push_back(PropertyCommand(target.objectId, target.componentId, "entries", raw_)); }
    AppendSettings(commands, target, read_.settings, settings_);
    if (!Transact(ctx, commands, error_)) { return false; }
    ScriptSnapshot after;
    const bool readBack = Read(ctx, after, error_);
    // 自分の書き込みを目印に含めておく（すぐもう一度読み直さない）。
    (void)Pulse(ctx, true);
    if (!readBack) { return false; }
    const bool landed = after.target == target && after.settings == settings_ &&
        (parsed ? after.parseError.empty() && after.entries == entries_ : after.text == raw_);
    if (!landed) { read_ = std::move(after); ++version_; error_ = "台本の書き込みを確かめられません"; return false; }
    Adopt(std::move(after));
    error_.clear();
    return true;
}

bool PopupEditor::Create(EditorWorkspaceHostContext& ctx) {
    const auto created = Execute(ctx, "scene.create_empty_object", {{"name", "TD2 弾窓の台本"}});
    if (!created.ok) { error_ = created.error; return false; }
    const uint64_t objectId = ObjectId(created.data.value("objectId", Json{}));
    if (!objectId) { error_ = "台本の物体を作れません"; return false; }
    const auto added = Execute(ctx, "component.add", {{"objectId", objectId}, {"componentType", std::string(kPopupScriptComponentType)}});
    if (!added.ok) { error_ = added.error; return false; }
    error_.clear(); stale_ = true; regionsLoaded_ = false;
    (void)Pulse(ctx, true);
    return true;
}

void PopupEditor::Publish(EditorWorkspaceHostContext& ctx) {
    const std::string& message = !error_.empty() ? error_ : readError_;
    if (message == published_) { return; }
    ctx.commands->PublishStatus(message.empty() ? EditorWorkspaceStatusSeverity::Info : EditorWorkspaceStatusSeverity::Warning, message);
    published_ = message;
}

void PopupEditor::NormalizeDraft() {
    if (normalized_ == version_) { return; }
    bool changed = false;
    for (auto& e : entries_) {
        auto n = e;
        Normalize(n);
        if (n != e) { e = std::move(n); changed = true; }
    }
    if (changed) { ++version_; }
    normalized_ = version_;
}

// 草稿の最初の問題（problem_ が空なら書ける）。草稿が変わった時だけ調べ直す。
void PopupEditor::Validate() {
    if (validated_ == version_) { return; }
    validated_ = version_;
    problem_.clear(); problemIndex_ = -1;
    if (!Editable()) { return; }
    std::set<std::string, std::less<>> seen;
    for (size_t i = 0; i < entries_.size(); ++i) {
        auto e = entries_[i];
        Normalize(e);
        auto problem = EntryProblem(e);
        if (problem.empty() && !seen.insert(e.id).second) { problem = "ID が重なっています"; }
        if (!problem.empty()) { problem_ = (e.id.empty() ? std::string("ID なし") : e.id) + "：" + problem; problemIndex_ = static_cast<int>(i); return; }
    }
}

void PopupEditor::Select(int index) {
    if (index == selected_) { return; }
    selected_ = index; ++serial_; confirmDelete_ = false;
}

std::string PopupEditor::UniqueId(std::string base, int first) const {
    if (base.size() > 88) { base.resize(88); }
    for (int n = first;; ++n) {
        auto id = base + std::to_string(n);
        if (std::none_of(entries_.begin(), entries_.end(), [&id](const PopupEntry& e) { return e.id == id; })) { return id; }
    }
}

// 今の絞り込みの種類で一つ足し、同じ種類の最後の後ろへ入れる（台本の並びを種類ごとに保つ）。
void PopupEditor::AddEntry() {
    PopupEntry e;
    std::string prefix = "r.";
    switch (filter_) {
    case PopupFilter::Mood: e.trigger = PopupTrigger::Mood; prefix = "m."; break;
    case PopupFilter::Event: e.trigger = PopupTrigger::Event; prefix = "e."; break;
    case PopupFilter::Room: e.trigger = PopupTrigger::Room; prefix = "o."; break;
    case PopupFilter::Egg: e.chance = .05; break;
    default: break;
    }
    e.id = UniqueId(prefix + "new", 1);
    e.text = "…";
    const auto group = filter_ == PopupFilter::All ? PopupFilter::Random : filter_;
    size_t at = entries_.size();
    for (size_t i = entries_.size(); i-- > 0;) { if (Matches(entries_[i], group)) { at = i + 1; break; } }
    entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(at), std::move(e));
    selected_ = static_cast<int>(at); ++serial_; confirmDelete_ = false;
    MarkPending();
}

void PopupEditor::DuplicateEntry() {
    if (!HasSelection()) { return; }
    const size_t at = static_cast<size_t>(selected_) + 1;
    auto copy = entries_[static_cast<size_t>(selected_)];
    copy.id = UniqueId(std::string(TrimSpace(copy.id)) + ".", 2);
    entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(at), std::move(copy));
    selected_ = static_cast<int>(at); ++serial_; confirmDelete_ = false;
    MarkPending();
}

// 消した後は同じ絞り込みの次（無ければ前）を選ぶ。
void PopupEditor::DeleteEntry() {
    confirmDelete_ = false;
    if (!HasSelection()) { return; }
    const int from = selected_;
    entries_.erase(entries_.begin() + from);
    int next = -1;
    for (int i = from; i < static_cast<int>(entries_.size()); ++i) { if (Matches(entries_[static_cast<size_t>(i)], filter_)) { next = i; break; } }
    for (int i = from - 1; next < 0 && i >= 0; --i) { if (Matches(entries_[static_cast<size_t>(i)], filter_)) { next = i; } }
    selected_ = next; ++serial_;
    MarkPending();
}

void PopupEditor::DrawList(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (!error_.empty()) { ui.Text("⚠ " + error_); }
    if (!readError_.empty()) { ui.Text("⚠ " + readError_); }
    if (!loaded_) { return; }
    if (!read_.target.objectId) {
        if (ui.Button("td2.popup.create", "台本を作る")) { (void)Create(ctx); }
        return;
    }
    if (read_.count > 1) { ui.Text("⚠ 台本が二つ以上あります"); }
    if (!read_.parseError.empty()) { ui.Text("⚠ " + read_.parseError); }
    if (!problem_.empty() && problem_ != error_) { ui.Text("⚠ " + problem_); }
    // 絞り込み（札に数）。
    std::array<size_t, kFilterIds.size()> counts{};
    for (const auto& e : entries_) for (size_t i = 0; i < counts.size(); ++i) { if (Matches(e, static_cast<PopupFilter>(i))) { ++counts[i]; } }
    if (ui.BeginTable("td2.popup.filters", 3)) {
        for (size_t i = 0; i < kFilterIds.size(); ++i) {
            if (i % 3 == 0) { ui.TableNextRow(); }
            ui.TableNextColumn();
            const auto filter = static_cast<PopupFilter>(i);
            if (ui.Selectable("td2.popup.filter." + std::string(kFilterIds[i]), std::string(kFilterLabels[i]) + " " + std::to_string(counts[i]), filter_ == filter)) { filter_ = filter; }
        }
        ui.EndTable();
    }
    if (Editable()) {
        if (!confirmDelete_) {
            if (ui.Button("td2.popup.add", "追加")) { AddEntry(); }
            if (HasSelection()) {
                ui.SameLine(); if (ui.Button("td2.popup.duplicate", "複製")) { DuplicateEntry(); }
                ui.SameLine(); if (ui.Button("td2.popup.delete", "削除")) { confirmDelete_ = true; }
            }
        } else {
            if (ui.Button("td2.popup.delete.confirm", "削除する")) { DeleteEntry(); }
            ui.SameLine(); if (ui.Button("td2.popup.delete.cancel", "取り消し")) { confirmDelete_ = false; }
        }
    }
    ui.Separator();
    Validate();
    // 台本の並びのまま。ID が重なった時だけ番号を足して札の ID をぶつけない。
    std::set<std::string, std::less<>> seen;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        if (!Matches(e, filter_)) { continue; }
        std::string id = "td2.popup.item." + e.id;
        if (!seen.insert(e.id).second) { id += "~" + std::to_string(i); }
        if (ui.Selectable(id, ItemLabel(e, static_cast<int>(i) == problemIndex_), selected_ == static_cast<int>(i))) { Select(static_cast<int>(i)); }
    }
}

void PopupEditor::DrawEntry(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (!loaded_ || !read_.target.objectId) { return; }
    if (!read_.parseError.empty()) { DrawRaw(ctx); return; }
    if (!HasSelection()) { ui.Text("一覧から選ぶ"); return; }
    auto& e = entries_[static_cast<size_t>(selected_)];
    DrawPreview(ui, e);
    const auto before = e;
    const std::string s = "." + std::to_string(serial_);

    (void)EditLines(ui, "td2.popup.text" + s, "文", e.text, 70);
    {
        const auto text = CleanText(e.text);
        const auto lines = SplitLines(text);
        const bool longLine = std::any_of(lines.begin(), lines.end(), [](std::string_view line) { return LineLength(line) > 10; });
        if (text.empty()) { ui.Text("⚠ 文が要ります"); }
        else if (lines.size() > 2 || longLine) { ui.Text("⚠ 二行・十字を超えています"); }
    }

    (void)ChoiceRow(ui, "td2.popup.kind", "種類", kPopupTriggerNames, kTriggerLabels, e.trigger, 4);
    switch (e.trigger) {
    case PopupTrigger::Random: (void)ChoiceRow(ui, "td2.popup.tier", "段", kPopupTierNames, kTierLabels, e.tier, 4); break;
    case PopupTrigger::Mood: (void)ChoiceRow(ui, "td2.popup.mood", "様子", kPopupMoodNames, kMoodLabels, e.mood, 4); break;
    case PopupTrigger::Event: (void)ChoiceRow(ui, "td2.popup.event", "出来事", kPopupEventNames, kEventLabels, e.event, 3); break;
    case PopupTrigger::Room: break;
    }

    const bool roomKind = e.trigger == PopupTrigger::Room;
    (void)EditText(ui, "td2.popup.room" + s, roomKind ? "部屋" : "この部屋だけ", e.room);
    {
        const auto room = TrimSpace(e.room);
        if (roomKind && room.empty()) { ui.Text("⚠ 部屋が要ります"); }
        else if (!room.empty() && !PopupText::StableId(room)) { ui.Text("⚠ 部屋の ID は英数字と . _ - だけ"); }
        else if (!room.empty() && regionsOk_ && !regions_.contains(room)) { ui.Text("⚠ その部屋はありません"); }
    }

    if (e.trigger == PopupTrigger::Random || e.trigger == PopupTrigger::Mood) {
        float chance = static_cast<float>(e.chance);
        EditorWorkspaceFloatInputOptions options{};
        options.step = .01f; options.minimum = 0; options.maximum = 1; options.clampToRange = 1;
        // 小数 4 桁に丸め、台本に長い数を書かない。
        if (ui.InputFloat("td2.popup.chance" + s, "おまけの確率", &chance, options) && std::isfinite(chance)) {
            e.chance = std::round(static_cast<double>(std::clamp(chance, 0.f, 1.f)) * 10000.0) / 10000.0;
        }
    }
    (void)ui.Checkbox("td2.popup.once" + s, "一度だけ", &e.once);
    (void)EditText(ui, "td2.popup.title" + s, "題", e.title);
    (void)ChoiceRow(ui, "td2.popup.icon", "絵", kPopupIconNames, kIconLabels, e.icon, 5);

    (void)EditText(ui, "td2.popup.id" + s, "ID", e.id);
    {
        const auto id = TrimSpace(e.id);
        const auto self = static_cast<size_t>(selected_);
        bool duplicate = false;
        for (size_t i = 0; i < entries_.size() && !duplicate; ++i) { duplicate = i != self && TrimSpace(entries_[i].id) == id; }
        if (id.empty()) { ui.Text("⚠ ID が要ります"); }
        else if (!PopupText::StableId(id)) { ui.Text("⚠ ID は英数字と . _ - だけ"); }
        else if (duplicate) { ui.Text("⚠ ID が重なっています"); }
    }
    if (e != before) { MarkPending(); }
}

// 読めない行がある時：原文だけを直す（言葉の欄から書き直して原文を消さない）。
void PopupEditor::DrawRaw(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    std::vector<char> buffer((std::max)(size_t{4096}, raw_.size() + 64u * 1024u), '\0');
    std::memcpy(buffer.data(), raw_.data(), raw_.size());
    if (ui.InputTextMultiline("td2.popup.raw", "原文", buffer.data(), buffer.size(), 320)) { raw_ = buffer.data(); MarkPending(); }
}

void PopupEditor::DrawSettings(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (!loaded_ || !read_.target.objectId) { return; }
    bool changed = false;
    for (size_t section = 0; section < kSections.size(); ++section) {
        if (section > 0) { ui.Separator(); }
        ui.Text(kSections[section]);
        for (const auto& f : kFloatSettings) {
            if (f.section == section) { changed |= EditSetting(ui, "td2.popup.setting." + std::string(f.id), f.label, settings_.*f.member, f.minimum, f.maximum, f.step); }
        }
        for (const auto& f : kIntSettings) {
            if (f.section == section) { changed |= EditSetting(ui, "td2.popup.setting." + std::string(f.id), f.label, settings_.*f.member, f.minimum, f.maximum); }
        }
        for (const auto& f : kBoolSettings) {
            if (f.section == section) { changed |= ui.Checkbox("td2.popup.setting." + std::string(f.id), f.label, &(settings_.*f.member)); }
        }
        if (section == 0 && settings_.intervalMin > settings_.intervalMax) { ui.Text("⚠ 間隔の最短が最長より長い"); }
    }
    if (changed) { MarkPending(); }
}

std::vector<std::unique_ptr<IEditorWorkspace>> MakePopupWorkspaces() {
    std::vector<std::unique_ptr<IEditorWorkspace>> workspaces;
    workspaces.push_back(std::make_unique<PopupEditor>());
    return workspaces;
}
}
