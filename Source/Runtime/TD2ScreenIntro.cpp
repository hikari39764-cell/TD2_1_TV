// オープニングと通関の画面：雪・白・消灯の段取りを各テレビの画面（砂嵐の窓への結び）へ写す。
#include "TD2Screen3D.h"
#include "TD2Screen3DShared.h"
#include "TD2StageProjection.h"
#include "TD2ScreenKitAssets.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/PresentationViews.h"
#include "HIKARI/Project/VfxView.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>

namespace TD2 {
namespace {
using namespace Stage3D;
// オープニングで飾りのテレビの信号が合う波の数（波ごとに一度、エンジンが GPU のシーンの経路を組み直す）。
constexpr double kIntroLockWaves = 3;
}

void Screen3DStage::ApplyIntro(HIKARI::WorldView& world, std::map<uint64_t, Binding>& next) const {
    if (!introStatic_) { return; }
    const auto view = std::find_if(views_.begin(), views_.end(), [](const View& v) { return v.kind == ViewKind::Noise; });
    if (view == views_.end() || !view->handle.IsValid()) { return; }
    const double t = intro_->time;
    const auto tick = static_cast<uint32_t>(std::floor(t * kNoiseFlips));
    // 段：0 黒、1 暗い砂嵐、2 砂嵐、3 明るい砂嵐。窓を横に 4 等分した範囲が一段（板は段ごとに別の明るさ）。
    // テレビごとに段の中の位置を変え、隣り合うテレビが同じ絵にならないようにする。
    // 4 は消える瞬間のブラウン管の線：黒い段の真ん中の細い線を、squash（0〜1）が進むほど広い範囲で写して細くする。
    // 5 は真っ白（黒い段の下の方の白い板）、6 はタイトル（2・3 段目に置いた仮タイトルの板）、
    // 7 は線のあとの光の点（黒い段の上の小さな白い点を、squash が進むほど広く写して小さくする）。
    struct Look { int band = -1; double squash = 0; };
    const auto bind = [&](HIKARI::GameProjectObjectHandle object, uint64_t key, Look look, const std::string& region) {
        double x = (look.band >= 4 ? 0 : look.band) * .25 + .015 + IntroHash(key, 1) * .02;
        double y = IntroHash(key, 2) * .4, w = .2, h = .55;
        if (look.band == 0) { y = IntroHash(key, 2) * .05; h = .15; }   // 黒：点と線にかからない一番上
        if (look.band == 4) { h = kNoiseLine + (.95 - kNoiseLine) * std::pow(std::clamp(look.squash, 0.0, 1.0), .6); y = .5 - h * .5; }
        if (look.band == 5) { y = .64 + IntroHash(key, 2) * .03; h = .3; }
        if (look.band == 6) { x = .25; y = 0; w = .5; h = 1; }
        if (look.band == 7) {
            const double k = std::clamp(look.squash, 0.0, 1.0);
            w = .04 + .16 * k; h = .04 + .16 * k;
            x = .125 - w * .5; y = kNoiseDot - h * .5;
        }
        next[object.value] = {object, view->handle, std::string(kScreenSlot),
                              {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)}, false, region};
    };
    // 通関：普段の画面のまま前兆で乱れ、最初の稲妻からはオープニングと同じく砂嵐になり（遠くのテレビは普段の画面を撮っていないので
    // 黒いまま残さない）、ゴールから遠い順にブラウン管のように消える。ゴールのテレビは扉を映したまま、最後にゆっくり消える。
    // 暗転の後は全部黒く、目覚めからはパソコンだけがタイトルを映す（オープニングのタイトル待ちと同じ姿）。
    if (intro_->ending) {
        const double first = intro_->lightning.empty() ? intro_->suckStart : intro_->lightning.front();
        const bool flash = std::any_of(intro_->lightning.begin(), intro_->lightning.end(), [&](float at) { return t >= at && t < at + .12; });
        const bool omen = IntroOmenBurst(t, intro_->flickerStart, first);
        const auto staticLook = [&](uint64_t key) {
            const double r = IntroHash(key, tick * 13u + 1u);
            return Look{r < .12 ? 0 : r < .42 ? 1 : r < .85 ? 2 : 3};
        };
        const auto endingLook = [&](uint64_t handle, bool computer) {
            if (t >= intro_->blackTime) { return computer && t >= intro_->revealTime ? Look{6} : Look{0}; }
            const bool goal = handle == intro_->goalScreen;
            const auto found = intro_->offTime.find(handle);
            const double off = found != intro_->offTime.end() ? found->second : intro_->suckStart + intro_->collapseSeconds;
            const double line = goal ? .45 : .16, dot = goal ? .5 : .2;
            if (t >= off + line + dot) { return Look{0}; }
            if (t >= off + line) { return Look{7, (t - off - line) / dot}; }
            if (t >= off) { return Look{4, (t - off) / line}; }
            if (flash) { return Look{3}; }
            const double r = IntroHash(handle, tick * 5u + 9u);
            if (t >= first) { return !goal ? staticLook(handle) : r < .06 ? Look{1 + int(r < .03)} : Look{}; }
            return omen && IntroHash(handle, 31) < .35 ? Look{1 + int(IntroHash(handle, tick) < .5)} : Look{};
        };
        std::set<uint64_t> placed;
        for (const auto* room : introRooms_) {
            const auto id = room->configuration.screenObject;
            const auto screen = world.FindSceneObject(id);
            if (!room->placed || !screen.IsValid()) { continue; }
            placed.insert(screen.GetHandle().value);
            if (const auto look = endingLook(screen.GetHandle().value, id == intro_->computerScreen); look.band >= 0) { bind(screen.GetHandle(), id, look, room->regionId); }
        }
        for (const auto& handle : intro_->tvScreens) {
            if (placed.contains(handle.value) || next.contains(handle.value)) { continue; }
            if (const auto look = endingLook(handle.value, false); look.band >= 0) { bind(handle, handle.value, look, {}); }
        }
        return;
    }
    // 砂嵐の始まり：パソコンに近いテレビほど早く、最初の稲妻の瞬間に机の周りから工場の奥へ広がる。
    if (introOnset_.empty()) {
        HIKARI::GameProjectTransform origin{};
        (void)world.FindSceneObject(intro_->computerScreen).GetWorldTransform(origin);
        const auto add = [&](uint64_t key, HIKARI::ObjectView object) {
            HIKARI::GameProjectTransform tv{};
            double d = 12;
            if (object.IsValid() && object.GetWorldTransform(tv)) {
                d = std::hypot(tv.position.x - origin.position.x, tv.position.y - origin.position.y, tv.position.z - origin.position.z);
            }
            const double spread = (std::max)(0.0f, intro_->staticFull - intro_->staticStart);
            const double reach = std::pow(std::clamp((d - .4) / 24.0, 0.0, 1.0), .7);
            introOnset_[key] = {intro_->staticStart + (std::max)(0.0, spread * reach + (IntroHash(key, 5) - .5) * .6), reach};
        };
        for (const auto* room : introRooms_) { add(room->configuration.screenObject, world.FindSceneObject(room->configuration.screenObject)); }
        for (const auto& handle : intro_->tvScreens) { add(handle.value, world.FindObject(handle)); }
    }
    // テレビごとの砂嵐の始まりとパソコンからの遠さ（0〜1）。
    const auto infoOf = [&](uint64_t key) {
        const auto found = introOnset_.find(key);
        return found != introOnset_.end() ? found->second : std::array<double, 2>{intro_->staticFull, 1.0};
    };
    // 稲妻の瞬間は砂嵐が一斉に明るくなる（電気が走った感じ）。前兆の電気の乱れでは手元のテレビとパソコンが一瞬砂嵐になる。
    const bool flash = std::any_of(intro_->lightning.begin(), intro_->lightning.end(), [&](float at) { return t >= at && t < at + .12; });
    const double first = intro_->lightning.empty() ? intro_->staticStart : intro_->lightning.front();
    const bool omen = IntroOmenBurst(t, intro_->flickerStart, first);
    // パソコン：タイトルの間は普段の画面、スペースの瞬間に乱れて消え、最初の稲妻まで黒い。吸い込まれる時は明るい砂嵐で光る。
    // ほかのテレビ：テレビごとの時刻から砂嵐になり、段を毎秒 kNoiseFlips 回入れ替えてちらつく。-1 は普段の画面のまま。
    const auto noise = [&](uint64_t key) {
        const double r = IntroHash(key, tick * 13u + 1u);
        return Look{r < .12 ? 0 : r < .42 ? 1 : r < .85 ? 2 : 3};
    };
    // 山場：全部が一度に消え、一斉に真っ白に点き、遠いテレビから一台ずつ線になって消える。始めはまばらで、
    // 手元へ近づくほど詰まる。最後はパソコンだけが白く残り、やがて砂嵐へ戻って吸い込みへ続く。
    const double alone = intro_->aloneEnd;
    // decorative は区域の無い飾りのテレビ（普段の画面が自分の材質で、結び付けを外して戻す）。
    const auto bandFor = [&](uint64_t key, bool computer, uint64_t handle, bool decorative) {
        if (t >= intro_->restoreTime) {
            // 暗転の間（カメラは画面の中）、パソコンは普段の画面に戻しておく（砂嵐の窓を写していると暗転の黒が茶色く浮く）。
            if (t < intro_->revealTime) { return computer ? Look{} : Look{0}; }
            // 電気が戻る：手元から遠くへ順に、点 → 横線 → 砂嵐と点く。信号は遠くから合っていき、パソコンが最後。
            const double reach = computer ? 0.0 : infoOf(key)[1];
            const double on = intro_->powerOn + .15 + std::pow(reach, .8) * intro_->powerSeconds * .9 + IntroHash(key, 21) * .2;
            double locked = intro_->lockStart + (1 - reach) * intro_->lockSeconds + IntroHash(key, 22) * .15;
            // 飾りのテレビは結び付けを外して普段の材質へ戻すので、外す時刻を遠い・中ほど・手前の三つの波にまとめる（外すたびに
            // エンジンが GPU のシーンの経路を組み直して数十 ms 止まる。細かく分けると信号が合っていく間ずっと引っかかる）。
            if (decorative) {
                const double wave = (std::max)(.25, (intro_->lockSeconds + .15) / kIntroLockWaves);
                locked = intro_->lockStart + std::ceil((locked - intro_->lockStart) / wave) * wave;
            }
            if (t < on) { return Look{0}; }
            if (t < on + .22) { return Look{7, 1 - (t - on) / .22}; }
            if (t < on + .44) { return Look{4, 1 - (t - on - .22) / .22}; }
            if (t < locked) { return noise(key); }
            return Look{};
        }
        if (computer) {
            if (t < intro_->titleOff) { return Look{6}; }
            if (t < intro_->titleOff + .3) { const double r = IntroHash(key, tick * 7u + 3u); return Look{r < .35 ? 6 : r < .7 ? 3 : 0}; }
            if (t < intro_->staticStart) { return Look{omen ? 2 + int(IntroHash(key, tick) < .5) : 0}; }
            if (t >= intro_->pullStart) { return Look{3}; }
            if (t >= alone) { return Look{IntroHash(key, tick) < .85 ? 3 : 2}; }
            // 見つめる間の終わりに白い画面がちらつき、砂嵐へ戻る。
            if (t >= alone - .3) { return Look{IntroHash(key, tick * 3u + 1u) < .5 ? 5 : 3}; }
            if (t >= intro_->whiteStart) { return Look{5}; }
            if (t >= intro_->blackoutStart) { return Look{0}; }
            return flash ? Look{3} : noise(key);
        }
        const auto info = infoOf(key);
        if (t < info[0]) { return omen && info[1] < .35 ? Look{1 + int(IntroHash(key, tick) < .5)} : Look{0}; }
        if (t >= intro_->whiteStart) {
            // 消える拍で、白い画面が横線へ潰れ、光の点になって消える（ブラウン管の電源を切った時）。
            const auto found = intro_->offTime.find(handle);
            const double off = found != intro_->offTime.end() ? found->second : intro_->suckStart + intro_->collapseSeconds;
            if (t >= off + .26) { return Look{0}; }
            if (t >= off + .12) { return Look{7, (t - (off + .12)) / .14}; }
            if (t >= off) { return Look{4, (t - off) / .12}; }
            return Look{5};
        }
        if (t >= intro_->blackoutStart) { return Look{0}; }
        return flash ? Look{3} : noise(key);
    };
    std::set<uint64_t> placed;
    for (const auto* room : introRooms_) {
        const auto id = room->configuration.screenObject;
        const auto screen = world.FindSceneObject(id);
        if (!room->placed || !screen.IsValid()) { continue; }
        placed.insert(screen.GetHandle().value);
        if (const auto look = bandFor(id, id == intro_->computerScreen, screen.GetHandle().value, false); look.band >= 0) { bind(screen.GetHandle(), id, look, room->regionId); }
    }
    // 装飾テレビと置いていない区域のテレビ：普段の画面が無いので、砂嵐になるまでは黒い段に結んでおく
    // （オープニングの始めに一度だけ結び、途中でシーンを作り直さない）。
    for (const auto& handle : intro_->tvScreens) {
        if (placed.contains(handle.value) || next.contains(handle.value)) { continue; }
        // 元に戻した後は結ばない（普段の材質へ戻る）。
        if (const auto look = bandFor(handle.value, false, handle.value, true); look.band >= 0) { bind(handle, handle.value, look, {}); }
    }
}
}
