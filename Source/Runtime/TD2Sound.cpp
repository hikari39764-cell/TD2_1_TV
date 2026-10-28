#include "TD2Sound.h"
#include "../Common/TD2Math.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace TD2 {
namespace {
using Bus = HIKARI::GameProjectAudioBus;
// 音量の移り（音量 1 あたりの秒）。
constexpr float kTitleIn = .4f, kTitleOut = 1.5f, kStageIn = 2.5f, kStageOut = 1.2f, kAmbientMove = 2.0f, kStepsMove = .08f;
// オープニングの電話：前兆が始まって少し後から鳴り、遠くから近づきながら揺れ、停電の瞬間に切れる（長くても 5 秒）。
constexpr double kPhoneDelay = .3, kPhoneLongest = 5, kPhoneApproach = 3, kPhoneCut = .12;
// 雷は稲妻の閃きから少し遅れて鳴らす（遠くに落ちた感じ）。
constexpr double kThunderDelay = .4;
bool Trace() { static const bool trace = std::getenv("TD2_SPIKE_EVENTS") != nullptr; return trace; }
std::string_view NameOf(std::string_view path) {
    const auto slash = path.find_last_of('/'); const auto dot = path.find_last_of('.');
    return path.substr(slash + 1, dot - slash - 1);
}
}

void SoundDirector::Load(HIKARI::WorldView& world) {
    loaded_ = true;
    const auto audio = world.GetAudio();
    const auto load = [&](std::string_view path) {
        const auto sound = audio.Load(path);
        if (!sound.IsValid()) { std::fprintf(stderr, "[TD2 sound] cannot load %.*s\n", static_cast<int>(path.size()), path.data()); }
        return sound;
    };
    for (Loop* loop : {&titleBgm_, &stageBgm_, &ambient_, &phone_, &footsteps_}) { loop->sound = load(loop->path); loop->failed = !loop->sound.IsValid(); }
    phone_.loop = false;
    ambient_.keep = footsteps_.keep = true;
    swap_ = load(kSwapPath); checkpoint_ = load(kCheckpointPath); blink_ = load(kBlinkPath); plate_ = load(kPlatePath); death_ = load(kDeathPath);
    popup_ = load(kPopupPath); spring_ = load(kSpringPath);
    for (size_t i = 0; i < thunder_.size(); ++i) { thunder_[i] = load(kThunderPaths[i]); }
}

void SoundDirector::Update(HIKARI::WorldView& world, const IntroDirector& intro, const Rules::BodyState& player, bool playing, double seconds) {
    if (!loaded_) { Load(world); }
    // 音量は最初の「TD2 音」の部品から（Play 中に変えてもすぐ効く）。無ければ既定値。
    settings_ = {};
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kSoundComponentType)};
    bool found = false;
    (void)world.ForEachComponents<SoundComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView, SoundComponent& component) { if (!found) { settings_ = component.Settings(); found = true; } });

    const auto& s = intro.Screens();
    const double t = s.time;
    float title = 0, stage = 0, ambient = settings_.ambientQuiet, phone = 0, steps = 0;
    if (intro.TitleWait()) {
        // タイトル待ち：タイトルの BGM、環境音は小さく。次のオープニングでもう一度電話を鳴らせるようにし、チェックポイントの
        // 記憶も最初からにする。
        title = settings_.titleBgm; phoneDone_ = false; lastCheckpoint_.clear();
    } else if (intro.Opening()) {
        // オープニング：タイトルの BGM を退かせ、環境音を主役にする。黒帯が退く頃に環境音を普段へ下げ、主人公が
        // 現れたら道中の BGM を入れる。
        ambient = t >= s.letterboxOut - 1.5 ? settings_.ambientQuiet : settings_.ambientCinematic;
        if (t >= s.arriveTime) { stage = settings_.stageBgm; }
        const double start = s.flickerStart + kPhoneDelay, end = (std::min)(double(s.blackoutStart), start + kPhoneLongest);
        if (!phoneDone_ && t >= start && t < end + kPhoneCut) {
            const double u = t - start;
            // 遠くの小さな音から近づき、ゆっくり揺れて（部屋の中を動くように）、終わりは切る。
            const double approach = .15 + .85 * Math::SmoothStep(u / kPhoneApproach);
            const double sway = .8 + .2 * std::sin(2 * Math::kPi * .7 * u);
            const double cut = t < end ? 1.0 : 1 - (t - end) / kPhoneCut;
            const double tail = end < s.blackoutStart ? Math::SmoothStep((end - t) / 1.0) : 1.0;   // 停電より先に 5 秒が来たら静かに消す
            phone = static_cast<float>(settings_.phone * approach * sway * std::clamp(cut, 0.0, 1.0) * (t < end ? tail : 1.0));
        } else if (t >= end + kPhoneCut) {
            phoneDone_ = true;
        }
    } else if (intro.EndingPlaying()) {
        // 通関：道中の BGM を退かせ、環境音を主役にする。
        ambient = settings_.ambientCinematic;
    } else {
        // 道中（オープニングを飛ばした時も）。歩いている間だけ足音。
        stage = settings_.stageBgm;
        steps = playing && player.grounded && !player.dead && std::abs(player.walkVx) > .01 ? settings_.footsteps : 0;
    }
    // 稲妻：通関の演出の秒が稲妻の時刻を越えたフレームで、三つの雷を順に一度ずつ。オープニングの雷と風は演出アセットの
    // 音のトラックが鳴らす（時刻は演出の編集で直す）ので、ここで鳴らすと二重になる。
    const bool ending = intro.EndingPlaying();
    if (ending && lastTime_ >= 0 && t >= lastTime_) {
        for (size_t i = 0; i < s.lightning.size(); ++i) {
            const double at = s.lightning[i] + kThunderDelay;
            if (lastTime_ < at && t >= at) { PlayOnce(world, thunder_[i % thunder_.size()], settings_.thunder, NameOf(kThunderPaths[i % thunder_.size()])); }
        }
    }
    lastTime_ = ending ? t : -1;
    const auto toward = [](Loop& loop, float target, float riseSeconds, float fallSeconds) {
        loop.rate = 1.0f / (std::max)(.01f, target > loop.volume ? riseSeconds : fallSeconds); loop.target = target;
    };
    toward(titleBgm_, title, kTitleIn, kTitleOut);
    toward(stageBgm_, stage, kStageIn, kStageOut);
    toward(ambient_, ambient, kAmbientMove, kAmbientMove);
    toward(footsteps_, steps, kStepsMove, kStepsMove);
    phone_.target = phone; phone_.rate = 1000;   // 電話は包絡そのものを毎フレーム与える

    // 診断（環境変数 TD2_SPIKE_EVENTS）：目標の音量が変わった時に出す（無頭では音が鳴らないので、ここで流れを確かめる）。
    if (static std::array<float, 4> last{-1, -1, -1, -1}; Trace()) {
        const std::array<float, 4> now{title, stage, ambient, phone > 0 ? 1.f : 0.f};
        if (now != last) {
            std::fprintf(stderr, "[TD2 sound] title=%.2f stage=%.2f ambient=%.2f phone=%d t=%.2f\n", title, stage, ambient, phone > 0, t);
            last = now;
        }
    }
    const auto audio = world.GetAudio();
    for (Loop* loop : {&titleBgm_, &stageBgm_, &ambient_, &phone_, &footsteps_}) { Drive(audio, *loop, seconds); }
}

void SoundDirector::Drive(HIKARI::AudioView audio, Loop& loop, double seconds) {
    if (loop.failed) { return; }
    const float step = static_cast<float>(loop.rate * seconds);
    loop.volume = loop.target > loop.volume ? (std::min)(loop.target, loop.volume + step) : (std::max)(loop.target, loop.volume - step);
    if (loop.playback.IsValid() && !audio.IsPlaying(loop.playback)) { loop.playback = {}; }   // 一度きりの音が終わった
    if (!loop.playback.IsValid()) {
        if (loop.volume <= 1e-3f && !(loop.keep && loop.target > 0)) { return; }
        HIKARI::GameProjectAudioPlayOptions options{};
        options.bus = loop.bus; options.loop = loop.loop ? 1 : 0; options.volume = loop.volume;
        loop.playback = audio.Play(loop.sound, options);
        return;
    }
    if (loop.volume <= 1e-3f && loop.target <= 0 && !loop.keep) { (void)audio.Stop(loop.playback); loop.playback = {}; return; }
    (void)audio.SetVolume(loop.playback, loop.volume);
}

void SoundDirector::PlayOnce(HIKARI::WorldView& world, HIKARI::GameProjectAudioSoundHandle sound, float volume, std::string_view name) {
    if (Trace()) { std::fprintf(stderr, "[TD2 sound] se=%.*s\n", static_cast<int>(name.size()), name.data()); }
    if (!sound.IsValid() || volume <= 0) { return; }
    (void)world.GetAudio().Play(sound, Bus::Se, 0, volume);
}

void SoundDirector::PlayPopups(HIKARI::WorldView& world, int count, float gain) {
    if (count <= 0 || !popup_.IsValid() || settings_.popup <= 0) { return; }
    const auto audio = world.GetAudio();
    for (int i = 0; i < (std::min)(count, 3); ++i) {
        // 0.85〜1.35 倍の高さで、古い音から差し替えて重ねる。
        popupSeed_ = popupSeed_ * 1664525u + 1013904223u;
        HIKARI::GameProjectAudioPlayOptions options{};
        options.bus = Bus::Se; options.volume = settings_.popup * gain;
        options.concurrency = HIKARI::GameProjectAudioConcurrency::ReplaceOldest; options.maximumInstances = 10;
        options.playbackRate = .85f + .5f * static_cast<float>(popupSeed_ >> 8) / static_cast<float>(1u << 24);
        (void)audio.Play(popup_, options);
    }
}

void SoundDirector::OnEvent(HIKARI::WorldView& world, const Rules::RuleEvent& event) {
    using Kind = Rules::EventKind;
    switch (event.kind) {
    case Kind::Blink: case Kind::Bounce: PlayOnce(world, blink_, settings_.blink, NameOf(kBlinkPath)); break;
    case Kind::EchoSwapped: PlayOnce(world, swap_, settings_.swap, NameOf(kSwapPath)); break;
    // チェックポイントは新しい所に替わった時だけ（同じ所に触れ直した時や、そこへ戻された時は鳴らさない）。
    case Kind::CheckpointActivated:
        if (event.subjectId != lastCheckpoint_) { lastCheckpoint_ = event.subjectId; PlayOnce(world, checkpoint_, settings_.checkpoint, NameOf(kCheckpointPath)); }
        break;
    case Kind::PlayerDied: PlayOnce(world, death_, settings_.death, NameOf(kDeathPath)); break;
    // バネが体（主人公か残影）を打ち上げた時。
    case Kind::SpringLaunched: PlayOnce(world, spring_, settings_.spring, NameOf(kSpringPath)); break;
    // 踏み板は踏まれた瞬間（pressed）だけ。踏み続けても、切り替え式の入り（latched）でも重ねて鳴らさない。
    case Kind::PlateChanged: if (event.detail == "pressed") { PlayOnce(world, plate_, settings_.plate, NameOf(kPlatePath)); } break;
    default: break;
    }
}

void SoundDirector::Stop(HIKARI::WorldView& world) {
    const auto audio = world.GetAudio();
    for (Loop* loop : {&titleBgm_, &stageBgm_, &ambient_, &phone_, &footsteps_}) {
        if (loop->playback.IsValid()) { (void)audio.Stop(loop->playback); }
        loop->playback = {}; loop->volume = loop->target = 0;
    }
    phoneDone_ = false; lastCheckpoint_.clear();
}
}
