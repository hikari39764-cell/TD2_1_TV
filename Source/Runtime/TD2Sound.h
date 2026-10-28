#pragma once
// ゲームの音：タイトル・オープニング・道中・通関の BGM と環境音の受け渡し、オープニングの電話、通関の稲妻の雷、規則の
// 出来事の効果音、歩く音。音量は「TD2 音」の部品（無ければ既定値）。音は Play の間だけ鳴らす。
// オープニングの雷と風は演出アセット（TD2_Intro）の音のトラックで鳴る。
#include "TD2Intro.h"
#include "Rules/TD2Rules.h"
#include "HIKARI/Project/AudioView.h"
#include "HIKARI/Project/GameComponent.h"
#include "HIKARI/Project/WorldView.h"
#include <array>
#include <string>
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kSoundComponentType = "TD2.Sound";

// 音の資産（Assets/Audio の下、英語名）。
inline constexpr std::string_view kTitleBgmPath = "Assets/Audio/BGM/BGM_Title.mp3";
// 道中の曲は継ぎ目なく繰り返すので FLAC（MP3 は頭と尾に無音が付き、繰り返しの継ぎ目が途切れる）。
// 元は forbidden_space_v2.wav：書き出し側の繰り返し区間で切り、区間の後の余韻を頭へ重ねてある。
inline constexpr std::string_view kStageBgmPath = "Assets/Audio/BGM/BGM_Stage.flac";
inline constexpr std::string_view kAmbientPath = "Assets/Audio/AMB/AMB_Rain.mp3";
inline constexpr std::string_view kPhonePath = "Assets/Audio/SE/SE_Phone.mp3";
inline constexpr std::string_view kSwapPath = "Assets/Audio/SE/SE_Swap.mp3";
inline constexpr std::string_view kCheckpointPath = "Assets/Audio/SE/SE_Checkpoint.mp3";
inline constexpr std::string_view kBlinkPath = "Assets/Audio/SE/SE_Blink.mp3";
inline constexpr std::string_view kFootstepsPath = "Assets/Audio/SE/SE_Footsteps.mp3";
inline constexpr std::string_view kPlatePath = "Assets/Audio/SE/SE_PlateTrigger.mp3";
inline constexpr std::string_view kDeathPath = "Assets/Audio/SE/SE_Death.mp3";
// バネに打ち上げられた音（元はジャンプ.mp3）。
inline constexpr std::string_view kSpringPath = "Assets/Audio/SE/SE_Spring.mp3";
// 弾窓が出る音（古いパソコンの「ピコン」。プログラムで作った短い音）。
inline constexpr std::string_view kPopupPath = "Assets/Audio/SE/SE_Popup.wav";
// 通関の稲妻の雷（順に使う）。
inline constexpr std::array<std::string_view, 3> kThunderPaths{"Assets/Audio/SE/SE_Thunder01.mp3", "Assets/Audio/SE/SE_LightningStrike1.mp3",
                                                              "Assets/Audio/SE/SE_LightningStrike2.mp3"};

struct SoundSettings {
    float titleBgm = .8f;        // タイトル待ちの BGM
    float stageBgm = .7f;        // 道中の BGM
    float ambientQuiet = .18f;   // タイトル待ちと道中の環境音（雨。小さく）
    float ambientCinematic = 1;  // オープニングと通関の環境音（主役）
    float phone = .9f;           // オープニングの電話
    // 道中の効果音。
    float swap = .7f, checkpoint = .7f, blink = .55f, footsteps = .55f, plate = .7f, death = .7f, spring = .55f;
    float thunder = .8f;         // 通関の雷
    float popup = .55f;          // 弾窓が出る音
};

class SoundComponent final : public HIKARI::IGameComponent {
public:
    const SoundSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Float("titleBgm", "タイトルの BGM", settings_.titleBgm, 0, 1);
        fields.Float("stageBgm", "道中の BGM", settings_.stageBgm, 0, 1);
        fields.Float("ambientQuiet", "環境音（普段）", settings_.ambientQuiet, 0, 1);
        fields.Float("ambientCinematic", "環境音（演出）", settings_.ambientCinematic, 0, 1);
        fields.Float("phone", "電話", settings_.phone, 0, 1);
        fields.Float("swap", "交換", settings_.swap, 0, 1);
        fields.Float("checkpoint", "チェックポイント", settings_.checkpoint, 0, 1);
        fields.Float("blink", "瞬間移動", settings_.blink, 0, 1);
        fields.Float("footsteps", "足音", settings_.footsteps, 0, 1);
        fields.Float("plate", "踏み板", settings_.plate, 0, 1);
        fields.Float("death", "死亡", settings_.death, 0, 1);
        fields.Float("spring", "バネ", settings_.spring, 0, 1);
        fields.Float("thunder", "雷", settings_.thunder, 0, 1);
        fields.Float("popup", "弾窓", settings_.popup, 0, 1);
    }
private:
    SoundSettings settings_;
};

class SoundDirector {
public:
    // 毎フレーム（演出の間も）。intro はオープニングと通関の進み、player は操作中の体（歩く音に使う）、
    // playing は規則が動いている（道中）か。
    void Update(HIKARI::WorldView& world, const IntroDirector& intro, const Rules::BodyState& player, bool playing, double seconds);
    // 規則の出来事の効果音（踏み板は踏まれた瞬間だけ、一度の踏みで一度。チェックポイントは新しく替わった時だけ）。
    void OnEvent(HIKARI::WorldView& world, const Rules::RuleEvent& event);
    // シーンを出る時：全部止める。
    void Stop(HIKARI::WorldView& world);
    // 続きから始めた時：そのチェックポイントは鳴らし済みにする（立たせた瞬間に鳴らさない）。
    void NoteCheckpoint(std::string id) { lastCheckpoint_ = std::move(id); }
    // 弾窓が count 個出た：一フレームに 3 つまで、音の高さを少しずつ変えて重ねる（嵐の間は鳴り続ける）。gain は音量に掛ける
    // （遊んでいる間のテレビの中の弾窓は小さく）。
    void PlayPopups(HIKARI::WorldView& world, int count, float gain = 1);

private:
    // 鳴らし続ける音（BGM・環境音・足音）。音量は目標へ秒あたり rate で近づき、0 になれば止める（keep なら止めない）。
    struct Loop {
        std::string_view path;
        HIKARI::GameProjectAudioBus bus = HIKARI::GameProjectAudioBus::Bgm;
        HIKARI::GameProjectAudioSoundHandle sound{};
        HIKARI::GameProjectAudioPlaybackHandle playback{};
        float volume = 0, target = 0, rate = 1;
        bool loop = true, keep = false, failed = false;
    };
    void Load(HIKARI::WorldView& world);
    void Drive(HIKARI::AudioView audio, Loop& loop, double seconds);
    void PlayOnce(HIKARI::WorldView& world, HIKARI::GameProjectAudioSoundHandle sound, float volume, std::string_view name);
    SoundSettings settings_;
    bool loaded_ = false;
    // 環境音は声の系統で鳴らす（設定の「環境音」の音量をこの系統に掛ける。このゲームに声は無い）。
    Loop titleBgm_{kTitleBgmPath}, stageBgm_{kStageBgmPath}, ambient_{kAmbientPath, HIKARI::GameProjectAudioBus::Voice}, phone_{kPhonePath, HIKARI::GameProjectAudioBus::Se};
    Loop footsteps_{kFootstepsPath, HIKARI::GameProjectAudioBus::Se};
    HIKARI::GameProjectAudioSoundHandle swap_{}, checkpoint_{}, blink_{}, plate_{}, death_{}, popup_{}, spring_{};
    uint32_t popupSeed_ = 1;      // 弾窓の音の高さの乱数
    std::array<HIKARI::GameProjectAudioSoundHandle, kThunderPaths.size()> thunder_{};
    double lastTime_ = -1;        // 前のフレームの演出の秒（稲妻を越えた瞬間を見る）
    std::string lastCheckpoint_;  // 最後に音を鳴らしたチェックポイント（同じ所に触れ直しても、戻されても鳴らさない）
    bool phoneDone_ = false;      // このオープニングで電話を鳴らし終えた
};
}
