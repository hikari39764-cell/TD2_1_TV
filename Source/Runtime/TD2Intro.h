#pragma once
// オープニングの演出。作り手の机の一人称（パソコンの画面にタイトルのメール）から始まり、スペースで工場の異変（灯の明滅・
// 稲妻・全部の画面の砂嵐）を経て、自分のパソコンの画面へ吸い込まれて暗転する（一つ目の演出アセット）。暗転の中は弾窓の
// 語りかけ（コードで描く）。その後、机の遠景で電気が戻り、パソコンの中に主人公が現れてゲームが始まる（二つ目の演出アセット）。
// カメラと画面エフェクトは演出アセット、灯・画面・小物・黒帯の時刻は演出アセットのイベントトラック（下の事件の名前）で、
// どちらも演出ワークスペースで編集する。効果は演出の秒だけで決まるので、Play と演出ワークスペースのプレビューで同じ絵になる。
// 二つの演出と間の弾窓は一本の時刻につなぎ、画面の表示と音は同じ時刻を読む。
// 通関はカメラを動かさないのでコードで流す：ゴールの扉が閉じ、ゴールのテレビが消え、弾窓（TD2Finale）の後に暗転する。
// 目覚めはタイトル待ちの構図のまま、まぶたが開いて顔を上げる。
#include "HIKARI/Project/CameraLensTypes.h"
#include "HIKARI/Project/EnvironmentTypes.h"
#include "HIKARI/Project/GameComponent.h"
#include "HIKARI/Project/SequenceTypes.h"
#include "HIKARI/Project/WorldView.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace TD2 {
inline constexpr std::string_view kIntroComponentType = "TD2.Intro";
// 砂嵐の板（暗い・普通・明るい）。画面の舞台がこれを撮り、テレビは段ごとの範囲を切り出して映す。
inline constexpr std::array<std::string_view, 3> kStaticPlaneGuids{
    "1c8e4a6f2d9b47e3a0f5c2b7d8e6a914", "7a3f9c1e5b2d48a6b9e0c4f7d1a2e358", "c4d2e8f1a6b94375a2e7f0c9b3d8e146"};
// 全部のテレビが一斉に白くなる時の真っ白な板。
inline constexpr std::string_view kWhitePlaneGuid = "d40d696ce6b34b86b22adc341d837970";
// 黒帯に使う真っ黒な小さなテクスチャ（Assets からの相対）。
inline constexpr std::string_view kLetterboxTexture = "TD2/Intro/T_TD2_LetterboxBlack.png";
// 白い板（点灯・消灯の点と線）。
inline constexpr std::string_view kWhiteTexture = "TD2/Intro/T_TD2_White.png";

// 暗転の中の弾窓の段取り（秒）。初めの kIntroBlackHold 秒は画面へ飛び込んだ後の黒だけを見せ、弾窓が済んだ後に
// 登場の演出が続く。
inline constexpr double kIntroBlackHold = 1.35;
inline constexpr double kIntroPopupsSeconds = kIntroBlackHold + 6.75;

// 演出アセットのイベントトラックで使う事件の名前。始まりの時刻と長さを演出ワークスペースで動かす。
namespace IntroEvent {
// 一つ目（タイトルから暗転まで）。
inline constexpr std::string_view TitleOff = "タイトル消去";    // 始まり：パソコンのタイトルが乱れて消える
inline constexpr std::string_view Omen = "前兆";            // 始まり：灯が揺れ始める（最初の稲妻まで前兆）
inline constexpr std::string_view Lightning = "稲妻";       // 始まり：稲妻（何度でも置ける）
inline constexpr std::string_view Static = "砂嵐拡散";      // 範囲：手元から最も遠いテレビまで砂嵐が広がる
inline constexpr std::string_view Blackout = "停電";        // 始まり：全部の灯とテレビが一度に消える
inline constexpr std::string_view White = "白画面";           // 始まり：全部のテレビが一斉に白く点く
inline constexpr std::string_view Collapse = "順に消灯";    // 範囲：最初の拍から最後の拍まで、遠いものから消える
inline constexpr std::string_view Alone = "凝視";           // 終わり：パソコンだけが白く残る時の終わり（砂嵐に変わる）
inline constexpr std::string_view Pull = "吸い込み";            // 範囲：引かれ始めから画面に飛び込んで切れるまで（終わりで暗転）
// 二つ目（暗転の後の机の遠景から、ゲームが始まるまで）。
inline constexpr std::string_view Reveal = "遠景";          // 始まり：机の遠景に切り替わる（暗いまま）
inline constexpr std::string_view Power = "復電";           // 範囲：手元から遠くへ電気が戻る
inline constexpr std::string_view Lock = "信号合わせ";          // 範囲：遠くから信号が合う（パソコンが最後）
inline constexpr std::string_view Arrive = "登場";          // 始まり：パソコンの中に主人公が現れる
// 両方：映画の黒帯が入ってから退くまで（一つ目で入り、二つ目の終わりで退く）。
inline constexpr std::string_view Letterbox = "黒帯";
}

struct IntroSettings {
    bool enabled = true;
    std::string sequence;              // 一つ目の演出アセット（タイトルから暗転まで）
    std::string arrival;               // 二つ目の演出アセット（机の遠景から主人公の登場まで）
    uint64_t cameraObject = 0;         // 演出が動かすゲームのカメラ
    uint64_t computerScreen = 0;       // プレイヤーのパソコン（タイトルを映し、最後に吸い込まれる画面）
    std::array<std::string, 4> tvModels;   // 砂嵐を出す装飾テレビの型（区域のテレビは自動で入る）
    uint64_t lightning1 = 0, lightning2 = 0;   // 窓際の稲妻の灯（普段は消しておく。広い範囲の影は壊れるので影は落とさない）
    uint64_t screenGlow = 0;           // パソコンの画面の前の小さな灯（画面の明るさに合わせて点け消しする）
    // 消える拍の数：テレビと灯をパソコンからの遠さで一列に並べ、遠い方から、だんだん詰まる拍ごとに一まとまりずつ消す。
    int collapseBeats = 8;
    float lightningPeak = 400000.0f;  // 稲妻の灯の一番明るい時（ルーメン）
    float flicker = 1.0f;              // 明滅の強さ（0 で灯はそのまま）
    float depthOfField = .3f;          // オープニングの間の被写界深度のぼけ（カメラの設定に掛ける倍率）
    float letterbox = 2.35f;           // 映画の黒帯の画面比（16:9 の画面で見た比。0 で無し）
    uint64_t hiddenEntry = 0;          // 隠しの道の入口の部屋のテレビ（通関の問いで本当のマウスを使った時に移る）
    std::array<uint64_t, 6> props{};
};

class IntroComponent final : public HIKARI::IGameComponent {
public:
    const IntroSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Bool("enabled", "最初からの Play でオープニング", settings_.enabled);
        fields.AssetId("sequenceAssetGuid", "オープニングの演出", HIKARI::GameProjectAssetKind::Sequence, settings_.sequence);
        fields.AssetId("arrivalAssetGuid", "登場の演出", HIKARI::GameProjectAssetKind::Sequence, settings_.arrival);
        fields.SceneObject("cameraObject", "ゲームカメラ", settings_.cameraObject);
        fields.SceneObject("computerScreen", "主人公のパソコン", settings_.computerScreen);
        fields.AssetId("tvModel1AssetGuid", "砂嵐の飾りテレビ 1", HIKARI::GameProjectAssetKind::Model, settings_.tvModels[0]);
        fields.AssetId("tvModel2AssetGuid", "砂嵐の飾りテレビ 2", HIKARI::GameProjectAssetKind::Model, settings_.tvModels[1]);
        fields.AssetId("tvModel3AssetGuid", "砂嵐の飾りテレビ 3", HIKARI::GameProjectAssetKind::Model, settings_.tvModels[2]);
        fields.AssetId("tvModel4AssetGuid", "砂嵐の飾りテレビ 4", HIKARI::GameProjectAssetKind::Model, settings_.tvModels[3]);
        fields.SceneObject("lightning1", "稲妻の灯 1", settings_.lightning1);
        fields.SceneObject("lightning2", "稲妻の灯 2", settings_.lightning2);
        fields.SceneObject("screenGlow", "パソコンの画面の光", settings_.screenGlow);
        fields.Int("collapseBeats", "消灯の拍数", settings_.collapseBeats, 2, 16);
        fields.Float("lightningPeak", "稲妻の明るさ", settings_.lightningPeak, 0, 2000000);
        fields.Float("flicker", "灯の明滅", settings_.flicker, 0, 1);
        fields.Float("depthOfField", "被写界深度の強さ", settings_.depthOfField, 0, 1);
        fields.Float("letterbox", "黒帯の画面比", settings_.letterbox, 0, 4);
        fields.SceneObject("hiddenEntry", "隠しステージの入口", settings_.hiddenEntry);
        if (fields.Section("吸い込まれる机の小物")) {
            for (size_t i = 0; i < settings_.props.size(); ++i) {
                const std::string id = "prop" + std::to_string(i + 1), label = "小物 " + std::to_string(i + 1);
                fields.SceneObject(id, label, settings_.props[i]);
            }
        }
    }
private:
    IntroSettings settings_;
};

// 前兆の電気の乱れ：灯が揺れ始めてから最初の稲妻までの間、0.2 秒の区切りごとに時々起こり、稲妻が近いほど多い。
// 灯（IntroDirector）と画面（Screen3D）が同じ時刻に乱れるよう、同じ式を使う。
inline bool IntroOmenBurst(double t, double start, double end) {
    if (!(end > start) || t < start || t >= end) { return false; }
    const double p = (t - start) / (end - start);
    uint32_t h = static_cast<uint32_t>(std::floor(t * 5)) * 0x9E3779B1u + 0x51ED27u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (h & 0xFFFFu) / 65536.0 < .06 + .32 * p * p;
}

// 消える拍の時刻：k 番目（0 から beats-1）。間隔は始めが長く、終わりへ向けて三分の一まで詰まる。
inline double IntroBeatTime(int k, int beats, double start, double span) {
    const double x = beats > 1 ? double(k) / (beats - 1) : 1.0;
    return start + span * x * (1.5 - .5 * x);
}
// 遠い方から数えて order 番目（全 count 個）が消える拍。拍が進むほど一拍で消える数を増やす（最初はぽつぽつ、最後は一斉に）。
inline int IntroBeatOf(size_t order, size_t count, int beats) {
    if (count == 0 || beats <= 1) { return beats - 1; }
    const double f = (double(order) + .5) / double(count);
    for (int k = 0; k < beats; ++k) {
        if (f <= std::pow(double(k + 1) / beats, 1.6)) { return k; }
    }
    return beats - 1;
}

// イベントトラックから読んだオープニングの時刻（一本につないだ秒）。置かれていない事件は起こらない（kIntroNever）。
inline constexpr float kIntroNever = 1.0e6f;
struct IntroTimeline {
    float titleOff = kIntroNever, flickerStart = kIntroNever, staticStart = kIntroNever, staticFull = kIntroNever;
    float blackoutStart = kIntroNever, whiteStart = kIntroNever, suckStart = kIntroNever, collapseSeconds = 1;
    float aloneEnd = kIntroNever, pullStart = kIntroNever, blackTime = kIntroNever, restoreTime = kIntroNever;
    float revealTime = kIntroNever;
    float powerOn = kIntroNever, powerSeconds = 1, lockStart = kIntroNever, lockSeconds = 1, arriveTime = kIntroNever;
    float letterboxIn = kIntroNever, letterboxOut = kIntroNever;
    float closeStart = kIntroNever;    // 通関：扉が閉じ始める時刻
    std::vector<float> lightning;      // 稲妻の時刻（この瞬間は砂嵐が全部明るく光る）
};
// イベントの区間から時刻を読み、offset 秒ずらす。missing には置かれていない事件の名前を並べる（ログ用）。
// arrival は二つ目の演出（遠景から登場まで）として読む。
IntroTimeline ReadIntroTimeline(const std::vector<HIKARI::GameProjectSequenceEventClip>& clips, bool arrival, float offset, std::string& missing);

// 画面の表示へ渡すオープニングの状態（区域のテレビと装飾テレビの砂嵐、パソコンのタイトル）。
struct IntroScreens : IntroTimeline {
    bool active = false;               // オープニングの間（タイトル待ちとプレビューを含む）
    bool ending = false;               // 通関の間（画面の消え方が違う）
    double time = 0;                   // 一本につないだ秒（タイトル待ちは 0）
    uint64_t computerScreen = 0;       // パソコンの画面の物体（シーンの ID）
    uint64_t goalScreen = 0;           // 通関：ゴールのテレビ（物体の handle）。扉が閉じると消える
    std::vector<HIKARI::GameProjectObjectHandle> tvScreens;   // 砂嵐を出すテレビ（型で見つけたもの。区域のテレビも含む）
    std::unordered_map<uint64_t, float> offTime;   // テレビ（物体の handle）ごとの白い画面が消える時刻
};

// オープニングを動かす。Play ではゲームのセッションが、演出ワークスペースのプレビューでは画面の表示が持つ。
// どちらも同じ時刻の式で灯・小物・黒帯・暗転を出し、終わると作者の状態へ戻す。
class IntroDirector {
public:
    // タイトル待ちの入り方：普通・通関の後の目覚め・隠しの道を通した後の目覚め（パソコンだけが残る）。
    enum class Wake { None, Normal, TrueEnd };
    // Play の最初のフレームに呼ぶ。オープニングを使うなら true（タイトル待ちから始める）。preview は「この画面から」。
    bool Begin(HIKARI::WorldView& world, bool preview, Wake wake = Wake::None);
    // Play の毎フレーム。オープニング・通関の間は true（規則は止める）。start はこのフレームでスペースを押したか。seconds はフレームの秒。
    bool Update(HIKARI::WorldView& world, bool start, double seconds = 0);
    // ゴールの扉に入った時に呼ぶ（扉が閉じ、ゴールのテレビが消える）。暗転は BlackOut で始め、EndEnding で終える。
    void BeginEnding(HIKARI::WorldView& world, HIKARI::GameProjectObjectHandle goalScreen);
    void BlackOut() { if (phase_ == Phase::Ending && screens_.blackTime >= kIntroNever) { screens_.blackTime = static_cast<float>(screens_.time); } }
    void EndEnding(HIKARI::WorldView& world);
    // 編集中の毎フレーム。演出ワークスペースがオープニングの演出をプレビューしている間はその時刻の姿を出し、
    // プレビューが終わる・別の演出に替わると元へ戻す。
    void Preview(HIKARI::WorldView& world);
    // シーンを出る時：灯を戻し、演出を止める。
    void Stop(HIKARI::WorldView& world);
    // 開発用の早送り：タイトル待ちでも流している途中でも、終わった時と同じ姿へ戻してゲームを始める。
    void Skip(HIKARI::WorldView& world);
    const IntroScreens& Screens() const noexcept { return screens_; }
    const IntroSettings& Settings() const noexcept { return settings_; }
    // 今の段（音の受け渡しに使う）：タイトル待ち・オープニングを流している・通関。
    bool TitleWait() const noexcept { return phase_ == Phase::Title; }
    bool Opening() const noexcept { return phase_ == Phase::Playing || phase_ == Phase::Popups || phase_ == Phase::Arrival; }
    bool EndingPlaying() const noexcept { return phase_ == Phase::Ending; }
    // 目覚めの経過秒（目覚めでなければ負）と、隠しの道を通した後の目覚めか。
    double WakeSeconds() const noexcept { return phase_ == Phase::Title && wake_ != Wake::None ? titleSeconds_ : -1; }
    bool TrueEnd() const noexcept { return wake_ == Wake::TrueEnd; }
    // 暗転の中の弾窓が新しく出た数（このフレームまでの分。読むと 0 に戻る）。効果音に使う。
    int TakePopupSounds() { return std::exchange(popupSounds_, 0); }
    // 登場の演出が終わりまで流れた（このフレームにゲームのカメラへ引き継ぐ）。読んだら消える。
    bool TakeHandoff() { return std::exchange(handoff_, false); }

private:
    enum class Phase { None, Title, Playing, Popups, Arrival, Ending, Preview, Done };
    // reach はパソコンからの遠さ（0 が手元、1 が工場の奥）。off は白い画面の間に灯が消える時刻（同じ遠さのテレビと同じ拍）。
    struct Lamp { HIKARI::GameProjectObjectHandle handle{}; float base = 0, sent = -1; uint32_t seed = 0; bool lightning = false, enabled = false, glow = false; float reach = 1, off = 0; };
    struct Prop { HIKARI::GameProjectObjectHandle handle{}; HIKARI::GameProjectTransform rest{}; uint32_t seed = 0; };
    // 設定の部品を探し、演出のイベントの区間から時刻を読み、テレビと灯の消える拍を決める。arrival は二つ目の演出だけをプレビューする時。
    bool Prepare(HIKARI::WorldView& world, HIKARI::GameProjectSequencePlaybackHandle playback, bool arrival = false);
    // 暗転の中の弾窓が終わった：二つ目の演出を流し、その時刻を一本の時刻へつなぐ。流せなければ false。
    bool StartArrival(HIKARI::WorldView& world);
    bool ReadSettings(HIKARI::WorldView& world);
    void Collect(HIKARI::WorldView& world);
    // テレビを遠い順に並べて拍へ割り振り、灯も同じ遠さのテレビの拍で消す。
    void ScheduleCollapse(HIKARI::WorldView& world);
    // 演出の t 秒の姿を出す（時刻だけで決まる。巻き戻しても同じ）。
    void Apply(HIKARI::WorldView& world, double t);
    void DriveLights(HIKARI::WorldView& world, double t);
    // 暗転の中の弾窓（タイトルの知らせ・楽しんでね・嵐）。u は弾窓が始まってからの秒。
    void DrawPopups(HIKARI::WorldView& world, double u, double seconds);
    // 画の上を UI の黒で覆う（画面へ飛び込んで切れた後と、二つ目の演出が遠景を出すまでのつなぎ）。
    void DrawBlack(HIKARI::WorldView& world) const;
    // 通関の灯：暗転で全部消える。
    double EndingLamp(const Lamp& lamp, double t) const;
    // 通関の暗転：画面全体を UI の黒で覆う。
    void DriveEndingBlack(HIKARI::WorldView& world, double t) const;
    // 目覚め：まぶたのように上下から開き（途中で一度瞬く）、机に伏せた顔を上げ、ぼけからピントが合う。u は目覚めの秒。
    void DriveWake(HIKARI::WorldView& world, double u);
    // タイトル待ちの寄り：画角を狭めてパソコンの画面を大きく写す（メールの字が読める大きさ）。weight は 0〜1。
    void DriveTitleZoom(HIKARI::WorldView& world, double weight) const;
    // タイトル待ちのピント：パソコンの画面の中心に合わせる（作者のピントは奥の工場にあり、近い画面の字がぼける）。weight は 0〜1。
    void DriveTitleFocus(HIKARI::WorldView& world, double weight) const;
    // 演出の暗転は描線と仕上げの色より前に掛かるので、暗転の間は色調整を 0 倍して本当の黒にする。
    void DriveBlack(HIKARI::WorldView& world, double t);
    // 机の小物：吸い込みの前に震え、引かれ始めると順に画面へ飛んで消える。
    void DriveProps(HIKARI::WorldView& world, double t);
    void RestoreProps(HIKARI::WorldView& world);
    // カメラの被写界深度のぼけを弱めて毎フレーム出す（ピントはゲームのカメラの合わせ方のまま）。
    void DriveDepthOfField(HIKARI::WorldView& world, double scale = 1);
    // 映画の黒帯：黒帯の範囲の始めに上下から入り、終わりに退く。画面の比に合わせて毎フレーム出す。
    void DriveLetterbox(HIKARI::WorldView& world, double t) const;
    // タイトル待ちの間、演出で使う UI のテクスチャ（黒帯・白）を見えない 1 画素で毎フレーム出しておく
    // （初めて出したフレームに読み込みで 0.1 秒ほど止まるのを、演出の外へ移す）。
    void WarmUi(HIKARI::WorldView& world) const;
    // 灯・小物・色調整・カメラを作者の状態へ戻す（演出は止めない）。
    void Restore(HIKARI::WorldView& world);
    void Finish(HIKARI::WorldView& world);
    Phase phase_ = Phase::None;
    HIKARI::GameProjectColorGrading grading_{};
    bool gradingSaved_ = false, gradingOverridden_ = false;
    float blackSent_ = -1;
    bool ending_ = false;   // 流している（プレビューしている）のが通関か
    Wake wake_ = Wake::None;
    bool handoff_ = false;   // 登場の演出が終わった（TakeHandoff で読む）
    IntroSettings settings_;
    HIKARI::GameProjectSequencePlaybackHandle playback_{};
    std::vector<Lamp> lamps_;
    std::vector<Prop> props_;
    bool propsMoved_ = false;   // 小物を作者の位置から動かしているか（戻す時だけ位置を書き直す）
    HIKARI::GameProjectFloat3 screenCenter_{};   // パソコンの画面の中心（世界座標）
    HIKARI::GameProjectDepthOfFieldSettings depthOfField_{};   // カメラに作者が付けた被写界深度
    bool depthOfFieldRead_ = false;
    // タイトル待ちの寄りの進み（0〜1）と、オープニングを始めた時の寄り（始めの 1.2 秒で 0 へ戻す）。
    double titleZoom_ = 0, zoomAtStart_ = 0, titleSeconds_ = 0;
    // 一つ目の演出の長さ（暗転の中の弾窓はこの後から数える）、弾窓の経過秒、二つ目の演出の始まり（一本の時刻）。
    double firstSeconds_ = 0, popupSeconds_ = 0, arrivalOffset_ = 0;
    int revealCovered_ = 0;   // 二つ目の演出が遠景を出した後、まだ黒で覆ったフレームの数
    int popupSounds_ = 0;
    IntroScreens screens_;
};
}
