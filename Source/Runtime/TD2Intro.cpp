#include "TD2Intro.h"
#include "TD2RegionComponent.h"
#include "TD2StageLight.h"
#include "TD2Transform.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/ComponentViews.h"
#include "HIKARI/Project/EnvironmentView.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/LightTypes.h"
#include "HIKARI/Project/SequenceView.h"
#include "HIKARI/Project/CameraViewSnapshot.h"
#include "HIKARI/Project/UiTypes.h"
#include "HIKARI/Project/UiView.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace TD2 {
namespace {
// UI の論理の大きさ（取れなければ 1920×1080）。黒帯・タイトル・まぶたの帯をこの大きさで置く。
std::pair<float, float> UiSize(HIKARI::WorldView& world) {
    HIKARI::GameProjectCameraViewSnapshot view{};
    if (world.GetCamera().TryGetViewSnapshot(view) && view.uiLogicalWidth > 0 && view.uiLogicalHeight > 0) {
        return {static_cast<float>(view.uiLogicalWidth), static_cast<float>(view.uiLogicalHeight)};
    }
    return {1920.f, 1080.f};
}
// シーンの物体の ID を探す上限（灯と装飾テレビをオープニングの始めに一度だけ集める）。
constexpr uint64_t kScanLimit = 40000;
// 0〜1 の決まった乱数（灯ごと・刻みごとに同じ値）。
double Hash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (h & 0xFFFFFFu) / double(0x1000000u);
}
// 刻みの間をなめらかにつないだ乱数（frequency 回/秒）。
double Noise(uint32_t seed, double t, double frequency) {
    const double x = (std::max)(0.0, t) * frequency;
    const auto i = static_cast<uint32_t>(std::floor(x));
    const double f = x - std::floor(x), s = f * f * (3 - 2 * f);
    return Hash(seed, i) * (1 - s) + Hash(seed, i + 1) * s;
}
// タイトルの点灯（点・線・開く）と消灯（閉じる・線・点）にかかる秒。タイトルカードの範囲からこれを引いた残りが映っている時間。
constexpr double kTitleOnSeconds = .42, kTitleOffSeconds = .36;
// 画面に飛び込んで切れてから灯とテレビを戻すまで（暗転の中で戻し、戻る瞬間を見せない）。
constexpr float kRestoreAfterBlack = .06f;
}

// パソコンからの遠さ（0 が手元、1 が工場の奥）。砂嵐の広がりと吸い込みの順に使う。
double IntroReach(double distance) { return std::pow(std::clamp((distance - .4) / 24.0, 0.0, 1.0), .7); }

IntroTimeline ReadIntroTimeline(const std::vector<HIKARI::GameProjectSequenceEventClip>& clips, std::string& missing) {
    IntroTimeline t;
    std::set<std::string, std::less<>> seen;
    for (const auto& clip : clips) {
        const std::string_view name(clip.name, strnlen(clip.name, sizeof(clip.name)));
        const float start = clip.startSeconds, length = clip.durationSeconds, end = start + length;
        seen.emplace(name);
        if (name == IntroEvent::TitleOff) { t.titleOff = start; }
        else if (name == IntroEvent::Omen) { t.flickerStart = start; }
        else if (name == IntroEvent::Lightning) { t.lightning.push_back(start); }
        else if (name == IntroEvent::Static) { t.staticStart = start; t.staticFull = end; }
        else if (name == IntroEvent::Blackout) { t.blackoutStart = start; }
        else if (name == IntroEvent::White) { t.whiteStart = start; }
        else if (name == IntroEvent::Collapse) { t.suckStart = start; t.collapseSeconds = length; }
        else if (name == IntroEvent::Alone) { t.aloneEnd = end; }
        else if (name == IntroEvent::Pull) { t.pullStart = start; t.blackTime = end; t.restoreTime = end + kRestoreAfterBlack; }
        else if (name == IntroEvent::TitleCard) { t.titleIn = start; t.titleHold = static_cast<float>((std::max)(0.0, length - kTitleOnSeconds - kTitleOffSeconds)); }
        else if (name == IntroEvent::Reveal) { t.revealTime = start; }
        else if (name == IntroEvent::Power) { t.powerOn = start; t.powerSeconds = length; }
        else if (name == IntroEvent::Lock) { t.lockStart = start; t.lockSeconds = length; }
        else if (name == IntroEvent::Arrive) { t.arriveTime = start; }
        else if (name == IntroEvent::Letterbox) { t.letterboxIn = start; t.letterboxOut = end; }
    }
    std::sort(t.lightning.begin(), t.lightning.end());
    missing.clear();
    for (const auto name : {IntroEvent::TitleOff, IntroEvent::Omen, IntroEvent::Lightning, IntroEvent::Static, IntroEvent::Blackout,
                            IntroEvent::White, IntroEvent::Collapse, IntroEvent::Alone, IntroEvent::Pull, IntroEvent::TitleCard,
                            IntroEvent::Reveal, IntroEvent::Power, IntroEvent::Lock, IntroEvent::Arrive, IntroEvent::Letterbox}) {
        if (!seen.contains(name)) { missing += std::string(name) + " "; }
    }
    return t;
}

IntroTimeline ReadEndingTimeline(const std::vector<HIKARI::GameProjectSequenceEventClip>& clips, std::string& missing) {
    IntroTimeline t;
    std::set<std::string, std::less<>> seen;
    for (const auto& clip : clips) {
        const std::string_view name(clip.name, strnlen(clip.name, sizeof(clip.name)));
        const float start = clip.startSeconds, length = clip.durationSeconds, end = start + length;
        seen.emplace(name);
        if (name == EndingEvent::Close) { t.closeStart = start; }
        else if (name == IntroEvent::Omen) { t.flickerStart = start; }
        else if (name == IntroEvent::Lightning) { t.lightning.push_back(start); }
        else if (name == IntroEvent::Collapse) { t.suckStart = start; t.collapseSeconds = length; }
        else if (name == EndingEvent::Black) { t.blackTime = start; }
        else if (name == EndingEvent::Wake) { t.revealTime = start; t.wakeEnd = end; }
        else if (name == IntroEvent::Letterbox) { t.letterboxIn = start; t.letterboxOut = end; }
    }
    std::sort(t.lightning.begin(), t.lightning.end());
    missing.clear();
    for (const auto name : {EndingEvent::Close, IntroEvent::Omen, IntroEvent::Lightning, IntroEvent::Collapse, EndingEvent::Black, EndingEvent::Wake}) {
        if (!seen.contains(name)) { missing += std::string(name) + " "; }
    }
    // 扉が閉じ始める瞬間、主人公の足元に登場の光を出し、その中へ消えていく（画面の表示が数える）。
    t.arriveTime = t.closeStart;
    return t;
}

bool IntroDirector::ReadSettings(HIKARI::WorldView& world) {
    bool found = false;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kIntroComponentType)};
    (void)world.ForEachComponents<IntroComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView, IntroComponent& intro) { if (!found) { settings_ = intro.Settings(); found = true; } });
    return found;
}

bool IntroDirector::Prepare(HIKARI::WorldView& world, HIKARI::GameProjectSequencePlaybackHandle playback) {
    screens_ = {}; lamps_.clear(); props_.clear(); screenCenter_ = {}; ending_ = false;
    std::vector<HIKARI::GameProjectSequenceEventClip> clips(HIKARI::kGameProjectSequenceMaximumEventClips);
    const auto sequence = world.GetSequence();
    clips.resize((std::min)(clips.size(), size_t(sequence.GetEventClips(playback, clips.data(), static_cast<uint32_t>(clips.size())))));
    std::string missing;
    static_cast<IntroTimeline&>(screens_) = ReadIntroTimeline(clips, missing);
    if (!missing.empty()) { std::fprintf(stderr, "[TD2 intro] events missing: %s\n", missing.c_str()); }
    playback_ = playback;
    Collect(world);
    ScheduleCollapse(world);
    screens_.active = true; screens_.time = 0;
    screens_.computerScreen = settings_.computerScreen;
    return true;
}

void IntroDirector::PrepareEnding(HIKARI::WorldView& world, HIKARI::GameProjectSequencePlaybackHandle playback,
                                  HIKARI::GameProjectObjectHandle goal) {
    screens_ = {}; lamps_.clear(); props_.clear(); screenCenter_ = {}; ending_ = true;
    std::vector<HIKARI::GameProjectSequenceEventClip> clips(HIKARI::kGameProjectSequenceMaximumEventClips);
    const auto sequence = world.GetSequence();
    clips.resize((std::min)(clips.size(), size_t(sequence.GetEventClips(playback, clips.data(), static_cast<uint32_t>(clips.size())))));
    std::string missing;
    static_cast<IntroTimeline&>(screens_) = ReadEndingTimeline(clips, missing);
    if (!missing.empty()) { std::fprintf(stderr, "[TD2 ending] events missing: %s\n", missing.c_str()); }
    playback_ = playback;
    screens_.goalScreen = goal.value;
    CenterOn(world, goal);
    Collect(world);
    ScheduleCollapse(world);
    screens_.active = true; screens_.ending = true; screens_.time = 0;
    screens_.computerScreen = settings_.computerScreen;
}

void IntroDirector::CenterOn(HIKARI::WorldView& world, HIKARI::GameProjectObjectHandle handle) {
    // テレビの縮尺と向きを掛けたモデルの画面の中心（世界座標）。
    const auto screen = world.FindObject(handle);
    HIKARI::GameProjectTransform tv{};
    if (!screen.IsValid() || !screen.GetWorldTransform(tv)) { return; }
    const auto g = RegionComponent::Surface(screen);
    const auto local = QRotate(tv.rotation, {g.centerX * tv.scale.x, g.centerY * tv.scale.y, g.centerZ * tv.scale.z});
    screenCenter_ = {tv.position.x + local.x, tv.position.y + local.y, tv.position.z + local.z};
}

HIKARI::GameProjectObjectHandle IntroDirector::FindGoalScreen(HIKARI::WorldView& world) const {
    HIKARI::GameProjectObjectHandle goal{};
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kRegionComponentType)};
    (void)world.ForEachComponents<RegionComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView screen, RegionComponent& region) {
            Rules::WorldDefinition definition; std::string error;
            if (goal.IsValid() || !region.BuildDefinition(definition, error)) { return; }
            if (std::any_of(definition.devices.begin(), definition.devices.end(), [](const auto& d) { return d.kind == Rules::DeviceKind::Goal; })) {
                goal = screen.GetHandle();
            }
        });
    return goal;
}

bool IntroDirector::BeginEnding(HIKARI::WorldView& world, HIKARI::GameProjectObjectHandle goalScreen, uint64_t goalCamera,
                                HIKARI::GameProjectFloat3 eye, HIKARI::GameProjectFloat3 look) {
    if (!ReadSettings(world) || settings_.ending.empty()) { return false; }
    const auto camera = world.FindSceneObject(settings_.cameraObject);
    if (!camera.IsValid() || !world.FindObject(goalScreen).IsValid()) { std::fprintf(stderr, "[TD2 ending] camera or goal missing\n"); return false; }
    HIKARI::GameProjectSequencePlayOptions options;
    options.holdAtEnd = 1; options.priority = 3000;
    options.channelPolicy = HIKARI::GameProjectSequenceChannelPolicy::Replace;
    constexpr char channel[] = "TD2.Ending";
    std::memcpy(options.channel, channel, sizeof(channel));
    // スロット：ゲームのカメラ、ゴールのテレビ（カメラの基準）、その区域カメラ（最初の画）。
    std::array<HIKARI::GameProjectSequenceBinding, 3> slots{};
    const auto bindSlot = [&](size_t i, std::string_view name, HIKARI::GameProjectObjectHandle object) {
        std::memcpy(slots[i].slotName, name.data(), (std::min)(name.size(), sizeof(slots[i].slotName) - 1));
        slots[i].object = object;
    };
    bindSlot(0, "Camera", camera.GetHandle());
    bindSlot(1, "Goal", goalScreen);
    const auto regionCamera = world.FindSceneObject(goalCamera);
    bindSlot(2, "GoalCamera", regionCamera.IsValid() ? regionCamera.GetHandle() : camera.GetHandle());
    options.bindings = slots.data(); options.bindingCount = static_cast<uint32_t>(slots.size());
    const auto playback = world.GetSequence().Play(settings_.ending, options);
    if (!playback.IsValid()) { std::fprintf(stderr, "[TD2 ending] sequence rejected %s\n", settings_.ending.c_str()); return false; }
    PrepareEnding(world, playback, goalScreen);
    startEye_ = eye; startLook_ = look;
    phase_ = Phase::Ending;
    std::fprintf(stderr, "[TD2 ending] start lamps=%zu tvs=%zu lightning=%zu\n", lamps_.size(), screens_.tvScreens.size(), screens_.lightning.size());
    return true;
}

bool IntroDirector::Begin(HIKARI::WorldView& world, bool preview) {
    phase_ = Phase::None; playback_ = {};
    if (!ReadSettings(world) || !settings_.enabled || settings_.sequence.empty() || preview) { return false; }
    // 演出を 0 秒で止めて待つ（タイトルの構図）。スペースで続きを流す。
    const auto camera = world.FindSceneObject(settings_.cameraObject);
    if (!camera.IsValid()) { std::fprintf(stderr, "[TD2 intro] camera object missing\n"); return false; }
    HIKARI::GameProjectSequencePlayOptions options;
    options.holdAtEnd = 1; options.priority = 3000;
    options.channelPolicy = HIKARI::GameProjectSequenceChannelPolicy::Replace;
    constexpr char channel[] = "TD2.Intro";
    std::memcpy(options.channel, channel, sizeof(channel));
    HIKARI::GameProjectSequenceBinding slot;
    constexpr char slotName[] = "Camera";
    std::memcpy(slot.slotName, slotName, sizeof(slotName));
    slot.object = camera.GetHandle(); options.bindings = &slot; options.bindingCount = 1;
    const auto sequence = world.GetSequence();
    const auto playback = sequence.Play(settings_.sequence, options);
    if (!playback.IsValid()) { std::fprintf(stderr, "[TD2 intro] sequence rejected %s\n", settings_.sequence.c_str()); return false; }
    (void)sequence.Pause(playback);
    Prepare(world, playback);
    phase_ = Phase::Title;
    std::fprintf(stderr, "[TD2 intro] title lamps=%zu tvs=%zu lightning=%zu\n", lamps_.size(), screens_.tvScreens.size(), screens_.lightning.size());
    return true;
}

void IntroDirector::Collect(HIKARI::WorldView& world) {
    // 舞台の灯（画面の中を照らす）は明滅させない。稲妻の灯は普段消しておき、閃く時だけ点ける。
    std::set<uint64_t> stage;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kStageLightComponentType)};
    (void)world.ForEachComponents<StageLightComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView object, StageLightComponent&) { stage.insert(object.GetHandle().value); });
    // 区域のテレビは世界地図に置いていないものも含めて全部砂嵐を出す（置いたものは砂嵐になるまで普段の画面）。
    std::set<uint64_t> screens;
    const HIKARI::ComponentTypeId regionTypes[]{HIKARI::MakeComponentTypeId(kRegionComponentType)};
    (void)world.ForEachComponents<RegionComponent>(regionTypes, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView screen, RegionComponent&) {
            if (screens.insert(screen.GetHandle().value).second) { screens_.tvScreens.push_back(screen.GetHandle()); }
            // パソコンの画面の中心（テレビの縮尺と向きを掛けたモデルの画面位置）。小物はここへ飛び込む。
            HIKARI::GameProjectTransform tv{};
            if (!ending_ && screen.GetSceneObjectId() == settings_.computerScreen && screen.GetWorldTransform(tv)) {
                const auto g = RegionComponent::Surface(screen);
                const auto local = QRotate(tv.rotation, {g.centerX * tv.scale.x, g.centerY * tv.scale.y, g.centerZ * tv.scale.z});
                screenCenter_ = {tv.position.x + local.x, tv.position.y + local.y, tv.position.z + local.z};
            }
        });
    // 区域が見つからない時はパソコンの物体の位置を画面の中心とする。
    if (!ending_ && screenCenter_.x == 0 && screenCenter_.y == 0 && screenCenter_.z == 0) {
        HIKARI::GameProjectTransform tv{};
        if (world.FindSceneObject(settings_.computerScreen).GetWorldTransform(tv)) { screenCenter_ = tv.position; }
    }
    const auto reachOf = [&](HIKARI::ObjectView object) {
        HIKARI::GameProjectTransform at{};
        if (!object.GetWorldTransform(at)) { return 1.0f; }
        const auto d = Sub(at.position, screenCenter_);
        return static_cast<float>(IntroReach(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z)));
    };
    for (size_t i = 0; i < settings_.props.size(); ++i) {
        const auto object = world.FindSceneObject(settings_.props[i]);
        HIKARI::GameProjectTransform rest{};
        if (settings_.props[i] != 0 && object.IsValid() && object.GetWorldTransform(rest)) {
            props_.push_back({object.GetHandle(), rest, static_cast<uint32_t>(settings_.props[i])});
        }
    }
    for (uint64_t id = 1; id < kScanLimit; ++id) {
        const auto object = world.FindSceneObject(id);
        if (!object.IsValid()) { continue; }
        if (id == settings_.lightning1 || id == settings_.lightning2) {
            lamps_.push_back({object.GetHandle(), settings_.lightningPeak, -1, static_cast<uint32_t>(id), true, false});
            continue;
        }
        HIKARI::GameProjectLightState state{};
        // 視野の外で灯の予算が切っている灯も入れる（明滅は強さだけを変え、点け消しは予算に任せる）。
        if (!stage.contains(object.GetHandle().value) && object.GetLight().TryGetState(state)) {
            lamps_.push_back({object.GetHandle(), state.intensity, -1, static_cast<uint32_t>(id), false, true, id == settings_.screenGlow, reachOf(object)});
        }
        HIKARI::GameProjectModelAppearance appearance{};
        if (object.GetModel().TryGetAppearance(appearance)) {
            const std::string_view asset(appearance.assetId, strnlen(appearance.assetId, sizeof(appearance.assetId)));
            if (!asset.empty() && std::find(settings_.tvModels.begin(), settings_.tvModels.end(), asset) != settings_.tvModels.end() &&
                screens.insert(object.GetHandle().value).second) {
                screens_.tvScreens.push_back(object.GetHandle());
            }
        }
    }
}

void IntroDirector::ScheduleCollapse(HIKARI::WorldView& world) {
    struct Item { uint64_t key; double reach; };
    std::vector<Item> items;
    // オープニングはパソコンを、通関はゴールのテレビを最後まで残す（ゴールは範囲の終わりに一台だけで消える）。
    const auto computer = world.FindSceneObject(settings_.computerScreen);
    const uint64_t last = ending_ ? screens_.goalScreen : computer.IsValid() ? computer.GetHandle().value : 0;
    for (const auto& handle : screens_.tvScreens) {
        HIKARI::GameProjectTransform at{};
        if ((last && handle.value == last) || !world.FindObject(handle).GetWorldTransform(at)) { continue; }
        const auto d = Sub(at.position, screenCenter_);
        items.push_back({handle.value, IntroReach(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z))});
    }
    // 遠い順。同じ遠さは handle の順にして、毎回同じ並びにする。
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.reach != b.reach ? a.reach > b.reach : a.key < b.key; });
    const int beats = (std::max)(1, settings_.collapseBeats);
    const double span = screens_.collapseSeconds * (ending_ ? .8 : 1.0);
    const auto beatTime = [&](int k) { return IntroBeatTime(k, beats, screens_.suckStart, span); };
    if (ending_ && last) { screens_.offTime[last] = screens_.suckStart + screens_.collapseSeconds; }
    std::vector<double> nearest(beats, -1);   // 拍ごとに消えるうちで一番近いものの遠さ（-1 はその拍に何も無い）
    for (size_t i = 0; i < items.size(); ++i) {
        const int k = IntroBeatOf(i, items.size(), beats);
        // 同じ拍の中も数十ミリ秒ずつずらし、一列ずつ倒れるように見せる。
        screens_.offTime[items[i].key] = static_cast<float>(beatTime(k) + Hash(static_cast<uint32_t>(items[i].key), 11) * .06);
        nearest[k] = items[i].reach;
    }
    // 灯は同じ遠さのテレビと同じ拍で消える。どのテレビより近い灯は最後の拍。
    for (auto& lamp : lamps_) {
        lamp.off = static_cast<float>(beatTime(beats - 1));
        for (int k = 0; k < beats; ++k) {
            if (nearest[k] >= 0 && lamp.reach >= nearest[k]) { lamp.off = static_cast<float>(beatTime(k)); break; }
        }
    }
}

bool IntroDirector::Update(HIKARI::WorldView& world, bool start) {
    if (phase_ == Phase::Ending) {
        HIKARI::GameProjectSequencePlaybackSnapshot snapshot{};
        const bool alive = world.GetSequence().GetSnapshot(playback_, snapshot);
        const float end = (std::min)(alive ? snapshot.durationSeconds : 0.f, screens_.wakeEnd);
        if (!alive || snapshot.timeSeconds >= end - 1e-3f) { FinishEnding(world); return true; }
        Apply(world, snapshot.timeSeconds);
        return true;
    }
    if (phase_ == Phase::Title) {
        DriveDepthOfField(world);
        WarmUi(world);
        if (start && world.GetSequence().Resume(playback_)) { phase_ = Phase::Playing; std::fprintf(stderr, "[TD2 intro] start\n"); }
        return true;
    }
    if (phase_ != Phase::Playing) { return false; }
    HIKARI::GameProjectSequencePlaybackSnapshot snapshot{};
    if (!world.GetSequence().GetSnapshot(playback_, snapshot) || snapshot.timeSeconds >= snapshot.durationSeconds - 1e-3f) {
        Finish(world);
        return false;
    }
    Apply(world, snapshot.timeSeconds);
    return true;
}

void IntroDirector::Preview(HIKARI::WorldView& world) {
    HIKARI::GameProjectSequencePlaybackSnapshot preview{};
    const bool found = world.GetSequence().GetEditorPreview(preview) && ReadSettings(world);
    const std::string_view asset(preview.assetGuid, strnlen(preview.assetGuid, sizeof(preview.assetGuid)));
    const bool endingPreview = found && !settings_.ending.empty() && asset == settings_.ending;
    const bool previewing = endingPreview || (found && settings_.enabled && !settings_.sequence.empty() && asset == settings_.sequence);
    if (!previewing) {
        if (phase_ == Phase::Preview) { Restore(world); playback_ = {}; phase_ = Phase::None; }
        return;
    }
    // タイムラインでイベントの区間やカメラを直すとプレビューが流し直される（新しい handle）。その時は元へ戻してから読み直す。
    if (phase_ != Phase::Preview || !(preview.handle == playback_)) {
        if (phase_ == Phase::Preview) { Restore(world); }
        if (endingPreview) { PrepareEnding(world, preview.handle, FindGoalScreen(world)); startEye_ = startLook_ = {}; }
        else { Prepare(world, preview.handle); }
        phase_ = Phase::Preview;
    }
    Apply(world, preview.timeSeconds);
}

void IntroDirector::Apply(HIKARI::WorldView& world, double t) {
    screens_.time = t;
    if (ending_) {
        // ぼけはオープニングと同じ弱さ（引いた画で奥の工場が読める）。目覚めでは強くぼかしてからその弱さへ戻す（ピントが合っていく）。
        const double focus = std::clamp((t - screens_.revealTime) / 4.0, 0.0, 1.0), k = (std::max)(.05f, settings_.depthOfField);
        DriveDepthOfField(world, t < screens_.revealTime ? 1.0 : 1 + (1 / k - 1) * (1 - focus * focus * (3 - 2 * focus)));
        DriveLetterbox(world, t);
        DriveLights(world, t);
        DriveEyelids(world, t);
        DriveStartOffset(world, t);
        return;
    }
    DriveDepthOfField(world);
    DriveLetterbox(world, t);
    DriveTitleCard(world, t);
    DriveLights(world, t);
    DriveBlack(world, t);
    DriveProps(world, t);
}

void IntroDirector::DriveDepthOfField(HIKARI::WorldView& world, double scale) {
    const auto view = world.GetCamera();
    const auto camera = world.FindSceneObject(settings_.cameraObject);
    if (!depthOfFieldRead_) {
        HIKARI::GameProjectCameraLens lens{};
        depthOfFieldRead_ = true;
        if (!camera.IsValid() || !view.GetAuthoredLens(camera.GetHandle().value, lens) || !lens.depthOfField.enabled) { return; }
        depthOfField_ = lens.depthOfField;
    }
    if (!depthOfField_.enabled) { return; }
    // 一人称では手元の机も奥の工場も写るので、ぼけの半径だけを小さくして奥行きの手掛かりは残す。
    HIKARI::GameProjectCameraDepthOfFieldRequest request{};
    request.cameraObjectHandle = camera.GetHandle().value;
    request.sourceId = 0x7D2147u; request.priority = 300;
    request.settings = depthOfField_;
    const float k = std::clamp(settings_.depthOfField, 0.0f, 1.0f) * static_cast<float>(scale);
    request.settings.nearBlurRadius1080pPixels = (std::min)(32.0f, request.settings.nearBlurRadius1080pPixels * k);
    request.settings.farBlurRadius1080pPixels = (std::min)(32.0f, request.settings.farBlurRadius1080pPixels * k);
    request.settings.enabled = k > 0 ? 1u : 0u;
    (void)view.SubmitDepthOfField(request);
}

void IntroDirector::DriveLetterbox(HIKARI::WorldView& world, double t) const {
    if (!(settings_.letterbox > 0)) { return; }
    // 黒帯の範囲の始めから 1 秒で入り、終わりまでの 0.8 秒で退く（なめらかに）。
    const double amount = Math::Smoothstep((t - screens_.letterboxIn) / 1.0) * (1 - Math::Smoothstep((t - (screens_.letterboxOut - .8)) / .8));
    if (amount <= 0) { return; }
    const auto [width, height] = UiSize(world);
    // 帯の太さは 16:9 の画面で letterbox の比になる割合に揃える（編集器の縦長なゲームビューでも帯が太くなりすぎない）。
    const double bar = height * (std::max)(0.0, 1.0 - 16.0 / 9.0 / settings_.letterbox) * .5 * amount;
    if (bar < .5) { return; }
    HIKARI::GameProjectUiSpriteRequest request{};
    request.sourceId = 0x7D2148u; request.order = 100000;
    request.referenceWidth = width; request.referenceHeight = height;
    request.width = width; request.height = static_cast<float>(bar + 1);
    const auto ui = world.GetUi();
    request.y = -1;
    (void)ui.SubmitSprite(kLetterboxTexture, request);
    request.y = static_cast<float>(height - bar);
    (void)ui.SubmitSprite(kLetterboxTexture, request);
}

void IntroDirector::WarmUi(HIKARI::WorldView& world) const {
    HIKARI::GameProjectUiSpriteRequest request{};
    request.sourceId = 0x7D214Cu; request.order = -100000;
    request.referenceWidth = 1920; request.referenceHeight = 1080;
    request.width = 1; request.height = 1;
    request.tint = {1, 1, 1, .004f};
    const auto ui = world.GetUi();
    for (const auto texture : {kLetterboxTexture, kTitleTexture, kWhiteTexture}) { (void)ui.SubmitSprite(texture, request); }
}

void IntroDirector::DriveTitleCard(HIKARI::WorldView& world, double t) const {
    // 点灯 0.42 秒（点 0.12・線 0.16・開く 0.14）、映る titleHold 秒、消灯 0.36 秒（閉じる・線・点 各 0.12）。
    const double on = t - screens_.titleIn, off = on - kTitleOnSeconds - screens_.titleHold;
    if (on < 0 || off >= kTitleOffSeconds) { return; }
    const auto [width, height] = UiSize(world);
    // 黒帯の内側に 16:9 で収める。
    const double bar = settings_.letterbox > 0 ? height * (std::max)(0.0, 1.0 - 16.0 / 9.0 / settings_.letterbox) * .5 : 0.0;
    const double cardW = (std::min)(double(width), (height - 2 * bar) * 16 / 9), cardH = cardW * 9 / 16;
    const double cx = width * .5, cy = height * .5, line = (std::max)(2.0, height * .004);
    HIKARI::GameProjectUiSpriteRequest request{};
    request.sourceId = 0x7D2149u; request.order = 99000;
    request.referenceWidth = width; request.referenceHeight = height;
    const auto ui = world.GetUi();
    const auto put = [&](std::string_view texture, double w, double h, float brightness) {
        request.x = static_cast<float>(cx - w * .5); request.y = static_cast<float>(cy - h * .5);
        request.width = static_cast<float>(w); request.height = static_cast<float>(h);
        request.tint = {brightness, brightness, brightness, 1};
        (void)ui.SubmitSprite(texture, request);
    };
    if (off < 0) {
        if (on < .12) { const double d = line * (1 + 2 * Math::Smoothstep(on / .12)); put(kWhiteTexture, d, d, 1); }
        else if (on < .28) { put(kWhiteTexture, line * 3 + (cardW - line * 3) * Math::Smoothstep((on - .12) / .16), line, 1); }
        else if (on < kTitleOnSeconds) { put(kTitleTexture, cardW, line + (cardH - line) * Math::Smoothstep((on - .28) / .14), .95f); }
        else {
            // 映っている間はわずかにちらつく。
            const auto tick = static_cast<uint32_t>(t * 12);
            put(kTitleTexture, cardW, cardH, static_cast<float>(.82 + .08 * Hash(0x7171u, tick)));
        }
        return;
    }
    if (off < .12) { put(kTitleTexture, cardW, line + (cardH - line) * (1 - Math::Smoothstep(off / .12)), .95f); }
    else if (off < .24) { put(kWhiteTexture, line * 3 + (cardW - line * 3) * (1 - Math::Smoothstep((off - .12) / .12)), line, 1); }
    else { const double d = line * 3 * (1 - Math::Smoothstep((off - .24) / .12)); if (d > .5) { put(kWhiteTexture, d, d, 1); } }
}

double IntroDirector::EndingLamp(const Lamp& lamp, double t) const {
    const auto& s = screens_;
    // 目覚めは元の工場（夢から覚めた）。暗転と、同じ遠さのテレビが消えた後は真っ暗。
    if (t >= s.revealTime || t < s.flickerStart) { return lamp.base; }
    if (t >= s.blackTime || t >= lamp.off + .05) { return 0; }
    const double ramp = std::clamp((t - s.flickerStart) / 1.2, 0.0, 1.0) * std::clamp(double(settings_.flicker), 0.0, 1.0);
    // 揺れ方はオープニングと同じ：同じ電源の灯が一緒に揺れ、最初の稲妻までは前兆、その後は非常灯のように暗くちらつく。
    const double power = Noise(0xF11Cu, t, 9), surge = Noise(0xF11Du, t, 2.2), own = Noise(lamp.seed, t, 21);
    const double first = s.lightning.empty() ? s.suckStart : s.lightning.front();
    double v = 1;
    if (t < first) {
        const double p = std::clamp((t - s.flickerStart) / (std::max)(.1, first - s.flickerStart), 0.0, 1.0);
        if (power < .1 + .32 * p) { v = .1; }
        if (surge < .04 + .2 * p * p) { v = 0; }
        if (IntroOmenBurst(t, s.flickerStart, first)) { v = (std::min)(v, .03); }
    } else {
        v = power < .35 ? .02 : .12;
        if (surge < .25) { v = 0; }
    }
    if (own < .15) { v *= .4; }
    for (const float flash : s.lightning) { if (t > flash - .25 && t < flash + .3) { v = 0; } }
    return lamp.base * (1 + (v - 1) * ramp);
}

void IntroDirector::DriveEyelids(HIKARI::WorldView& world, double t) const {
    if (t < screens_.blackTime) { return; }
    // まぶたの開き（0 閉じ、1 開いた）：目覚めの 0.8 秒後から薄く開き、一度閉じかけてから、ゆっくり開ききる。
    const double u = t - screens_.revealTime;
    double open = 0;
    if (u >= .8) { open = .35 * Math::Smoothstep((u - .8) / .7); }
    if (u >= 1.7) { open = .35 - .3 * Math::Smoothstep((u - 1.7) / .25); }
    if (u >= 1.95) { open = .05 + .95 * Math::Smoothstep((u - 1.95) / 1.4); }
    if (open >= .999) { return; }
    const auto [width, height] = UiSize(world);
    // UI は仕上げの後に描くので、上下の黒は本当の黒になる（色調整では漫画の仕上げの紙の色が残る）。
    const double lid = height * .5 * (1 - open);
    HIKARI::GameProjectUiSpriteRequest request{};
    request.sourceId = 0x7D214Bu; request.order = 100001;
    request.referenceWidth = width; request.referenceHeight = height;
    request.width = width; request.height = static_cast<float>(lid + 2);
    const auto ui = world.GetUi();
    request.y = -1;
    (void)ui.SubmitSprite(kLetterboxTexture, request);
    request.y = static_cast<float>(height - lid - 1);
    (void)ui.SubmitSprite(kLetterboxTexture, request);
}

void IntroDirector::DriveStartOffset(HIKARI::WorldView& world, double t) const {
    // 区域カメラから始まる演出を、扉に入った時の追従の画からなめらかに受け継ぐ。
    const auto w = static_cast<float>(1 - Math::Smoothstep(t / 1.2));
    if (w <= 0) { return; }
    HIKARI::GameProjectCameraModifier modifier{};
    modifier.sourceId = 0x7D214Au; modifier.priority = 60; modifier.applyDuringOverride = 1;
    modifier.worldPositionOffset = {startEye_.x * w, startEye_.y * w, startEye_.z * w};
    modifier.worldTargetOffset = {startLook_.x * w, startLook_.y * w, startLook_.z * w};
    (void)world.GetCamera().SubmitModifier(modifier);
}

void IntroDirector::DriveBlack(HIKARI::WorldView& world, double t) {
    // 1 は作者の色調整のまま、0 は真っ黒。演出の暗転と同じく飛び込む直前の 1 フレームで切り、遠景に替わってから 0.4 秒で戻す。
    const double in = std::clamp((t - (screens_.blackTime - .015)) / .015, 0.0, 1.0);
    const double out = std::clamp((t - screens_.revealTime) / .4, 0.0, 1.0);
    const auto k = static_cast<float>(1 - in * (1 - out));
    if (std::abs(k - blackSent_) < .01f) { return; }
    const auto environment = world.GetEnvironment();
    if (!gradingSaved_) {
        grading_ = {};
        if (!environment.GetColorGrading(grading_).Accepted()) { return; }
        gradingSaved_ = true;
    }
    if (k >= .999f) {
        if (gradingOverridden_) { (void)environment.ClearColorGradingOverride(); gradingOverridden_ = false; }
        blackSent_ = 1;
        return;
    }
    auto grading = grading_;
    for (auto* tint : {&grading.shadowsTint, &grading.midtonesTint, &grading.highlightsTint}) {
        tint->x *= k; tint->y *= k; tint->z *= k;
    }
    const auto result = environment.SetColorGradingOverride(grading);
    if (result.Accepted()) { gradingOverridden_ = true; blackSent_ = k; }
}

void IntroDirector::DriveLights(HIKARI::WorldView& world, double t) {
    const auto& s = screens_;
    for (auto& lamp : lamps_) {
        const auto object = world.FindObject(lamp.handle);
        if (!object.IsValid()) { continue; }
        double intensity = lamp.base;
        bool enabled = true;
        if (lamp.lightning) {
            // 稲妻：一回ごとに強い閃きと、少し遅れた弱い閃き。灯ごとに強さを変え、二つの窓が別々に光る。
            double env = 0;
            for (size_t i = 0; i < s.lightning.size(); ++i) {
                const double dt = t - s.lightning[i];
                if (dt < 0 || dt > .6) { continue; }
                const double strength = .55 + .45 * Hash(lamp.seed, static_cast<uint32_t>(i));
                env += strength * std::exp(-dt / .07);
                if (dt > .11) { env += .45 * strength * std::exp(-(dt - .11) / .05); }
            }
            intensity = lamp.base * (std::min)(env, 1.0);
            enabled = intensity > lamp.base * .002;
        } else if (ending_) {
            intensity = EndingLamp(lamp, t);
        } else if (lamp.glow && t >= s.blackTime) {
            const double on = s.powerOn + .1, locked = s.lockStart + s.lockSeconds;
            intensity = lamp.base * (t < on ? 0 : t < locked ? .5 + .3 * Hash(lamp.seed, static_cast<uint32_t>(t * 12)) : 1);
        } else if (lamp.glow && t >= s.titleOff && t < s.restoreTime) {
            // パソコンの画面の光：乱れて消え、最初の稲妻で砂嵐と一緒に点き、吸い込まれる時は画面と一緒に強くなる。
            const auto tick = static_cast<uint32_t>(std::floor(t * 12));
            double v = 0;
            if (t < s.titleOff + .3) { v = Hash(lamp.seed, tick) < .5 ? 1.6 : 0; }
            else if (t < s.staticStart) { v = IntroOmenBurst(t, s.flickerStart, s.staticStart) ? .9 : 0; }
            else if (t >= s.whiteStart) { v = 2.5 + 2 * std::clamp((t - s.suckStart) / (s.blackTime - s.suckStart), 0.0, 1.0); }
            else if (t >= s.blackoutStart) { v = 0; }
            else if (t >= s.staticStart) { v = .5 + .4 * Hash(lamp.seed, tick + 7u); }
            for (const float flash : s.lightning) { if (t >= flash && t < flash + .12 && t >= s.staticStart) { v = 1.5; } }
            intensity = lamp.base * v;
        } else if (!lamp.glow && t >= s.flickerStart) {
            // 工場の灯の明滅。最初の稲妻までは前兆（時々ちらつき、一瞬落ちる）、稲妻の後は非常灯のように暗いまま
            // ちらつき続け、一部は切れて戻らない。
            const double ramp = std::clamp((t - s.flickerStart) / 1.2, 0.0, 1.0) * std::clamp(double(settings_.flicker), 0.0, 1.0);
            // 工場の灯は同じ電源につながっているので、全部が一緒に揺れる。灯ごとの揺れは少しだけ足す。
            const double power = Noise(0xF11Cu, t, 9), surge = Noise(0xF11Du, t, 2.2), own = Noise(lamp.seed, t, 21);
            const double first = s.lightning.empty() ? s.staticStart : s.lightning.front();
            // 露出が夜向き（明るめ）なので、消える時は 0 まで落とさないと消えて見えない。
            double v = 1;
            if (t < first) {
                // 前兆：初めはまれに揺れる程度で、稲妻が近づくほど落ちる回数も長さも増える。電気が乱れる瞬間は全部落ちる。
                const double p = std::clamp((t - s.flickerStart) / (std::max)(.1, first - s.flickerStart), 0.0, 1.0);
                if (power < .1 + .32 * p) { v = .1; }
                if (surge < .04 + .2 * p * p) { v = 0; }
                if (IntroOmenBurst(t, s.flickerStart, first)) { v = (std::min)(v, .03); }
            } else {
                v = power < .35 ? .02 : .12;
                if (surge < .25) { v = 0; }
                if (Hash(lamp.seed, 99) < .35 && t > first + .4 + Hash(lamp.seed, 98) * 6) { v = 0; }
            }
            if (own < .15) { v *= .4; }
            // 稲妻の直前と最中は全部落とし、窓から差す光だけにする。
            for (const float flash : s.lightning) { if (t > flash - .25 && t < flash + .3) { v = 0; } }
            // 全消灯：全部の灯が一度に切れる。白い画面の間は、画面に照らされたように弱く灯り、近くのテレビと同じ拍で消える。
            if (t >= s.blackoutStart) { v = t >= s.whiteStart && t < lamp.off ? .22 : 0; }
            // 電気が戻る：手元の灯から遠くへ順に、何度か頼りなく瞬いてから点く。
            if (t >= s.blackTime) {
                const double on = s.powerOn + std::pow(lamp.reach, .8) * s.powerSeconds + Hash(lamp.seed, 21) * .25;
                v = t < on ? 0 : t < on + .6 ? (Hash(lamp.seed, static_cast<uint32_t>((t - on) * 14)) < .45 ? .8 : 0) : 1;
            }
            intensity = lamp.base * (1 + (v - 1) * ramp);
        }
        HIKARI::GameProjectLightPatch patch{};
        if (lamp.lightning && enabled != lamp.enabled) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Enabled);
            patch.enabled = enabled ? 1u : 0u;
        }
        if (std::abs(intensity - lamp.sent) > lamp.base * .01) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity);
            patch.intensity = static_cast<float>(intensity);
        }
        if (patch.fieldMask == 0) { continue; }
        if (object.GetLight().ApplyPatch(patch).Accepted()) {
            if (patch.fieldMask & HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity)) { lamp.sent = static_cast<float>(intensity); }
            if (lamp.lightning) { lamp.enabled = enabled; }
        }
    }
}

void IntroDirector::DriveProps(HIKARI::WorldView& world, double t) {
    if (props_.empty()) { return; }
    if (t >= screens_.restoreTime) { RestoreProps(world); return; }
    const double shake = screens_.pullStart - .9;
    // 震える前は作者の位置（プレビューで巻き戻した時も元の位置に戻る）。
    if (t < shake) { RestoreProps(world); return; }
    for (size_t i = 0; i < props_.size(); ++i) {
        auto& prop = props_[i];
        const auto object = world.FindObject(prop.handle);
        if (!object.IsValid()) { continue; }
        // 飛び立つ時刻は小物ごとにずらし、順に吸われていく。
        const double launch = screens_.pullStart + .08 + i * .13, flight = .5 + .15 * Hash(prop.seed, 3);
        auto pose = prop.rest;
        if (t < launch) {
            // 震え：引かれ始めが近づくほど大きく、少し浮く。
            const double k = std::clamp((t - shake) / (launch - shake), 0.0, 1.0);
            const auto tick = static_cast<uint32_t>(t * 30);
            const float jitter = static_cast<float>(.002 + .006 * k);
            pose.position = {pose.position.x + jitter * float(Hash(prop.seed, tick) - .5) * 2,
                             pose.position.y + static_cast<float>(.012 * k * k) + jitter * float(Hash(prop.seed, tick + 1)),
                             pose.position.z + jitter * float(Hash(prop.seed, tick + 2) - .5) * 2};
            pose.rotation = QMul(QFromEulerDeg(float((Hash(prop.seed, tick + 3) - .5) * 8 * k), 0, float((Hash(prop.seed, tick + 4) - .5) * 8 * k)), pose.rotation);
        } else {
            // 吸い込み：加速しながら弧を描いて画面の中心へ。回りながら縮み、画面に着くと消える。
            const double u = std::clamp((t - launch) / flight, 0.0, 1.0), e = u * u * u;
            const GameProjectFloat3 target{screenCenter_.x + float((Hash(prop.seed, 5) - .5) * .12), screenCenter_.y + float((Hash(prop.seed, 6) - .5) * .08), screenCenter_.z};
            pose.position = Lerp(prop.rest.position, target, static_cast<float>(e));
            pose.position.y += static_cast<float>(.07 * std::sin(u * Math::kPi));
            const float spin = static_cast<float>(e * (300 + 400 * Hash(prop.seed, 7)));
            pose.rotation = QMul(QFromEulerDeg(spin * .6f, spin, spin * .3f), prop.rest.rotation);
            const float size = u >= 1 ? .001f : static_cast<float>(1 - .8 * e * e);
            pose.scale = {prop.rest.scale.x * size, prop.rest.scale.y * size, prop.rest.scale.z * size};
        }
        (void)object.SetWorldTransform(pose);
        propsMoved_ = true;
    }
}

void IntroDirector::RestoreProps(HIKARI::WorldView& world) {
    if (!propsMoved_) { return; }
    propsMoved_ = false;
    for (const auto& prop : props_) {
        if (const auto object = world.FindObject(prop.handle); object.IsValid()) { (void)object.SetWorldTransform(prop.rest); }
    }
}

void IntroDirector::Restore(HIKARI::WorldView& world) {
    // 灯を作者の強さへ戻し、稲妻の灯を消す。小物を元の位置へ、色調整を作者のものへ戻す。
    for (auto& lamp : lamps_) {
        const auto object = world.FindObject(lamp.handle);
        if (!object.IsValid()) { continue; }
        HIKARI::GameProjectLightPatch patch{};
        patch.fieldMask = HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity);
        patch.intensity = lamp.base;
        if (lamp.lightning) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Enabled);
            patch.enabled = 0u;
        }
        (void)object.GetLight().ApplyPatch(patch);
    }
    lamps_.clear();
    depthOfFieldRead_ = false; depthOfField_ = {};
    RestoreProps(world);
    props_.clear();
    if (gradingOverridden_) { (void)world.GetEnvironment().ClearColorGradingOverride(); }
    gradingSaved_ = gradingOverridden_ = false; blackSent_ = -1;
    screens_.active = false;
}

void IntroDirector::Finish(HIKARI::WorldView& world) {
    // Play の終わり：元へ戻し、演出を止めてカメラをゲームのカメラへ返す。
    Restore(world);
    if (playback_.IsValid()) { (void)world.GetSequence().Stop(playback_); }
    playback_ = {};
    phase_ = Phase::Done;
    std::fprintf(stderr, "[TD2 intro] done\n");
}

void IntroDirector::FinishEnding(HIKARI::WorldView& world) {
    Restore(world);
    if (playback_.IsValid()) { (void)world.GetSequence().Stop(playback_); }
    playback_ = {};
    phase_ = Phase::Done; ending_ = false; endingFinished_ = true;
    std::fprintf(stderr, "[TD2 ending] done\n");
}

void IntroDirector::Stop(HIKARI::WorldView& world) {
    if (phase_ == Phase::Title || phase_ == Phase::Playing || phase_ == Phase::Ending) { Finish(world); }
    endingFinished_ = false; ending_ = false;
    if (phase_ == Phase::Preview) { Restore(world); playback_ = {}; }
    phase_ = Phase::None;
}

void IntroDirector::Skip(HIKARI::WorldView& world) {
    if (phase_ != Phase::Title && phase_ != Phase::Playing) { return; }
    std::fprintf(stderr, "[TD2 intro] skipped\n");
    Finish(world);
}
}
