#include "TD2TvPopups.h"
#include "TD2Popups.h"
#include "TD2Ui.h"
#include "../Common/TD2Math.h"
#include "HIKARI/Project/SceneCaptureView.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iterator>

namespace TD2 {
namespace {
using Rules::Rect;
using Math::Hash01;
// 画布：板一枚分（横 2：縦 1）を縦に三つ並べる。板の縦横もこの比（マスで 2：1）。
constexpr float kBoardWidth = 512, kBoardHeight = 256;
constexpr size_t kBoards = 3, kMaxShown = 2;
constexpr std::string_view kBoardSlot = "TD2_StaticWhite";
// テレビの画面へ描く時と同じ明るさ（画面の材質が光らせるので暗めに渡す）と字の縁。板は舞台に写るが、描き方を受けない
// 結び付け（Unstyled）なので、画面の材質へ直に貼った画布と同じに見える。
constexpr float kCanvasGain = .6f, kCanvasOutline = .09f;
// 本文の字の倍率（窓の幅は板の幅しか無いので、オープニングの全画面の弾窓より字を大きく）。
constexpr float kTextScale = 2.0f;
// 開く・閉じる秒。閉じる時は横線へ潰れてから点へ縮む（ブラウン管が消えるように）。
constexpr double kCloseSeconds = .24;
// 出来事の弾窓が出る場所を待つ秒（過ぎたら出さない）。
constexpr double kQueueSeconds = 8;
// 覚えた操作の案内を閉じる秒（閉じる動きの長さ）。
constexpr double kGuideCloseSeconds = .35;

// 出来事の弾窓が出る場所を待つ秒（過ぎたら出さない）は kQueueSeconds。言葉・きっかけ・頻度は台本（TD2.PopupScript）にある。
// 診断（環境変数 TD2_POPUP_SOON）：時々の弾窓を 1.5 秒後から 3〜4 秒ごとに出す（無頭の実行で姿を撮るため）。
bool Soon() { static const bool soon = std::getenv("TD2_POPUP_SOON") != nullptr; return soon; }

bool Overlap(const Rect& a, const Rect& b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
Rect Inflate(const Rect& r, double pad) { return {r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad}; }

// 絵の名前から素材へ。auto は短い言葉だけに絵を付ける（長い言葉は絵の分だけ字が小さくなる）。
std::string_view IconPath(PopupIcon icon, std::string_view text, double roll) {
    constexpr std::array<std::string_view, 3> kIcons{Popups::kIconQuestion, Popups::kIconWarning, Popups::kIconMail};
    switch (icon) {
    case PopupIcon::None: return {};
    case PopupIcon::Question: return Popups::kIconQuestion;
    case PopupIcon::Warning: return Popups::kIconWarning;
    case PopupIcon::Mail: return Popups::kIconMail;
    case PopupIcon::Auto: break;
    }
    size_t longest = 0;
    for (std::string_view scan = text;;) {
        const auto cut = scan.find('\n');
        longest = (std::max)(longest, Ui::CharCount(scan.substr(0, cut)));
        if (cut == std::string_view::npos) break;
        scan = scan.substr(cut + 1);
    }
    return longest <= 5 ? kIcons[static_cast<size_t>(roll * 3) % 3] : std::string_view{};
}

// 一枚の弾窓（オープニング・通関と同じ窓）。area は画布の中の板一枚分、age は出てからの秒、life は閉じ終わるまでの秒。
void DrawPopup(Ui::Painter& paint, const Ui::Rect& area, std::string_view title, std::string_view text, std::string_view icon, double age, double life) {
    Popups::Look look;
    look.title = title.empty() ? std::string_view("？？？") : title; look.text = text; look.icon = icon; look.textScale = kTextScale;
    // 開ききってから一字ずつ打ち出す。閉じる時は縦に潰れて消える。
    look.typed = (std::max)(0.0, (age - .14) * 26);
    look.appear = Popups::Appear(age);
    if (const double close = (life - age) / kCloseSeconds; close < 1) look.appear = (std::min)(look.appear, static_cast<float>((std::max)(0.0, close)));
    // 窓の高さは幅に比例するので、板の高さに収まる幅にする。余白は開く時の行き過ぎ（1.08 倍）でも隣の板の段へはみ出さず、
    // 切り抜きの縁が板の端に掛からない分。
    constexpr float margin = 11;
    const float perWidth = Popups::Height(100, text, false, kTextScale) / 100;
    const float width = (std::min)(area.w - 2 * margin, (area.h - 2 * margin) / perWidth);
    (void)Popups::Draw(paint, area.x + area.w * .5f, area.y + area.h * .5f, width, look);
    paint.SetAlpha(1);
}
}

void TvPopupDirector::Reset() {
    shown_.clear(); queue_.clear(); said_.clear(); deaths_.clear(); pending_.clear(); history_.clear(); echoAreas_.clear();
    lastArea_.clear(); lastRoom_.clear(); enteredRoom_.clear();
    playTime_ = 0; nextRandom_ = Soon() ? 1.5 : settings_.firstDelay; lastEvent_ = -1e9;
    idleTime_ = roomTime_ = 0; lastDeath_ = -1e9;
    walkTime_ = 0; dashed_ = swapped_ = false;
    moodLast_.fill(-1e9);
    bags_.clear();
    for (auto& board : boards_) { board.shown = false; }
    dirty_ = true;
}

void TvPopupDirector::Restore(const std::vector<std::string>& said) {
    Reset();
    said_.insert(said.begin(), said.end());
    // 案内の記録が無い前の保存：大区域を二つ以上進んでいれば、操作はもう覚えている。
    const auto areas = std::count_if(said_.begin(), said_.end(), [](const std::string& k) { return k.starts_with("area:"); });
    if (areas >= 2) said_.insert({"guide:move", "guide:dash", "guide:swap"});
}

void TvPopupDirector::OnEvent(std::string_view region, const Rules::RuleEvent& event) {
    using Rules::EventKind;
    const std::string id = event.subjectId.find('/') == std::string::npos ? std::string(region) + "/" + event.subjectId : event.subjectId;
    switch (event.kind) {
    case EventKind::CheckpointActivated: pending_.push_back({PopupEvent::Checkpoint, id}); break;
    case EventKind::CollectibleTaken: pending_.push_back({PopupEvent::Gem, id}); break;
    case EventKind::Blink: dashed_ = true; break;
    case EventKind::EchoSwapped: swapped_ = true; break;
    case EventKind::PlayerDied:
        lastDeath_ = playTime_;
        if (++deaths_[std::string(region)] == settings_.deathsToWorry) pending_.push_back({PopupEvent::Worry, std::string(region)});
        break;
    // 開いた装置は扉かどうかを Update で確かめる（出来事には装置の種類が無い）。
    case EventKind::DeviceChanged: if (event.detail == "active") pending_.push_back({PopupEvent::Door, id}); break;
    default: break;
    }
}

void TvPopupDirector::LoadScript(HIKARI::WorldView& world) {
    // シーンの台本（最初の一つ）。無い・読めない時は既定の台本で出す。
    std::string text{PopupText::DefaultScript()};
    bool found = false;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kPopupScriptComponentType)};
    (void)world.ForEachComponents<PopupScriptComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView, PopupScriptComponent& script) {
            if (found) return;
            found = true; text = script.Entries(); settings_ = script.Settings();
        });
    std::string error;
    if (!PopupText::Parse(text, entries_, error, true)) std::fprintf(stderr, "[TD2 popups] script error (line skipped): %s\n", error.c_str());
    // 操作の案内（移動・ダッシュ・入れ替え・入れ替えた）の言葉が台本に無い種類は、既定の台本の言葉で補う（案内が出ないと遊べない）。
    if (found) {
        std::vector<PopupEntry> defaults;
        std::string ignored;
        (void)PopupText::Parse(PopupText::DefaultScript(), defaults, ignored, true);
        for (const PopupEvent kind : {PopupEvent::Move, PopupEvent::Dash, PopupEvent::Swap, PopupEvent::Swapped}) {
            const auto has = [kind](const PopupEntry& e) { return e.trigger == PopupTrigger::Event && e.event == kind; };
            if (std::any_of(entries_.begin(), entries_.end(), has)) continue;
            for (const auto& e : defaults) {
                if (has(e) && std::none_of(entries_.begin(), entries_.end(), [&](const PopupEntry& x) { return x.id == e.id; })) entries_.push_back(e);
            }
        }
    }
    std::fprintf(stderr, "[TD2 popups] script=%s entries=%zu\n", found ? "scene" : "default", entries_.size());
}

bool TvPopupDirector::Acquire(HIKARI::WorldView& world) {
    if (acquired_) return true;
    if (failed_) return false;
    LoadScript(world);
    seed_ = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count() * 2654435761ull) | 1u;
    nextRandom_ = Soon() ? 1.5 : settings_.firstDelay;
    moodLast_.fill(-1e9);
    boards_.clear();
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kPopupBoardComponentType)};
    (void)world.ForEachComponents<PopupBoardComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView object, PopupBoardComponent&) { if (boards_.size() < kBoards) { boards_.push_back({object.GetHandle()}); } });
    const auto canvases = world.GetUiCanvases();
    if (boards_.empty() || !canvases.IsValid()) {
        failed_ = true;
        std::fprintf(stderr, "[TD2 popups] no boards (%zu) or canvas; in-TV popups off\n", boards_.size());
        return false;
    }
    HIKARI::GameProjectUiCanvasDesc desc{};
    desc.width = static_cast<uint32_t>(kBoardWidth); desc.height = static_cast<uint32_t>(kBoardHeight * boards_.size());
    desc.clearColor = {0, 0, 0, 0};
    canvas_ = canvases.Create(desc);
    if (!canvas_.IsValid()) { failed_ = true; std::fprintf(stderr, "[TD2 popups] canvas unavailable\n"); return false; }
    // 板ごとに画布の一段を結ぶ。結び付けは Play の間ずっと保つ（外すと材質が作り直しになる）。窓の外は透明にして切り抜き、
    // 舞台を撮る画面の描き方（線画・色調）は受けない。
    const float rows = static_cast<float>(boards_.size());
    for (size_t i = 0; i < boards_.size(); ++i) {
        HIKARI::GameProjectSceneCaptureMaterialRegionBindingDesc binding{};
        binding.capture = {canvas_.value, canvas_.epoch};
        binding.modelObject = boards_[i].object;
        std::memcpy(binding.materialSlotName, kBoardSlot.data(), kBoardSlot.size());
        binding.sourceRect = {0, static_cast<float>(i) / rows, 1, 1 / rows};
        binding.flags = HIKARI::GameProjectSceneCaptureMaterialBindingFlags(HIKARI::GameProjectSceneCaptureMaterialBindingFlag::AlphaMask) |
                        HIKARI::GameProjectSceneCaptureMaterialBindingFlags(HIKARI::GameProjectSceneCaptureMaterialBindingFlag::Unstyled);
        if (!world.GetSceneCapture().BindMaterialRegion(binding)) {
            failed_ = true;
            std::fprintf(stderr, "[TD2 popups] board %zu binding failed; in-TV popups off\n", i);
            return false;
        }
    }
    acquired_ = true; dirty_ = true;
    std::fprintf(stderr, "[TD2 popups] ready boards=%zu\n", boards_.size());
    return true;
}

bool TvPopupDirector::FindSpot(const TvPopupContext& context, const std::string& region, Rect& out) {
    const auto* room = context.exploration->FindRegion(region);
    if (!room || !room->placement.worldPlaced || !context.snapshot) return false;
    const auto& r = room->definition.region;
    const int cols = r.cols, rows = r.rows;
    const double ox = room->placement.worldX, oy = room->placement.worldY;
    // 地形（規則の向き。回された部屋は作者の地図を点対称に読む）。部屋の外は埋まり扱い。
    const auto solid = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= cols || y >= rows) return true;
        const int ax = room->flipped ? cols - 1 - x : x, ay = room->flipped ? rows - 1 - y : y;
        return r.cells[static_cast<size_t>(ay * cols + ax)] != '0';
    };
    // 避ける所（部屋の左上からのマス）：装置とその通り道、光線、人物、ほかの弾窓。
    std::vector<Rect> blocked;
    const Rect bounds{ox - 1, oy - 1, cols + 2.0, rows + 2.0};
    const auto avoid = [&](Rect b, double pad) {
        if (!Overlap(b, bounds)) return;
        blocked.push_back(Inflate({b.x - ox, b.y - oy, b.w, b.h}, pad));
    };
    using Rules::DeviceKind; using Rules::Direction;
    for (const auto& device : context.snapshot->devices) {
        if (device.kind == DeviceKind::Mechanism || !Overlap(device.rect, bounds)) continue;
        Rect b = device.rect;
        const bool vertical = device.direction == Direction::Up || device.direction == Direction::Down;
        if (device.kind == DeviceKind::Laser || device.kind == DeviceKind::Spring) {
            // 光線は向きの先の部屋の端まで、バネは打ち上げる向きの端まで（光が遮られていない時の道と、飛ぶ道）。
            if (device.direction == Direction::Up) { b.h += b.y - oy; b.y = oy; }
            else if (device.direction == Direction::Down) { b.h = oy + rows - b.y; }
            else if (device.direction == Direction::Left) { b.w += b.x - ox; b.x = ox; }
            else { b.w = ox + cols - b.x; }
        } else if (device.kind == DeviceKind::Pusher) {
            // 押し刺は伸び縮みする道の全部（今の位置から両側へ伸びる長さ）。
            const double reach = device.travel;
            if (vertical) { b.y -= reach; b.h += 2 * reach; } else { b.x -= reach; b.w += 2 * reach; }
        }
        avoid(b, .5);
    }
    for (const auto& beam : context.snapshot->beams) avoid(beam.rect, .5);
    avoid(context.snapshot->presentationPlayer, 1.0);
    if (context.snapshot->echo) avoid(context.snapshot->echo->rect, 1.0);
    if (context.snapshot->pendingEcho) avoid(*context.snapshot->pendingEcho, 1.0);
    for (const auto& shown : shown_) if (shown.region == region) blocked.push_back(Inflate(shown.local, .5));
    // 地形から空ける間の広い順・大きい順に、避ける所に掛からない所を探す。上の方・横の中ほどを好み、良い所の中から選ぶ。
    // 覗き窓は画面を斜めから見るので、手前へ張り出した地形の正面の縁が奥の板の上へずれて重なる（間が半マスでは天井の下の
    // 題の帯が隠れることがある）。狭い部屋では半マスまで詰める。
    struct Size { double w, h; };
    std::vector<Size> sizes{{5, 2.5}, {4, 2}};
    if (cols >= 16) sizes.insert(sizes.begin(), Size{6, 3});
    for (const double clear : {.75, .5}) for (const auto& [w, h] : sizes) {
        std::vector<std::pair<double, Rect>> found;
        for (double y = clear; y + h <= rows - clear + 1e-9; y += .25) {
            for (double x = clear; x + w <= cols - clear + 1e-9; x += .25) {
                const Rect spot{x, y, w, h};
                bool vacant = true;
                const int x0 = static_cast<int>(std::floor(x - clear)), x1 = static_cast<int>(std::ceil(x + w + clear));
                const int y0 = static_cast<int>(std::floor(y - clear)), y1 = static_cast<int>(std::ceil(y + h + clear));
                for (int cy = y0; cy < y1 && vacant; ++cy) for (int cx = x0; cx < x1 && vacant; ++cx) vacant = !solid(cx, cy);
                for (const auto& b : blocked) if (vacant && Overlap(spot, b)) vacant = false;
                if (vacant) found.push_back({y + .1 * std::abs(x + w * .5 - cols * .5), spot});
            }
        }
        if (found.empty()) continue;
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        size_t good = 1;
        while (good < found.size() && found[good].first <= found.front().first + 1.0) ++good;
        out = found[static_cast<size_t>(Hash01(seed_, picks_++) * good) % good].second;
        return true;
    }
    return false;
}

bool TvPopupDirector::Eligible(const PopupEntry& entry, std::string_view region) const {
    if (!entry.room.empty() && entry.room != region) return false;
    return !entry.once || !said_.contains("pop:" + entry.id);
}

bool TvPopupDirector::Recent(std::string_view text) const {
    return std::find(history_.begin(), history_.end(), text) != history_.end();
}

size_t TvPopupDirector::Pick(const std::string& pool, const std::vector<size_t>& candidates) {
    if (candidates.empty()) return kNone;
    // 混ぜた順に一巡するまで同じ言葉を出さない。最近出た言葉は飛ばし、どれも最近なら最近でも出す。
    auto& bag = bags_[pool];
    for (int round = 0; round < 2; ++round) {
        for (auto it = bag.rbegin(); it != bag.rend(); ++it) {
            const size_t index = *it;
            if (std::find(candidates.begin(), candidates.end(), index) == candidates.end() || Recent(entries_[index].text)) continue;
            bag.erase(std::next(it).base());
            return index;
        }
        bag = candidates;
        for (size_t i = bag.size(); i > 1; --i) std::swap(bag[i - 1], bag[static_cast<size_t>(Hash01(seed_ ^ 0xBA6u, picks_++) * i) % i]);
    }
    return candidates[static_cast<size_t>(Hash01(seed_ ^ 0xBA7u, picks_++) * candidates.size()) % candidates.size()];
}

void TvPopupDirector::QueueEvent(PopupEvent event, const std::string& objectKey, const std::string& areaKey, bool important, std::string_view region) {
    // 物の鍵（一つの物に一度）が出し済みなら何もしない。大区域の鍵が出し済みか、前の出来事の弾窓から間が短すぎる時は、
    // 物の鍵だけを出し済みにして出さない（同じ物でもう一度出さず、その大区域の一度は残す）。
    if (!objectKey.empty() && said_.contains(objectKey)) return;
    const auto skip = [&] { if (!objectKey.empty()) said_.insert(objectKey); };
    if ((!areaKey.empty() && said_.contains(areaKey)) || (!important && playTime_ - lastEvent_ < settings_.eventGap)) { skip(); return; }
    std::vector<size_t> candidates;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        if (e.trigger == PopupTrigger::Event && e.event == event && Eligible(e, region)) candidates.push_back(i);
    }
    skip();
    if (candidates.empty()) return;
    if (!areaKey.empty()) said_.insert(areaKey);
    queue_.push_back({kNone, static_cast<int>(event), important, clock_, {}});
}

void TvPopupDirector::Update(HIKARI::WorldView& world, const TvPopupContext& context) {
    if (!context.exploration || !Acquire(world)) { pending_.clear(); return; }
    if (Soon() && clock_ == 0) nextRandom_ = 1.5;
    clock_ += context.seconds;
    if (context.allowed) playTime_ += context.seconds;
    const auto& region = context.exploration->ActiveRegion();
    // その時の様子：止まっている秒と、同じ部屋にいる秒（遊んでいる間だけ数える）。部屋に初めて入った時の言葉は遊べる時に。
    if (region != lastRoom_) { lastRoom_ = region; roomTime_ = 0; }
    if (context.allowed && region != enteredRoom_) {
        enteredRoom_ = region;
        for (size_t i = 0; i < entries_.size(); ++i) {
            const auto& e = entries_[i];
            if (e.trigger != PopupTrigger::Room || e.room != region || said_.contains("room:" + e.id) || !Eligible(e, region)) continue;
            said_.insert("room:" + e.id);
            queue_.push_back({i, -1, true, clock_, region});
            break;
        }
    }
    if (context.allowed) {
        roomTime_ += context.seconds;
        const auto& body = context.snapshot->player;
        const bool still = body.grounded && std::abs(body.walkVx) < .01 && !context.snapshot->dashing && !body.dead;
        idleTime_ = still ? idleTime_ + context.seconds : 0;
    }
    // 新しい大区域に入った（続きから始めた最初の大区域と、最初の机は数えない）。切り替えを終えて遊べるようになってから
    // 数える（切り替えの途中で引き返すと元の大区域のまま。途中で数えると切り替えの間に言葉が捨てられる）。
    if (context.allowed && !context.areaKey.empty() && context.areaKey != lastArea_) {
        const std::string key = "area:" + std::string(context.areaKey);
        if (!lastArea_.empty()) QueueEvent(PopupEvent::Area, key, {}, true, region);
        said_.insert(key);
        lastArea_ = context.areaKey;
    }
    // 規則の出来事（遊べる間だけ。それ以外の間は貯めておく）：チェックポイント・機構は決まりで大区域ごとに一度、扉は大区域
    // ごとに一度、宝石は一つごと。
    if (context.allowed) {
        for (auto& [event, subject] : pending_) {
            if (event == PopupEvent::Checkpoint) {
                QueueEvent(event, "cp:" + subject, settings_.checkpointPerArea ? "cp-area:" + lastArea_ : std::string{}, false, region);
            } else if (event == PopupEvent::Gem) {
                QueueEvent(event, "gem:" + subject, {}, false, region);
            } else if (event == PopupEvent::Worry) {
                QueueEvent(event, "die:" + subject, {}, true, region);
            } else if (event == PopupEvent::Door) {
                const auto device = std::find_if(context.snapshot->devices.begin(), context.snapshot->devices.end(), [&](const auto& d) { return d.id == subject; });
                if (device != context.snapshot->devices.end() && device->kind == Rules::DeviceKind::Door) QueueEvent(event, {}, "door:" + lastArea_, false, region);
            }
        }
        pending_.clear();
        for (const auto& machine : context.exploration->Mechanisms()) {
            if (!machine.moving || machine.source.dwell > 0) continue;
            QueueEvent(PopupEvent::Mechanism, "mech:" + machine.source.id, settings_.mechanismPerArea ? "mech-area:" + lastArea_ : std::string{}, false, region);
        }
    }
    // 操作の案内：移動 → ダッシュ →（残影がいる時）入れ替え。覚えるまで今の部屋に出したままにし、覚えたら閉じる。
    // 初めて入れ替えた時は一度だけ「入れ替えた」の言葉を出す。
    if (context.allowed) {
        walkTime_ = std::abs(context.snapshot->player.walkVx) > .01 ? walkTime_ + context.seconds : walkTime_;
        if (walkTime_ >= .4) said_.insert("guide:move");
        if (dashed_) said_.insert("guide:dash");
        if (swapped_ && !said_.contains("guide:swap")) {
            said_.insert("guide:swap");
            queue_.push_back({kNone, static_cast<int>(PopupEvent::Swapped), true, clock_, {}});
        }
    }
    dashed_ = swapped_ = false;
    int guide = -1;
    if (!said_.contains("guide:move")) guide = static_cast<int>(PopupEvent::Move);
    else if (!said_.contains("guide:dash")) guide = static_cast<int>(PopupEvent::Dash);
    else if (!said_.contains("guide:swap") && context.snapshot->echo) guide = static_cast<int>(PopupEvent::Swap);
    for (auto& s : shown_) {
        if (s.guide < 0 || (s.guide == guide && s.region == region)) continue;
        s.life = (std::min)(s.life, clock_ - s.born + kGuideCloseSeconds);
        s.guide = -1;
    }
    // 閉じ終わった弾窓を外し、出る所を待ちすぎた出来事を捨てる。
    const size_t before = shown_.size();
    std::vector<bool> freed(boards_.size(), false);
    for (const auto& s : shown_) if (clock_ - s.born >= s.life) freed[s.board] = true;
    std::erase_if(shown_, [&](const Shown& s) { return clock_ - s.born >= s.life; });
    if (shown_.size() != before) dirty_ = true;
    std::erase_if(queue_, [&](const Line& line) { return clock_ - line.queued > kQueueSeconds; });
    // 出来事の弾窓どうしの間が短すぎる時は捨てる（大区域・何度も死ぬ・部屋は待つ）。部屋の言葉は、もうその部屋にいなければ捨てる。
    std::erase_if(queue_, [&](const Line& line) {
        return (!line.important && playTime_ - lastEvent_ < settings_.eventGap) || (!line.room.empty() && line.room != region);
    });
    // 新しく出す：出来事を先に、無ければ時々の弾窓（ほかに出ていない時だけ）。プレイヤーのいる部屋が砂嵐の間は待つ。
    const bool snowing = std::find(context.snowRooms.begin(), context.snowRooms.end(), region) != context.snowRooms.end();
    if (context.allowed && context.visible && !snowing && shown_.size() < kMaxShown) {
        // 案内を待っている間は時々の弾窓を出さない（案内が見つけにくくなる）。
        const bool guideNow = guide >= 0 && std::none_of(shown_.begin(), shown_.end(), [&](const Shown& s) { return s.guide == guide; });
        const bool random = guide < 0 && queue_.empty() && shown_.empty() && playTime_ >= nextRandom_;
        size_t chosen = kNone;
        int mood = -1;
        if (guideNow) {
            std::vector<size_t> candidates;
            for (size_t i = 0; i < entries_.size(); ++i) {
                const auto& e = entries_[i];
                if (e.trigger == PopupTrigger::Event && static_cast<int>(e.event) == guide && Eligible(e, region)) candidates.push_back(i);
            }
            chosen = Pick("g" + std::to_string(guide), candidates);
        } else if (!queue_.empty()) {
            auto& line = queue_.front();
            if (line.entry != kNone) chosen = line.entry;
            else {
                std::vector<size_t> candidates;
                for (size_t i = 0; i < entries_.size(); ++i) {
                    const auto& e = entries_[i];
                    if (e.trigger == PopupTrigger::Event && static_cast<int>(e.event) == line.event && Eligible(e, region)) candidates.push_back(i);
                }
                chosen = line.entry = Pick("e" + std::to_string(line.event), candidates);
            }
        } else if (random) {
            // 進んだ大区域の数で、後ろの段の言葉を混ぜる。
            const auto areas = static_cast<int>(std::count_if(said_.begin(), said_.end(), [](const std::string& k) { return k.starts_with("area:"); }));
            const int top = std::clamp(areas / 2, 0, 2);
            const int tier = top == 0 || Hash01(seed_ ^ 0x7E1u, picks_++) < .6 ? top : static_cast<int>(Hash01(seed_ ^ 0x7E2u, picks_++) * top) % top;
            // 当てはまる様子（同じ様子は間を空ける。残影は決まりで大区域ごとに一度）。
            std::vector<int> moods;
            const auto ready = [&](PopupMood m) { return playTime_ - moodLast_[static_cast<size_t>(m)] >= settings_.moodCooldown; };
            if (idleTime_ >= settings_.idleSeconds && ready(PopupMood::Idle)) moods.push_back(static_cast<int>(PopupMood::Idle));
            if (playTime_ - lastDeath_ <= settings_.diedSeconds && ready(PopupMood::Died)) moods.push_back(static_cast<int>(PopupMood::Died));
            if (roomTime_ >= settings_.stuckSeconds && ready(PopupMood::Stuck)) moods.push_back(static_cast<int>(PopupMood::Stuck));
            if (context.snapshot->echo && ready(PopupMood::Echo) && !(settings_.echoPerArea && echoAreas_.contains(lastArea_))) {
                moods.push_back(static_cast<int>(PopupMood::Echo));
            }
            // おまけ：当てはまる段・様子のおまけを混ぜた順に、それぞれの確率で引く。
            std::vector<size_t> eggs;
            for (size_t i = 0; i < entries_.size(); ++i) {
                const auto& e = entries_[i];
                if (e.chance <= 0 || !Eligible(e, region)) continue;
                const bool tierOk = e.trigger == PopupTrigger::Random && (e.tier == PopupTier::Any || static_cast<int>(e.tier) <= top);
                const bool moodOk = e.trigger == PopupTrigger::Mood && std::find(moods.begin(), moods.end(), static_cast<int>(e.mood)) != moods.end();
                if (tierOk || moodOk) eggs.push_back(i);
            }
            for (size_t i = eggs.size(); i > 1; --i) std::swap(eggs[i - 1], eggs[static_cast<size_t>(Hash01(seed_ ^ 0xE66u, picks_++) * i) % i]);
            for (const size_t i : eggs) if (Hash01(seed_ ^ 0xE67u, picks_++) < entries_[i].chance) { chosen = i; break; }
            if (chosen != kNone && entries_[chosen].trigger == PopupTrigger::Mood) mood = static_cast<int>(entries_[chosen].mood);
            if (chosen == kNone && !moods.empty() && Hash01(seed_ ^ 0x7E4u, picks_++) < settings_.moodChance) {
                mood = moods[static_cast<size_t>(Hash01(seed_ ^ 0x7E5u, picks_++) * moods.size()) % moods.size()];
                std::vector<size_t> candidates;
                for (size_t i = 0; i < entries_.size(); ++i) {
                    const auto& e = entries_[i];
                    if (e.trigger == PopupTrigger::Mood && static_cast<int>(e.mood) == mood && e.chance <= 0 && Eligible(e, region)) candidates.push_back(i);
                }
                chosen = Pick("m" + std::to_string(mood), candidates);
                if (chosen == kNone) mood = -1;
            }
            if (chosen == kNone) {
                std::vector<size_t> candidates;
                for (size_t i = 0; i < entries_.size(); ++i) {
                    const auto& e = entries_[i];
                    if (e.trigger == PopupTrigger::Random && e.chance <= 0 && (e.tier == PopupTier::Any || static_cast<int>(e.tier) == tier) && Eligible(e, region)) {
                        candidates.push_back(i);
                    }
                }
                chosen = Pick("r" + std::to_string(tier), candidates);
            }
            // 次の時々の弾窓：先へ進むほど短い間隔で（最長から最短まで、±3 割）。
            const double base = Soon() ? 3.0 : std::clamp(static_cast<double>(settings_.intervalMax) - settings_.intervalPerArea * areas,
                                                          static_cast<double>((std::min)(settings_.intervalMin, settings_.intervalMax)),
                                                          static_cast<double>(settings_.intervalMax));
            nextRandom_ = playTime_ + base * (.7 + .6 * Hash01(seed_ ^ 0x7E3u, picks_++));
        }
        Rect spot{};
        if (chosen != kNone && FindSpot(context, region, spot)) {
            // 使っていない板（このフレームに閉じ終わった板は除く。板は出せる数より一枚多い）。
            std::vector<bool> used = freed;
            for (const auto& s : shown_) used[s.board] = true;
            const auto board = static_cast<size_t>(std::find(used.begin(), used.end(), false) - used.begin());
            if (board < boards_.size()) {
                const auto& entry = entries_[chosen];
                const double life = guideNow ? 1e9 : std::clamp(2.2 + .12 * static_cast<double>(Ui::CharCount(entry.text)), 2.5, 4.0);
                const auto icon = IconPath(entry.icon, entry.text, Hash01(seed_ ^ 0x1C0u, picks_++));
                const auto* room = context.exploration->FindRegion(region);
                shown_.push_back({board, entry.title, entry.text, std::string(icon), region, spot, clock_, life, room && room->flipped,
                                  guideNow ? guide : -1});
                if (!guideNow && !queue_.empty() && !random) { queue_.erase(queue_.begin()); lastEvent_ = playTime_; }
                if (mood >= 0) {
                    moodLast_[static_cast<size_t>(mood)] = playTime_;
                    if (mood == static_cast<int>(PopupMood::Echo)) echoAreas_.insert(lastArea_);
                }
                if (entry.once) said_.insert("pop:" + entry.id);
                history_.push_back(entry.text);
                while (history_.size() > static_cast<size_t>((std::max)(0, settings_.history))) history_.pop_front();
                ++sounds_;
                dirty_ = true;
                std::fprintf(stderr, "[TD2 popups] show region=%s at=(%.1f,%.1f %.1fx%.1f) random=%d id=%s text=%s\n", region.c_str(), spot.x, spot.y,
                             spot.w, spot.h, random, entry.id.c_str(), entry.text.c_str());
            }
        } else if (random) {
            // 置ける所が無い時は少し後にもう一度だけ試す。
            nextRandom_ = playTime_ + 4;
        } else if (!guideNow && !queue_.empty() && chosen == kNone) {
            queue_.erase(queue_.begin());
        }
    }
    // 出ている間に部屋が 180° 回り終えた弾窓は、規則の向きの位置を点対称に直す（舞台の上では同じ所、確かめた所のまま）。
    for (auto& s : shown_) {
        const auto* room = context.exploration->FindRegion(s.region);
        if (!room || room->flipped == s.flipped) continue;
        const auto& g = room->definition.region;
        s.local.x = g.cols - s.local.x - s.local.w; s.local.y = g.rows - s.local.y - s.local.h;
        s.flipped = room->flipped;
    }
    // 板の置き場所。見せない間（通関の演出など）と、部屋が砂嵐の間は退ける。
    for (size_t i = 0; i < boards_.size(); ++i) {
        auto& board = boards_[i];
        const auto s = std::find_if(shown_.begin(), shown_.end(), [i](const Shown& v) { return v.board == i; });
        const bool hidden = s == shown_.end() || !context.visible ||
                            std::find(context.snowRooms.begin(), context.snowRooms.end(), s->region) != context.snowRooms.end();
        board.shown = !hidden;
        if (!hidden) { board.region = s->region; board.local = s->local; }
    }
    if (!shown_.empty() || dirty_) Paint(world);
}

void TvPopupDirector::Paint(HIKARI::WorldView& world) {
    const auto canvases = world.GetUiCanvases();
    if (!canvases.BeginUpdate(canvas_, ++revision_)) return;
    Ui::Painter paint(world, canvas_, kBoardWidth, kBoardHeight * static_cast<float>(boards_.size()), 0);
    paint.SetGain(kCanvasGain); paint.SetTextOutline(kCanvasOutline);
    for (const auto& s : shown_) {
        DrawPopup(paint, {0, kBoardHeight * static_cast<float>(s.board), kBoardWidth, kBoardHeight}, s.title, s.text, s.icon, clock_ - s.born, s.life);
    }
    (void)canvases.CommitUpdate(canvas_);
    dirty_ = false;
}
}
