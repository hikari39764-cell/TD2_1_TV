#include "TD2Intro.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/CameraViewSnapshot.h"
#include "HIKARI/Project/ComponentViews.h"
#include "HIKARI/Project/EnvironmentView.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/LightTypes.h"
#include "HIKARI/Project/SequenceView.h"
#include "HIKARI/Project/UiTypes.h"
#include "HIKARI/Project/UiView.h"
#include "HIKARI/Project/WorldQuery.h"
#include "TD2Popups.h"
#include "TD2RegionComponent.h"
#include "TD2StageLight.h"
#include "TD2Transform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

namespace TD2 {
namespace {
using Math::Hash01;
// タイトル待ちの寄りきるまでの秒と、オープニングで作者の構図へ戻す秒。
constexpr double kTitleZoomSeconds = 2.6, kTitleZoomOutSeconds = 2.2;
// 通関：扉が閉じるまでの秒（閉じ終えた瞬間にゴールのテレビが消え始める）。
constexpr float kDoorClose = 3.2f;
// 画面に飛び込んで切れてから灯とテレビを戻すまで（暗転の中で戻し、戻る瞬間を見せない）。
constexpr float kRestoreAfterBlack = .06f;
}

// パソコンからの遠さ（0 が手元、1 が工場の奥）。砂嵐の広がりと吸い込みの順に使う。
double IntroReach(double distance) { return std::pow(std::clamp((distance - .4) / 24.0, 0.0, 1.0), .7); }

IntroTimeline ReadIntroTimeline(const std::vector<HIKARI::GameProjectSequenceEventClip>& clips, bool arrival, float offset, std::string& missing) {
    IntroTimeline t;
    std::set<std::string, std::less<>> seen;
    for (const auto& clip : clips) {
        const std::string_view name(clip.name, strnlen(clip.name, sizeof(clip.name)));
        const float start = clip.startSeconds + offset, length = clip.durationSeconds, end = start + length;
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
        else if (name == IntroEvent::Reveal) { t.revealTime = start; }
        else if (name == IntroEvent::Power) { t.powerOn = start; t.powerSeconds = length; }
        else if (name == IntroEvent::Lock) { t.lockStart = start; t.lockSeconds = length; }
        else if (name == IntroEvent::Arrive) { t.arriveTime = start; }
        else if (name == IntroEvent::Letterbox) { t.letterboxIn = start; t.letterboxOut = end; }
    }
    std::sort(t.lightning.begin(), t.lightning.end());
    missing.clear();
    const auto check = [&](std::initializer_list<std::string_view> names) {
        for (const auto name : names) { if (!seen.contains(name)) { missing += std::string(name) + " "; } }
    };
    if (arrival) { check({IntroEvent::Reveal, IntroEvent::Power, IntroEvent::Lock, IntroEvent::Arrive, IntroEvent::Letterbox}); }
    else {
        check({IntroEvent::TitleOff, IntroEvent::Omen, IntroEvent::Lightning, IntroEvent::Static, IntroEvent::Blackout, IntroEvent::White,
               IntroEvent::Collapse, IntroEvent::Alone, IntroEvent::Pull, IntroEvent::Letterbox});
    }
    return t;
}

bool IntroDirector::ReadSettings(HIKARI::WorldView& world) {
    bool found = false;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kIntroComponentType)};
    (void)world.ForEachComponents<IntroComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView, IntroComponent& intro) { if (!found) { settings_ = intro.Settings(); found = true; } });
    return found;
}

bool IntroDirector::Prepare(HIKARI::WorldView& world, HIKARI::GameProjectSequencePlaybackHandle playback, bool arrival) {
    screens_ = {}; lamps_.clear(); props_.clear(); screenCenter_ = {}; ending_ = false;
    std::vector<HIKARI::GameProjectSequenceEventClip> clips(HIKARI::kGameProjectSequenceMaximumEventClips);
    const auto sequence = world.GetSequence();
    clips.resize((std::min)(clips.size(), size_t(sequence.GetEventClips(playback, clips.data(), static_cast<uint32_t>(clips.size())))));
    std::string missing;
    static_cast<IntroTimeline&>(screens_) = ReadIntroTimeline(clips, arrival, 0, missing);
    if (!missing.empty()) { std::fprintf(stderr, "[TD2 intro] events missing: %s\n", missing.c_str()); }
    if (arrival) {
        // 二つ目の演出だけをプレビューする時：暗転と吸い込みは始まる前に済んだものとし、灯は消えたまま電気が戻るのを待つ。
        screens_.flickerStart = -1.2f; screens_.pullStart = -1.3f; screens_.blackTime = -.06f; screens_.restoreTime = 0;
    }
    playback_ = playback;
    Collect(world);
    ScheduleCollapse(world);
    screens_.active = true; screens_.time = 0;
    screens_.computerScreen = settings_.computerScreen;
    return true;
}

void IntroDirector::BeginEnding(HIKARI::WorldView& world, HIKARI::GameProjectObjectHandle goalScreen) {
    (void)ReadSettings(world);
    screens_ = {}; lamps_.clear(); props_.clear(); screenCenter_ = {}; ending_ = true;
    playback_ = {};
    // 扉が閉じ始めると主人公は登場の光の中へ消え、閉じ終えた瞬間にゴールのテレビがブラウン管のように消える。
    screens_.closeStart = 0; screens_.arriveTime = 0;
    screens_.goalScreen = goalScreen.value;
    screens_.offTime[goalScreen.value] = kDoorClose;
    Collect(world);
    screens_.active = true; screens_.ending = true; screens_.time = 0;
    screens_.computerScreen = settings_.computerScreen;
    phase_ = Phase::Ending;
    std::fprintf(stderr, "[TD2 ending] start lamps=%zu tvs=%zu\n", lamps_.size(), screens_.tvScreens.size());
}

void IntroDirector::EndEnding(HIKARI::WorldView& world) {
    if (phase_ != Phase::Ending) { return; }
    Restore(world);
    phase_ = Phase::Done; ending_ = false;
    std::fprintf(stderr, "[TD2 ending] done\n");
}

bool IntroDirector::Begin(HIKARI::WorldView& world, bool preview, Wake wake) {
    phase_ = Phase::None; playback_ = {}; wake_ = Wake::None;
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
    // 一つ目の演出の黒帯は二つ目の終わりまで続ける（間の弾窓の間も退かない。退く時刻は二つ目を流す時に入れる）。
    screens_.letterboxOut = kIntroNever;
    phase_ = Phase::Title; wake_ = wake;
    titleZoom_ = zoomAtStart_ = titleSeconds_ = 0;
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
    // 穴だらけの文書 ID を総当たりせず、実在する物だけ集める。従来と同じ ID
    // 順にして演出の並びを保つ。
    std::vector<HIKARI::GameProjectObjectHandle> handles(static_cast<size_t>(world.GetObjectCount()));
    handles.resize(static_cast<size_t>(world.CopyObjectHandles(handles)));
    std::vector<std::pair<uint64_t, HIKARI::ObjectView>> objects;
    objects.reserve(handles.size());
    for (const auto handle : handles) {
        const auto object = world.FindObject(handle);
        const auto id = object.GetSceneObjectId();
        if (id != 0)
            objects.emplace_back(id, object);
    }
    std::sort(objects.begin(), objects.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, object] : objects) {
        if (id == settings_.lightning1 || id == settings_.lightning2) {
            lamps_.push_back({object.GetHandle(), settings_.lightningPeak, -1, static_cast<uint32_t>(id), true, false});
            continue;
        }
        HIKARI::GameProjectLightState state{};
        // 視野の外で灯の予算が切っている灯も入れる（明滅は強さだけを変え、点け消しは予算に任せる）。
        if (!stage.contains(object.GetHandle().value) && object.GetLight().TryGetState(state)) {
            lamps_.push_back({object.GetHandle(), state.intensity, -1, static_cast<uint32_t>(id), false, true,
                              id == settings_.screenGlow, reachOf(object)});
        }
        HIKARI::GameProjectModelAppearance appearance{};
        if (object.GetModel().TryGetAppearance(appearance)) {
            const std::string_view asset(appearance.assetId, strnlen(appearance.assetId, sizeof(appearance.assetId)));
            if (!asset.empty() &&
                std::find(settings_.tvModels.begin(), settings_.tvModels.end(), asset) != settings_.tvModels.end() &&
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
        screens_.offTime[items[i].key] = static_cast<float>(beatTime(k) + Hash01(static_cast<uint32_t>(items[i].key), 11) * .06);
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

bool IntroDirector::Update(HIKARI::WorldView& world, bool start, double seconds) {
    const double step = (std::min)(seconds, 1.0 / 30);
    if (phase_ == Phase::Ending) {
        screens_.time += step;
        Apply(world, screens_.time);
        return true;
    }
    if (phase_ == Phase::Title) {
        titleSeconds_ += step;
        if (wake_ != Wake::None) { DriveWake(world, titleSeconds_); } else { DriveDepthOfField(world); }
        WarmUi(world);
        // メールが開くのに合わせてゆっくり寄る（長いフレームでも飛ばないよう 1/30 秒ずつ）。
        titleZoom_ = (std::min)(1.0, titleZoom_ + step / kTitleZoomSeconds);
        DriveTitleZoom(world, Math::SmoothStep(titleZoom_));
        DriveTitleFocus(world, Math::SmoothStep(titleZoom_));   // 寄るのと一緒にピントを画面へ送る
        if (start && world.GetSequence().Resume(playback_)) {
            phase_ = Phase::Playing; zoomAtStart_ = Math::SmoothStep(titleZoom_); wake_ = Wake::None;
            std::fprintf(stderr, "[TD2 intro] start\n");
        }
        return true;
    }
    if (phase_ == Phase::Playing) {
        HIKARI::GameProjectSequencePlaybackSnapshot snapshot{};
        if (!world.GetSequence().GetSnapshot(playback_, snapshot)) { Finish(world); return false; }
        if (snapshot.timeSeconds < snapshot.durationSeconds - 1e-3f) {
            Apply(world, snapshot.timeSeconds);
            // 画面へ飛び込んで切れた後は、演出の終わりまで UI の黒で覆う。暗転の間パソコンの画面は普段のゲームの絵に戻り、
            // 読みやすさの保護で色調整も演出の黒も掛からないので、カメラが画面の中にいる間はそれが画全体に見えてしまう。
            if (snapshot.timeSeconds >= screens_.blackTime - .015) { DrawBlack(world); }
            // 寄った画から、メールが消える間に作者のタイトルの構図へ引き戻す。
            if (zoomAtStart_ > 0) {
                const double back = 1 - Math::SmoothStep(snapshot.timeSeconds / kTitleZoomOutSeconds);
                DriveTitleZoom(world, zoomAtStart_ * back);
                DriveTitleFocus(world, zoomAtStart_ * back);   // ピントも同じ間に作者の位置へ戻す
            }
            return true;
        }
        // 一つ目が終わった（暗転のまま止まっている）：弾窓へ。
        phase_ = Phase::Popups; firstSeconds_ = snapshot.durationSeconds; popupSeconds_ = 0;
        std::fprintf(stderr, "[TD2 intro] popups\n");
    }
    if (phase_ == Phase::Popups) {
        popupSeconds_ += step;
        Apply(world, firstSeconds_ + popupSeconds_);
        DrawPopups(world, popupSeconds_, step);
        if (popupSeconds_ >= kIntroPopupsSeconds && !StartArrival(world)) { Finish(world); return false; }
        return true;
    }
    if (phase_ != Phase::Arrival) { return false; }
    HIKARI::GameProjectSequencePlaybackSnapshot snapshot{};
    if (!world.GetSequence().GetSnapshot(playback_, snapshot) || snapshot.timeSeconds >= snapshot.durationSeconds - 1e-3f) {
        handoff_ = true;   // 最後の画からゲームのカメラへ滑らかに移す（Session がカメラに伝える）
        Finish(world);
        return false;
    }
    Apply(world, arrivalOffset_ + snapshot.timeSeconds);
    // 遠景を出すまでパソコンの画面は普段のゲームの絵のまま（読みやすさの保護で暗転の色調整も掛からない）。弾窓の黒が
    // 消えた最初のフレームにそれが見えるので、遠景までと、画面が砂嵐の窓へ結び直されるまでの 2 フレームを黒で覆う。
    const bool revealed = screens_.time >= screens_.revealTime;
    if (!revealed || revealCovered_ < 2) {
        DrawBlack(world);
        if (revealed) { ++revealCovered_; }
    }
    return true;
}

bool IntroDirector::StartArrival(HIKARI::WorldView& world) {
    if (settings_.arrival.empty()) { std::fprintf(stderr, "[TD2 intro] arrival sequence missing\n"); return false; }
    const auto camera = world.FindSceneObject(settings_.cameraObject);
    HIKARI::GameProjectSequencePlayOptions options;
    options.holdAtEnd = 1; options.priority = 3000;
    options.channelPolicy = HIKARI::GameProjectSequenceChannelPolicy::Replace;
    constexpr char channel[] = "TD2.Intro";
    std::memcpy(options.channel, channel, sizeof(channel));
    HIKARI::GameProjectSequenceBinding slot;
    constexpr char slotName[] = "Camera";
    std::memcpy(slot.slotName, slotName, sizeof(slotName));
    slot.object = camera.GetHandle(); options.bindings = &slot; options.bindingCount = 1;
    const auto playback = world.GetSequence().Play(settings_.arrival, options);
    if (!playback.IsValid()) { std::fprintf(stderr, "[TD2 intro] arrival sequence rejected %s\n", settings_.arrival.c_str()); return false; }
    // 二つ目の事件の時刻を一本の時刻へずらして入れる（画面の表示と音は同じ時刻を読む）。
    arrivalOffset_ = firstSeconds_ + kIntroPopupsSeconds;
    std::vector<HIKARI::GameProjectSequenceEventClip> clips(HIKARI::kGameProjectSequenceMaximumEventClips);
    clips.resize((std::min)(clips.size(), size_t(world.GetSequence().GetEventClips(playback, clips.data(), static_cast<uint32_t>(clips.size())))));
    std::string missing;
    const auto b = ReadIntroTimeline(clips, true, static_cast<float>(arrivalOffset_), missing);
    if (!missing.empty()) { std::fprintf(stderr, "[TD2 intro] arrival events missing: %s\n", missing.c_str()); }
    screens_.revealTime = b.revealTime; screens_.powerOn = b.powerOn; screens_.powerSeconds = b.powerSeconds;
    screens_.lockStart = b.lockStart; screens_.lockSeconds = b.lockSeconds; screens_.arriveTime = b.arriveTime;
    screens_.letterboxOut = b.letterboxOut;
    if (playback_.IsValid() && !(playback_ == playback)) { (void)world.GetSequence().Stop(playback_); }
    playback_ = playback;
    phase_ = Phase::Arrival; revealCovered_ = 0;
    std::fprintf(stderr, "[TD2 intro] arrival offset=%.2f\n", arrivalOffset_);
    return true;
}

void IntroDirector::Preview(HIKARI::WorldView& world) {
    HIKARI::GameProjectSequencePlaybackSnapshot preview{};
    const bool found = world.GetSequence().GetEditorPreview(preview) && ReadSettings(world);
    const std::string_view asset(preview.assetGuid, strnlen(preview.assetGuid, sizeof(preview.assetGuid)));
    const bool arrivalPreview = found && !settings_.arrival.empty() && asset == settings_.arrival;
    const bool previewing = arrivalPreview || (found && settings_.enabled && !settings_.sequence.empty() && asset == settings_.sequence);
    if (!previewing) {
        if (phase_ == Phase::Preview) { Restore(world); playback_ = {}; phase_ = Phase::None; }
        return;
    }
    // タイムラインでイベントの区間やカメラを直すとプレビューが流し直される（新しい handle）。その時は元へ戻してから読み直す。
    if (phase_ != Phase::Preview || !(preview.handle == playback_)) {
        if (phase_ == Phase::Preview) { Restore(world); }
        Prepare(world, preview.handle, arrivalPreview);
        phase_ = Phase::Preview;
    }
    Apply(world, preview.timeSeconds);
}

void IntroDirector::Apply(HIKARI::WorldView& world, double t) {
    screens_.time = t;
    if (ending_) {
        DriveLights(world, t);
        DriveEndingBlack(world, t);
        return;
    }
    DriveDepthOfField(world);
    DriveLetterbox(world, t);
    DriveLights(world, t);
    DriveBlack(world, t);
    DriveProps(world, t);
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
    phase_ = Phase::Done; wake_ = Wake::None;
    std::fprintf(stderr, "[TD2 intro] done\n");
}

void IntroDirector::Stop(HIKARI::WorldView& world) {
    if (phase_ == Phase::Title || phase_ == Phase::Playing || phase_ == Phase::Popups || phase_ == Phase::Arrival || phase_ == Phase::Ending) { Finish(world); }
    ending_ = false;
    if (phase_ == Phase::Preview) { Restore(world); playback_ = {}; }
    phase_ = Phase::None;
}

void IntroDirector::Skip(HIKARI::WorldView& world) {
    if (phase_ != Phase::Title && phase_ != Phase::Playing && phase_ != Phase::Popups && phase_ != Phase::Arrival) { return; }
    std::fprintf(stderr, "[TD2 intro] skipped\n");
    Finish(world);
}
}
