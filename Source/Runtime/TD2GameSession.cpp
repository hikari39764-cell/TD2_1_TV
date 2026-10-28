#include "TD2GameSession.h"
#include "TD2Ui.h"
#include "../Common/TD2Math.h"
#include "../Common/TD2PreviewEntry.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace TD2 {
namespace {
// 診断用（環境変数 TD2_TEST_ENTRY に区域 ID）：オープニングを飛ばし、その区域の出生点から始める（無窓の検証用）。
// 「区域 ID@x,y」と書けば区域内のその位置から始める（試遊の記録を同じ入口から流し直す時）。
const std::string& TestEntryText() {
    static const std::string text = [] { const char* value = std::getenv("TD2_TEST_ENTRY"); return std::string(value ? value : ""); }();
    return text;
}
const char* TestEntryRegion() {
    static const std::string region = TestEntryText().substr(0, TestEntryText().find('@'));
    return region.empty() ? nullptr : region.c_str();
}
std::optional<Rules::Vec2> TestEntrySpawn() {
    const auto at = TestEntryText().find('@');
    if (at == std::string::npos) { return std::nullopt; }
    double x = 0, y = 0;
    if (std::sscanf(TestEntryText().c_str() + at + 1, "%lf,%lf", &x, &y) != 2) { return std::nullopt; }
    return Rules::Vec2{x, y};
}
}

void GameSession::OnGameStarted(HIKARI::WorldView&) {
    exploration_ = {}; regions_ = {}; camera_.Reset(); machines_ = {}; introChecked_ = ending_ = false;
    menu_ = {}; popups_ = {}; pendingProgress_.reset(); saving_ = checkpointTouched_ = false;
    initialSources_.clear(); initialMachines_ = {};
    sceneActive_ = leftHeld_ = rightHeld_ = false;
    previewEntryObject_ = 0; previewSpawn_.reset(); previewEntryRegion_.clear(); previewStates_.clear();
    recording_ = recordClosed_ = false; record_ = {};
    traceSeconds_ = 0; lastFrame_ = (std::numeric_limits<uint64_t>::max)();
}

void GameSession::OnSceneEntered(HIKARI::WorldView& world) {
    HIKARI::GameProjectSceneEntry entry{};
    previewEntryObject_ = 0; previewSpawn_.reset();
    if (world.GetSceneFlow().ConsumeSceneEntry(entry) && std::string_view(entry.spawnPointId).starts_with(kPreviewEntryPrefix)) {
        const auto spawn = DecodePreviewSpawn(entry.spawnPointId);
        if (!spawn || !entry.userData) { std::fprintf(stderr, "[TD2 configuration] invalid test spawn\n"); return; }
        previewEntryObject_ = entry.userData; previewSpawn_ = Rules::Vec2{spawn->x, spawn->y};
    }
    if (!previewSpawn_ && TestEntryRegion()) { previewSpawn_ = TestEntrySpawn(); }
    sceneActive_ = true; SyncRegions(world);
}

void GameSession::OnSceneExiting(HIKARI::WorldView& world) {
    intro_.Stop(world); introChecked_ = ending_ = false;
    sound_.Stop(world);
    camera_.Stop(world);
    FinishRecord();
    sceneActive_ = false;
}

// 試遊の記録を書き終える（編集器が解法として取り込む）。
void GameSession::FinishRecord() {
    if (!recording_) return;
    recording_ = false; recordClosed_ = true;
    const auto& state = exploration_.State();
    record_.endRegion = exploration_.ActiveRegion();
    record_.endX = state.player.rect.x; record_.endY = state.player.rect.y; record_.endDeaths = state.deaths;
    (void)AppendPlayRecord(LastPlayPath(), record_, recordCursor_, true);
}

void GameSession::SyncRegions(HIKARI::WorldView& world) {
    std::vector<Rules::RegionSource> sources;
    bool definitionsValid = true;
    std::string previewEntry;
    regions_ = {};
    const HIKARI::ComponentTypeId regionTypes[]{HIKARI::MakeComponentTypeId(kRegionComponentType)};
    const auto found = world.ForEachComponents<RegionComponent>(regionTypes, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [this, &sources, &definitionsValid, &previewEntry](HIKARI::ObjectView object, RegionComponent& component) {
            Rules::RegionSource source; source.placement = component.Placement(); source.entry = component.IsEntryRegion();
            const auto& id = source.placement.regionId;
            // まだレベル編集が整えていないテレビ（区域を付けたばかりで ID が既定のまま、またはシーンで複製して ID が重なる）は
            // 世界に入れない。一台のために世界全体を止めない。
            if (id == RegionDefinition{}.regionId || regions_.televisions.contains(id)) {
                std::fprintf(stderr, "[TD2 configuration] skipped unprepared tv object=%llu region=%s\n",
                             static_cast<unsigned long long>(object.GetSceneObjectId()), id.c_str());
                return;
            }
            if (object.GetSceneObjectId() == previewEntryObject_ || (TestEntryRegion() && id == TestEntryRegion())) { previewEntry = id; }
            regions_.televisions[id] = object.GetHandle();
            regions_.cameras[id] = component.CameraObject();
            regions_.geometry[id] = RegionComponent::Surface(object);
            std::string error;
            if (!component.BuildDefinition(source.definition, error) && source.placement.worldPlaced) {
                std::fprintf(stderr, "[TD2 configuration] region=%s error=%s\n", id.c_str(), error.c_str());
                definitionsValid = false;
            }
            sources.push_back(std::move(source));
        });
    if (!found.Succeeded() || !definitionsValid) {
        std::fprintf(stderr, "[TD2 configuration] world rejected query=%u validDefinitions=%d\n", static_cast<unsigned>(found.code), definitionsValid);
        return;
    }
    if (!exploration_.Ready()) {
        // 不正な入口は既定の区域へ落とさずエラーにする。
        if (previewEntryObject_ && previewEntry.empty()) {
            std::fprintf(stderr, "[TD2 configuration] test start object missing: %llu\n", static_cast<unsigned long long>(previewEntryObject_));
            return;
        }
        auto machines = machines_.Sync(world, regions_, sources);
        initialSources_ = sources; initialMachines_ = machines;
        const auto error = exploration_.Initialize(std::move(sources), std::move(machines.mechanisms), std::move(machines.slides),
                                                   std::move(machines.stacks), std::move(machines.carousels), previewEntry, previewSpawn_);
        if (!error.empty()) { std::fprintf(stderr, "[TD2 configuration] %s\n", error.c_str()); return; }
        for (const auto& problem : exploration_.Problems()) std::fprintf(stderr, "[TD2 configuration] disabled %s\n", problem.c_str());
        // 編集器から途中の状態で試遊する時：書いておいた仕掛けの状態へ置く。
        previewEntryRegion_ = previewEntry; previewStates_.clear();
        if (previewEntryObject_) {
            for (const auto& [id, state] : TakePreviewStates()) {
                if (exploration_.SetMechanismState(id, state)) previewStates_.push_back({id, state});
            }
        }
        (void)exploration_.TakeCalls();
        std::fprintf(stderr, "[TD2 world] ready rooms=%zu entry=%s\n", exploration_.Regions().size(), exploration_.ActiveRegion().c_str());
    }
    camera_.SyncBindings(world);
    camera_.SyncAreas(world);
    if (world.GetRuntimeSession().IsPlayActive() && exploration_.Ready() && !camera_.Initialized()) {
        camera_.Initialize(world, exploration_, regions_);
    }
    std::fflush(stderr);
}

void GameSession::Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame) {
    if (!sceneActive_ || !exploration_.Ready() || !world.GetRuntimeSession().IsPlayActive() || frame.paused || lastFrame_ == frame.frameIndex) return;
    lastFrame_ = frame.frameIndex;
    HIKARI::GameProjectInputActionState move{}, action{};
    const auto input = world.GetInput();
    const bool moveValid = input.GetAction("Gameplay.Move", move), actionValid = input.GetAction("Gameplay.Jump", action);
    const bool left = moveValid && move.x < -.1f, right = moveValid && move.x > .1f;
    Rules::InputState buttons;
    buttons.left = {left, left && !leftHeld_, !left && leftHeld_};
    buttons.right = {right, right && !rightHeld_, !right && rightHeld_};
    buttons.action = {actionValid && action.down != 0, actionValid && action.pressed != 0, actionValid && action.released != 0};
    leftHeld_ = left; rightHeld_ = right;
    const double frameSeconds = std::isfinite(frame.gameDt) ? std::clamp(double(frame.gameDt), 0.0, .25) : 0;
    // 本起動だけ、最初の黒と UI
    // 予熱の提示を待つ。途中試遊や検証の入口には起動待ちを挟まない。
    if (!introChecked_ && !previewSpawn_ && previewEntryObject_ == 0 && !TestEntryRegion()) {
        const char* tour = std::getenv("TD2_TOUR");
        if (!(tour && *tour) && !menu_.PrepareStartup(world, frame.frameIndex))
            return;
    }
    // 画の上の窓：最初に設定を読み、毎フレームのキーを読む。窓がスペースを使った後は、離すまで遊びへ渡さない。
    menu_.Start(world);
    menu_.Warm(world);
    menu_.ReadKeys(world, frameSeconds);
    menu_.DrawClosing(world, frameSeconds);
    double remaining = camera_.HoldsWorld() ? 0 : frameSeconds;
    camera_.AdvanceClock(frameSeconds);
    if (!camera_.Initialized()) camera_.Initialize(world, exploration_, regions_);
    // オープニングの演出：最初の Play のフレームで始め（「この画面から」の試遊は除く）、終わるまで規則を進めない。
    // スペースは演出を始めるだけで、ダッシュにはならない。カメラは演出が動かし、焦点だけは画面に合わせて追う。
    static const std::string tour = [] { const char* v = std::getenv("TD2_TOUR"); return std::string(v ? v : ""); }();
    if (!introChecked_) {
        introChecked_ = true;
        const bool test = previewSpawn_.has_value() || previewEntryObject_ != 0 || TestEntryRegion();
        // 遊ぶ人の保存を書くのは普通に始めた時だけ（検証は TD2_SAVE_SLOT で別の枠を使う時だけ書く）。
        saving_ = tour.empty() && (!test || std::getenv("TD2_SAVE_SLOT") != nullptr);
        (void)intro_.Begin(world, test);
        // 続きがあれば、タイトル待ちの上に全画面の問いを出す（はい なら続きから、いいえ ならそのままタイトル）。
        // 隠しの道を通した後の保存なら問わず、机のパソコンの開発者のメールから（「最初から」を選ぶまで毎回そこから）。
        std::string text; Rules::Progress saved;
        if (saving_ && !test && LoadSlot(world, ProgressSlot(), text) && ReadProgressText(text, saved)) {
            if (saved.finale == "end") { RestartFromTitle(world, IntroDirector::Wake::TrueEnd); }
            else { pendingProgress_ = std::move(saved); menu_.OpenContinue(); }
        }
    }
    if (menu_.ContinueOpen()) {
        const auto choice = menu_.UpdateContinue(world, frameSeconds);
        if (choice == MenuDirector::Choice::Yes && pendingProgress_) { (void)ResumeProgress(world, *pendingProgress_); }
        if (choice != MenuDirector::Choice::None) { pendingProgress_.reset(); }
        if (menu_.ContinueOpen() || choice == MenuDirector::Choice::No) {
            menu_.EndFrame(world);
            (void)intro_.Update(world, false, frameSeconds);
            sound_.Update(world, intro_, exploration_.State().player, false, frameSeconds);
            return;
        }
    }
    // 開発用：エディタの Play の時だけ、K でオープニングを飛ばし、O でゴールの部屋へ移る（出荷したゲームでは効かない）。
    if (world.GetRuntimeSession().GetHostKind() == HIKARI::GameProjectRuntimeHostKind::EditorPreview) {
        HIKARI::GameProjectInputActionState skip{}, goal{};
        if (input.GetAction("Gameplay.DevSkipIntro", skip) && skip.pressed) { intro_.Skip(world); }
        if (!ending_ && input.GetAction("Gameplay.DevGoal", goal) && goal.pressed) { intro_.Skip(world); TeleportToGoal(world); }
    }
    // 開発用（環境変数 TD2_TOUR="区域,区域,…"）：TD2_TOUR_FRAMES フレームごとに次の区域の出生点へ移り、カメラもそこから
    // 追従し直す。無頭の実行で区域カメラの写りを順に撮るためのもので、変数が無ければ何もしない。
    if (!tour.empty()) {
        static const int period = [] { const char* v = std::getenv("TD2_TOUR_FRAMES"); return v ? std::max(10, std::atoi(v)) : 90; }();
        if (tourFrame_++ % period == 0) {
            size_t start = 0;
            for (size_t i = 0; i < tourIndex_; ++i) { start = tour.find(',', start); start = start == std::string::npos ? tour.size() : start + 1; }
            const auto end = tour.find(',', start);
            const auto region = start < tour.size() ? tour.substr(start, end == std::string::npos ? std::string::npos : end - start) : std::string{};
            // 出生点は開始の部屋にしか使わないので、ほかの部屋では既定の左上（壁の中）のことが多い。床に立てるマスのうち
            // 部屋の中央に一番近い所へ立たせる（体は幅 1 マス・高さ 2 マスに収まる）。
            std::optional<Rules::Vec2> stand;
            if (const auto* room = exploration_.FindRegion(region)) {
                const auto& r = room->definition.region;
                const auto solid = [&r](int c, int row) { return r.cells[static_cast<size_t>(row * r.cols + c)] == '1'; };
                double best = 1e9;
                for (int row = 1; row < r.rows; ++row) {
                    for (int c = 0; c < r.cols; ++c) {
                        if (!solid(c, row) || solid(c, row - 1) || row < 2 || solid(c, row - 2)) continue;
                        const double d = std::abs(c + .5 - r.cols * .5) + .5 * std::abs(row - r.rows * .5);
                        if (d < best) { best = d; stand = Rules::Vec2{c + .19, row - kPlayerHeight}; }
                    }
                }
            }
            if (!region.empty() && exploration_.Teleport(region, stand ? &*stand : nullptr)) {
                intro_.Skip(world); camera_.Jump(world, exploration_, regions_, region);
                std::fprintf(stderr, "[TD2 tour] frame=%llu region=%s\n", static_cast<unsigned long long>(frame.frameIndex), region.c_str());
            }
            ++tourIndex_;
        }
    }
    // タイトル待ちのパソコンのメール。スペースはメールの決定にだけ使う。目覚めでは、まぶたが開いてからメールが黒い画面に
    // 浮かび上がり、顔を上げ終えてから選べる。隠しの道を通した後は開発者のメール（「最初から」で保存を消して最初へ）。
    bool startIntro = false;
    {
        const auto& screens = intro_.Screens();
        const bool titleWait = intro_.TitleWait();
        if (titleWait || menu_.TitleLeaving()) {
            const double wake = intro_.WakeSeconds();
            const float alpha = wake >= 0 ? static_cast<float>(std::clamp((wake - 1.8) / .6, 0.0, 1.0)) : 1.f;
            const bool interactive = titleWait && (wake < 0 || wake >= 3.6);
            const bool developer = intro_.TrueEnd();
            const auto choice = menu_.UpdateTitle(world, world.FindSceneObject(screens.computerScreen), alpha, interactive, frameSeconds, developer);
            if (choice == MenuDirector::Choice::Yes && developer) {
                if (saving_) { DeleteSlot(world, ProgressSlot()); }
                RestoreTelevisions(world);
                RestartFromTitle(world);
            } else if (choice == MenuDirector::Choice::Yes) { startIntro = true; }
            if (choice == MenuDirector::Choice::No) { (void)world.GetRuntimeSession().RequestEndSession(); }
        }
        if (titleWait || menu_.HoldsAction()) { buttons.action = {}; }
    }
    const bool cinematic = intro_.Update(world, startIntro, frameSeconds);
    // 登場の演出が終わったフレーム：最後に映した画から追従を始める（演出の終わりの画と追従の位置の差で跳ばない）。
    if (intro_.TakeHandoff()) { camera_.AdoptView(world); }
    // 通関の弾窓：暗転を始めたら灯を落とし、終わり方に合わせて目覚め・隠しの道へ移る。
    if (finale_.Active()) {
        bool blackOut = false;
        const auto outcome = finale_.Update(world, menu_, intro_.Screens().time, frameSeconds, blackOut);
        if (blackOut) { intro_.BlackOut(); }
        if (outcome == FinaleDirector::Outcome::WakeNormal) { intro_.EndEnding(world); RestartFromTitle(world, IntroDirector::Wake::Normal); }
        else if (outcome == FinaleDirector::Outcome::EnterHidden) { intro_.EndEnding(world); EnterHidden(world); }
        else if (outcome == FinaleDirector::Outcome::WakeTrue) { intro_.EndEnding(world); RestartFromTitle(world, IntroDirector::Wake::TrueEnd); }
    }
    sound_.PlayPopups(world, finale_.TakeSounds() + intro_.TakePopupSounds());
    // 隠しの道へ移った直後：黒から 0.9 秒で明ける。
    if (hiddenFade_ >= 0) {
        hiddenFade_ += (std::min)(frameSeconds, 1.0 / 30);
        const double k = std::clamp(hiddenFade_ / .9, 0.0, 1.0);
        Ui::Painter fade(world, 160000);
        fade.Fill({0, 0, fade.Width(), fade.Height()}, {0, 0, 0, static_cast<float>(1 - Math::SmoothStep(k))});
        if (k >= 1) { hiddenFade_ = -1; }
    }
    // ESC の窓：遊んでいる間だけ開く。開いている間は世界を止める（カメラは止めたまま描き、音は続ける）。
    if (!cinematic && !ending_ && !menu_.PauseOpen() && menu_.MenuPressed()) { menu_.OpenPause(); }
    const bool paused = !cinematic && menu_.PauseOpen();
    if (paused) {
        // 窓は今いる部屋のテレビの画面の絵として出す。
        const auto result = menu_.UpdatePause(world, world.FindObject(regions_.Television(exploration_.ActiveRegion())), frameSeconds, saving_);
        if (result == MenuDirector::PauseResult::Quit) {
            if (menu_.QuitSave() && saving_) { WriteProgress(world); }
            (void)world.GetRuntimeSession().RequestEndSession();
        }
        remaining = 0;
    }
    menu_.EndFrame(world);
    // テレビの中の弾窓：遊んでいる間だけ新しく出し（演出・窓・大区域の切り替えの間は出さない）、タイトル・オープニング・
    // 通関の演出の間は退ける。出るたびに弾窓の音を小さく鳴らす。
    {
        TvPopupContext context;
        context.exploration = &exploration_; context.snapshot = &exploration_.Snapshot();
        const auto area = camera_.AreaKey(exploration_.ActiveRegion());
        context.areaKey = area;
        std::string from, to;
        // 隠しの道（さいごのおねがい）には出さない。
        context.allowed = !cinematic && !paused && !ending_ && !hiddenRoute_ && hiddenFade_ < 0 && !camera_.TravelRooms(from, to);
        context.visible = !ending_ && !hiddenRoute_ && !finale_.Active() && !intro_.Screens().active;
        context.snowRooms = machines_.SnowRooms();
        context.seconds = paused ? 0 : frameSeconds;
        popups_.Update(world, context);
        sound_.PlayPopups(world, popups_.TakeSounds(), .45f);
    }
    // 音：段ごとの BGM・環境音の受け渡しと足音（演出の間も毎フレーム）。
    sound_.Update(world, intro_, exploration_.State().player, !cinematic && remaining > 0, frameSeconds);
    if (paused) {
        camera_.Update(world, exploration_, regions_, 0.f, 0.0, 0.0);
        return;
    }
    if (cinematic) {
        camera_.Update(world, exploration_, regions_, 0.f, 0.0, std::isfinite(frame.gameDt) ? std::max(0.0, double(frame.gameDt)) : 0.0);
        return;
    }
    // 試遊の記録：このフレームの秒とボタン（空格は押した・離した瞬間も）、世界が止まっていたか。
    if (!recording_ && !recordClosed_) {
        record_ = {}; recordCursor_ = {}; recording_ = true; recordFrame_ = 0;
        // 入口は実際に始めた区域と区域内の位置で残す（作者が開始テレビを変えても解法は同じ所から再生する）。
        record_.states = previewStates_;
        recordRegion_ = exploration_.ActiveRegion(); record_.entryRegion = recordRegion_;
        if (const auto* start = exploration_.FindRegion(recordRegion_)) {
            const auto& body = exploration_.State().player.rect;
            record_.spawn = std::pair{body.x - start->placement.worldX, body.y - start->placement.worldY};
        }
        record_.regions.push_back({0, recordRegion_});
    }
    if (recording_) record_.frames.push_back({frameSeconds, static_cast<uint8_t>((left ? 1 : 0) | (right ? 2 : 0) | (buttons.action.held ? 4 : 0) | (remaining <= 0 ? 8 : 0) |
                                                                  (buttons.action.pressed ? 16 : 0) | (buttons.action.released ? 32 : 0))});
    uint32_t sliceIndex = 0;
    std::string goalRegion;
    const auto takeCalls = [this, &sliceIndex] {
        for (auto& call : exploration_.TakeCalls()) if (recording_) record_.calls.push_back({recordFrame_, sliceIndex, call.kind, call.region, call.direction});
    };
    while (remaining > 1e-12) {
        const double slice = std::min(remaining, 1.0 / 120);
        const auto output = exploration_.Step(buttons, slice);
        for (const auto& region : output.regions)
            for (const auto& event : region.output.events)
                if (recording_ && event.kind == Rules::EventKind::PlayerDied) record_.deaths.push_back({recordFrame_, region.regionId});
        for (const auto& region : output.regions)
            for (const auto& event : region.output.events)
                if (event.kind == Rules::EventKind::GoalReached && goalRegion.empty()) goalRegion = region.regionId;
        for (const auto& region : output.regions)
            for (const auto& event : region.output.events)
                if (event.kind == Rules::EventKind::CheckpointActivated) checkpointTouched_ = true;
        for (const auto& region : output.regions)
            for (const auto& event : region.output.events)
                if (event.kind == Rules::EventKind::CollectibleTaken) {
                    const auto count = exploration_.Collectibles();
                    std::fprintf(stderr, "[TD2 gem] taken=%d total=%d at=%s\n", count.taken, count.total, region.regionId.c_str());
                }
        for (const auto& region : output.regions)
            for (const auto& event : region.output.events) {
                sound_.OnEvent(world, event);
                popups_.OnEvent(region.regionId, event);
                const auto name = Rules::EventName(event.kind);
                std::fprintf(stderr, "[TD2 event] region=%s event=%.*s subject=%s x=%.4f y=%.4f sim=%.4f detail=%s\n", region.regionId.c_str(),
                    static_cast<int>(name.size()), name.data(), event.subjectId.c_str(), event.position.x, event.position.y, event.simulationTime,
                    event.detail.c_str());
            }
        for (const auto& transition : output.transitions) {
            std::fprintf(stderr, "[TD2 crossing] kind=%s from=%s to=%s world=(%.5f,%.5f) velocity=(%.5f,%.5f) frame=%llu\n",
                transition.kind.c_str(), transition.fromRegion.c_str(), transition.toRegion.c_str(), transition.worldPosition.x, transition.worldPosition.y,
                exploration_.State().player.vx, exploration_.State().player.vy, static_cast<unsigned long long>(frame.frameIndex));
            camera_.OnCrossing(world, exploration_, regions_, transition);
        }
        takeCalls();
        ++sliceIndex;
        buttons.action.pressed = buttons.action.released = false; remaining -= slice;
        // ゴールの扉に入ったら、その刻みで世界を止めて通関の演出へ移る。カメラの姿勢はフレームごとの提出なので、このフレームも
        // 出す（出さないとカメラの物体の置いたままの姿勢で 1 フレーム描かれ、別の場所が一瞬写る）。
        if (!goalRegion.empty()) {
            StartEnding(world, goalRegion);
            camera_.Update(world, exploration_, regions_, 0.f, 0.0, std::isfinite(frame.gameDt) ? std::max(0.0, double(frame.gameDt)) : 0.0);
            return;
        }
    }
    // チェックポイントに触れたら、その刻みの後の進みを保存する。
    if (std::exchange(checkpointTouched_, false) && saving_) { WriteProgress(world); }
    machines_.Update(world, exploration_, regions_, camera_, frameSeconds);
    camera_.Update(world, exploration_, regions_, moveValid ? move.x : 0.f, frameSeconds,
                   std::isfinite(frame.gameDt) ? std::max(0.0, double(frame.gameDt)) : 0.0);
    takeCalls();
    if (recording_ && exploration_.ActiveRegion() != recordRegion_) { recordRegion_ = exploration_.ActiveRegion(); record_.regions.push_back({recordFrame_, recordRegion_}); }
    ++recordFrame_;
    // 終わりの呼び出しが来ない止め方でも残るよう、10 秒ごとに続きを書き足す。長い試遊は 20 分で記録を締める。
    if (recording_ && recordFrame_ >= 72000) { FinishRecord(); }
    else if (recording_ && recordFrame_ % 600 == 0) { (void)AppendPlayRecord(LastPlayPath(), record_, recordCursor_, false); }
    traceSeconds_ += std::max(0.0, double(frame.gameDt));
    Trace(world, frame.frameIndex);
}

void GameSession::StartEnding(HIKARI::WorldView& world, const std::string& region) {
    FinishRecord();
    // 普通の通関の後に続きは無い（次の起動はタイトルから）。隠しの道の通関の後は、次の起動も机の開発者のメールから。
    const bool trueEnd = hiddenRoute_;
    if (saving_) { if (trueEnd) { WriteProgress(world, "end"); } else { DeleteSlot(world, ProgressSlot()); } }
    const auto goal = regions_.Television(region);
    intro_.BeginEnding(world, goal);
    finale_.Begin(trueEnd ? FinaleDirector::Kind::TrueEnding : FinaleDirector::Kind::Ending, goal);
    ending_ = true;
    std::fprintf(stderr, "[TD2 ending] goal region=%s true=%d\n", region.c_str(), trueEnd);
}

void GameSession::RestartFromTitle(HIKARI::WorldView& world, IntroDirector::Wake wake) {
    RestoreTelevisions(world);
    ResetWorld(world);
    // 試遊の記録は最初の一回だけ（戻った後の分は残さない）。
    recording_ = false; recordClosed_ = true;
    introChecked_ = true;
    hiddenRoute_ = false; hiddenFade_ = -1;
    menu_.ResetTitle();
    (void)intro_.Begin(world, false, wake);
    if (wake == IntroDirector::Wake::TrueEnd) { HideOtherTelevisions(world); }
    std::fprintf(stderr, "[TD2 world] restarted from title entry=%s wake=%d\n", exploration_.ActiveRegion().c_str(), static_cast<int>(wake));
}

void GameSession::EnterHidden(HIKARI::WorldView& world) {
    ending_ = false;
    // 入口は TD2.Intro の「隠しステージの入口」のテレビの部屋（その部屋の出生点に立つ）。
    const auto entry = world.FindSceneObject(intro_.Settings().hiddenEntry);
    std::string region;
    for (const auto& [id, handle] : regions_.televisions) { if (entry.IsValid() && handle == entry.GetHandle()) { region = id; break; } }
    if (region.empty() || !exploration_.Teleport(region)) {
        std::fprintf(stderr, "[TD2 hidden] entry region missing; back to title\n");
        RestartFromTitle(world, IntroDirector::Wake::Normal);
        return;
    }
    (void)exploration_.TakeCalls();
    hiddenRoute_ = true; hiddenFade_ = 0;
    camera_.Jump(world, exploration_, regions_, region);
    // 隠しの道の途中で止めても、次の起動は続きの問いからここへ戻れる。
    if (saving_) { WriteProgress(world); }
    std::fprintf(stderr, "[TD2 hidden] entered region=%s\n", region.c_str());
}

void GameSession::HideOtherTelevisions(HIKARI::WorldView& world) {
    RestoreTelevisions(world);
    const auto computer = world.FindSceneObject(intro_.Settings().computerScreen);
    for (const auto& handle : intro_.Screens().tvScreens) {
        const auto object = world.FindObject(handle);
        HIKARI::GameProjectTransform pose{};
        if (!object.IsValid() || (computer.IsValid() && handle == computer.GetHandle()) || !object.GetWorldTransform(pose)) { continue; }
        movedTelevisions_.push_back({handle, pose});
        // 床の下の遠くで、ほとんど大きさの無い姿にする（消すのではなく退ける。戻す時は元の姿勢）。
        auto away = pose;
        away.position.y -= 80; away.scale = {1e-4f, 1e-4f, 1e-4f};
        (void)object.SetWorldTransform(away);
    }
    std::fprintf(stderr, "[TD2 world] televisions moved away=%zu\n", movedTelevisions_.size());
}

void GameSession::RestoreTelevisions(HIKARI::WorldView& world) {
    for (const auto& [handle, pose] : movedTelevisions_) {
        if (const auto object = world.FindObject(handle); object.IsValid()) { (void)object.SetWorldTransform(pose); }
    }
    movedTelevisions_.clear();
}

void GameSession::ResetWorld(HIKARI::WorldView& world) {
    ending_ = false;
    auto sources = initialSources_;
    auto machines = initialMachines_;
    exploration_ = {};
    popups_.Reset();
    const auto error = exploration_.Initialize(std::move(sources), std::move(machines.mechanisms), std::move(machines.slides),
                                               std::move(machines.stacks), std::move(machines.carousels));
    if (!error.empty()) { std::fprintf(stderr, "[TD2 configuration] restart failed: %s\n", error.c_str()); return; }
    (void)exploration_.TakeCalls();
    camera_.Stop(world); camera_.Reset();
    camera_.SyncBindings(world); camera_.SyncAreas(world);
    camera_.Initialize(world, exploration_, regions_);
    // 動かした仕掛けのテレビを最初の場所へ書き戻す（棚や塔は止まっている間は書かないので、まとめて書かせる）。
    machines_.Resync();
    machines_.Update(world, exploration_, regions_, camera_, 0.0);
}

bool GameSession::ResumeProgress(HIKARI::WorldView& world, const Rules::Progress& progress) {
    intro_.Skip(world);
    const auto error = exploration_.RestoreProgress(progress);
    if (!error.empty()) {
        std::fprintf(stderr, "[TD2 save] restore failed: %s\n", error.c_str());
        // 今の関卡に合わない保存（部屋を消した・重なる）は消し、次の起動で同じ問いを出さない。
        if (saving_) { DeleteSlot(world, ProgressSlot()); }
        RestartFromTitle(world);
        return false;
    }
    (void)exploration_.TakeCalls();
    hiddenRoute_ = progress.finale == "hidden";
    // 仕掛けのテレビを戻した位置へ置いてから、カメラをその部屋へ付け直す。
    machines_.Resync();
    machines_.Update(world, exploration_, regions_, camera_, 0.0);
    camera_.Jump(world, exploration_, regions_, exploration_.ActiveRegion());
    sound_.NoteCheckpoint(progress.checkpoint);
    popups_.Restore(progress.said);
    // 続きから遊んだ分は解法の記録にしない。
    recording_ = false; recordClosed_ = true;
    std::fprintf(stderr, "[TD2 save] resumed region=%s checkpoint=%s gems=%zu\n", exploration_.ActiveRegion().c_str(),
                 progress.checkpoint.c_str(), progress.taken.size());
    return true;
}

void GameSession::WriteProgress(HIKARI::WorldView& world, std::string_view finale) {
    auto progress = exploration_.SaveProgress();
    progress.finale = !finale.empty() ? std::string(finale) : hiddenRoute_ ? "hidden" : "";
    progress.said = popups_.Said();
    (void)StoreSlot(world, ProgressSlot(), WriteProgressText(progress));
}

void GameSession::TeleportToGoal(HIKARI::WorldView& world) {
    std::string target;
    Rules::Vec2 at{};
    for (const auto& [id, room] : exploration_.Regions()) {
        if (!room.placement.worldPlaced) continue;
        const auto goal = std::find_if(room.definition.devices.begin(), room.definition.devices.end(), [](const auto& d) { return d.kind == Rules::DeviceKind::Goal; });
        if (goal == room.definition.devices.end()) continue;
        // 扉の左に立たせる（出生点は扉から遠いことがある。扉が部屋の左端なら扉の中）。
        const Rules::Rect body = exploration_.State().player.rect;
        target = id; at = {(std::max)(0.0, goal->rect.x - 1.0), goal->rect.y + goal->rect.h - body.h};
        break;
    }
    if (target.empty() || !exploration_.Teleport(target, &at)) { std::fprintf(stderr, "[TD2 dev] no goal room\n"); return; }
    camera_.Jump(world, exploration_, regions_, target);
    // 瞬間移動した試遊は解法にならないので、記録を書き終えずに捨てる（編集器は書き終わった記録だけ読む）。
    recording_ = false; recordClosed_ = true;
    std::fprintf(stderr, "[TD2 dev] teleported to goal region=%s\n", target.c_str());
}

// 状態の記録は 1 秒ごと（検証で細かく見たい時は TD2_TRACE_SECONDS で間隔を変える）。
void GameSession::Trace(HIKARI::WorldView& world, uint64_t frame) {
    static const double traceInterval = std::getenv("TD2_TRACE_SECONDS") ? std::atof(std::getenv("TD2_TRACE_SECONDS")) : 1.0;
    if (traceSeconds_ < traceInterval) return;
    traceSeconds_ = 0;
    const auto& state = exploration_.State();
    std::fprintf(stderr, "[TD2 world-state] region=%s tick=%llu sim=%.4f real=%.4f player=(%.5f,%.5f) velocity=(%.5f,%.5f) echo=%d echoPos=(%.5f,%.5f) deaths=%llu checkpoint=%s travel=%s progress=%.3f frame=%llu\n",
        exploration_.ActiveRegion().c_str(), static_cast<unsigned long long>(state.tick), state.simulationTime, state.realTime,
        state.player.rect.x, state.player.rect.y, state.player.vx, state.player.vy, state.echo.has_value(), state.echo ? state.echo->rect.x : 0,
        state.echo ? state.echo->rect.y : 0, static_cast<unsigned long long>(state.deaths), state.checkpointId.c_str(),
        camera_.TravelLabel().c_str(), camera_.TravelProgress(), static_cast<unsigned long long>(frame));
    const auto prefix = exploration_.ActiveRegion() + "/";
    for (const auto& device : exploration_.Snapshot().devices) if (device.id.starts_with(prefix))
        std::fprintf(stderr, "[TD2 device] region=%s id=%s active=%d pressed=%d solid=%d\n", exploration_.ActiveRegion().c_str(),
            device.id.c_str() + prefix.size(), device.active, device.pressed, device.solid);
    camera_.TraceSequence(world, frame);
    std::fflush(stderr);
}

}
