#pragma once
#include "../Common/TD2RegionData.h"
#include "Rules/TD2Rules.h"
#include "TD2ScreenEffects.h"
#include "TD2ScreenFx.h"
#include "TD2ScreenModels.h"
#include "TD2ScreenStyle.h"
#include "TD2StagePuppet.h"
#include "TD2StageDevices.h"
#include "TD2Intro.h"
#include "HIKARI/Project/SceneCaptureView.h"
#include "HIKARI/Project/WorldView.h"
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace TD2 {
// テレビの画面の材質スロット名（どの型のモデルも同じ名前）。
inline constexpr std::string_view kScreenSlot = "TD2_Screen";
// 画面を映すテレビの物体（区域の付いた物体そのもの）と、画面の明るさ。
struct ScreenConfiguration {
    uint64_t screenObject = 0;
    float brightness = 2.2f;
};
// 舞台に並べる一部屋。x, y は舞台上のマス位置（世界に置いた部屋は世界の位置、未配置の地図は世界の外に並べた位置）。
// region は描く間だけ借りる（Play は Session の地図、編集中は ownedRegion へ写した値）。
// テレビのモデルのローカル空間での画面。カメラに向いた画面の判定に使う。
struct ScreenSurface { float centerX = 0, centerY = .4f, centerZ = .28f, width = .84f, height = .49f; };
// 画面がゲームカメラの画にどう写っているか。coverage は画の中に入った面積の割合（裏向きは 0）、depth は画面の中心
// までの奥行き、centerX/Y は中心の画の位置（-1〜1）。鮮明に撮る部屋・保護・焦点の選択に使う。
// eyeX/Y は画面の右・上に対する目の向きの正接（画面の中の奥の層を見る角度）。reach は画を一回り広げた枠に入った割合
// （画の縁の少し外の画面も先に撮っておき、カメラが振れた瞬間に黒い画面を見せない）。
struct ScreenOnCamera { double coverage = 0, reach = 0, depth = 0, centerX = 0, centerY = 0, facing = 0, eyeX = 0, eyeY = 0; };
ScreenOnCamera ProjectScreen(const HIKARI::GameProjectCameraPose& pose, const HIKARI::GameProjectTransform& tv, const ScreenSurface& surface);
struct StageRoom {
    HIKARI::GameProjectObjectHandle owner;
    std::string regionId;
    int x = 0, y = 0;
    // 隣り合いの判定（部屋の外周の口がつながるか、地形が隣へ続くか）に使う世界の位置。Play では x, y と同じ。
    // 編集中は描く位置 x, y だけを詰めて並べ直す（PackRoomsForEditor）ので、世界の位置をここに残す。
    int linkX = 0, linkY = 0;
    // 編集中に詰めて並べた組（0 か 1）。組ごとに別の窓で撮る。
    int editorGroup = 0;
    bool placed = true;
    // 機構で 180° 回されている。region は作者の向きのまま（画面は絵ごと逆さに写る）。
    bool flipped = false;
    // 回す機構がある部屋。中の装置はテレビの傾きの分だけ逆に回して描き、回っている途中も正立に見せる。
    // restTurn は置いたままのテレビの傾き（最初から逆さに吊ったテレビは π）。
    bool flippable = false;
    double restTurn = 0;
    const RegionDefinition* region = nullptr;
    RegionDefinition ownedRegion;
    ScreenConfiguration configuration;
    ScreenSurface surface;
    ScreenStyleSettings style;
    const RegionDefinition& Region() const { return region ? *region : ownedRegion; }
};
// 舞台を動かす世界座標の状態。装置の ID は「区域ID/装置ID」。activeRegion は操作中の部屋。
// simulated は Play の snapshot。編集中は呼び手が作者の装置とスポーン位置から作る。
struct StageFrame {
    const Rules::WorldSnapshot* snapshot = nullptr;
    std::string activeRegion;
    bool simulated = false;
    bool authoring = false;
    // 大区域の切り替え中なら、出る部屋と入る部屋。一番細かい段・保護をこの二部屋に先に回す。
    std::string travelFrom, travelTo;
    // オープニングの演出（タイトルと砂嵐）。無ければ普段どおり。
    const IntroScreens* intro = nullptr;
};

// 編集中の並べ直し：全部の部屋を一枚ずつの窓に収めるため、描く位置（x, y）だけを二つの組に詰めて並べる。
// 世界の並びのまま写すと、大きな地図（500 マス四方など）では 1 マス 4 texel ほどになり画面がひどくぼやける。
// 呼ぶ前に linkX, linkY へ世界の位置を入れておく。
void PackRoomsForEditor(std::vector<StageRoom>& rooms);

// 一つの World に一つの連続した舞台。世界地図どおりに部屋を置き、プレイヤー・光線・エフェクトは世界に一つずつ。
// Play では写っているテレビごとに、外のカメラがその画面を見る角度で自分の部屋を覗く視点を作り（覗き窓）、細かさの段ごとに
// 一枚のキャプチャへ並べて描く。編集中は正投影の窓（鮮明・未配置）で全部の部屋を撮り、各テレビは自分の範囲を切り出す。
// 診断（環境変数 TD2_STAGE_TRACE）：舞台の各段にかかった時間を区間ごとに足し、240 フレームごとに平均を出す。
// 変数が無ければ何も測らない。
class StageTimer {
public:
    explicit StageTimer(const char* name);
    ~StageTimer();
    StageTimer(const StageTimer&) = delete; StageTimer& operator=(const StageTimer&) = delete;
    static void EndFrame();
private:
    const char* name_ = nullptr; long long start_ = 0;
};

class Screen3DStage {
public:
    // stage は舞台カメラに付けた共有設定。配色・カメラの角度・舞台カメラの指定はここだけが効く。
    // fx はイベントごとのエフェクト（VFX 資産）。舞台の上で出し、キャプチャがそのまま写す。
    void Render(HIKARI::WorldView& world, const std::vector<StageRoom>& rooms, const StageFrame& frame,
                const ScreenStyleSettings& stage, const ScreenFxSettings& fx);
    bool Release(HIKARI::WorldView& world);
    const std::string& Status() const noexcept { return status_; }

private:
    // 一枚の舞台カメラで撮る窓の種類。鮮明と未配置は編集中の正投影の二枚（詰めて並べた地図の二組）、砂嵐はオープニングの
    // ノイズの板を撮る透視の窓。Play のテレビの画面は窓でなく、テレビごとの覗き窓（Portal）で撮る。
    enum class ViewKind : int { Sharp = 0, Area = 1, Noise = 2 };
    static constexpr size_t kViewKinds = 3;
    // 正投影カメラを持つ窓の種類（編集中だけ使う）。
    static constexpr size_t kOrthoKinds = 2;
    // 窓は世界のマスの範囲（行数は正投影の高さで決まる）と、キャプチャの大きさ（texel）。
    struct Window {
        double x = 0, y = 0, cols = 0, rows = 0;
        int density = 16;
        uint32_t width = 0, height = 0;
        bool valid = false;
        // 透視の窓（砂嵐）：目から正面の面までの距離（舞台の長さ）。0 なら正投影。
        double distance = 0;
        bool Contains(double left, double top, double right, double bottom) const {
            constexpr double e = 1e-6;
            return valid && left >= x - e && top >= y - e && right <= x + cols + e && bottom <= y + rows + e;
        }
    };
    // 窓ごとの舞台カメラ（別々の物体。Play を止める時に元の姿勢へ戻す）。
    struct StageCamera {
        uint64_t sceneId = 0;
        HIKARI::GameProjectObjectHandle handle{};
        HIKARI::GameProjectTransform original{};
        double orthographicHeight = 0;
        double verticalFov = 0;   // 透視カメラの縦の画角（ラジアン）
    };
    // キャプチャ一枚。tier が 0 以上なら Play の覗き窓を並べる細かさの段、-1 なら窓（kind）を一台の舞台カメラで撮る。
    struct View {
        ViewKind kind = ViewKind::Sharp;
        int tier = -1;
        ScreenStyleSettings style;
        HIKARI::GameProjectSceneCaptureHandle handle{};
        HIKARI::GameProjectObjectHandle camera{};
        uint32_t width = 0, height = 0, flags = 0;
        float brightness = -1;          // 前のフレームに送った明るさ（変わったら全部の覗き窓を描き直す）
        bool parkLast = false;          // 前のフレームに下端の隅の視点を並べたか
    };
    struct Binding {
        HIKARI::GameProjectObjectHandle object{};
        HIKARI::GameProjectSceneCaptureHandle capture{};
        std::string slot;
        HIKARI::GameProjectSceneCaptureSourceRect rect{};
        bool protect = false;   // 後処理から画面を守る（カメラに向いて写っている画面、最大 12）
        std::string regionId;
    };
    // 部屋の背景と地形の描画要求。占有・置き場所・写し方・配色が同じ間は作り直さない。
    struct RoomParts {
        std::vector<char> occupancy, linked;
        int x = 0, y = 0, palette = -1;
        double scale = 0, pitch = 0, yaw = 0;
        bool compact = false, hall = false;
        int depthRows = 0;
        double playDepth = 0;
        WindowSpan window;
        std::vector<PartRequest> parts;
        // 窓の向こうの大広間（目の向きが同じ間は作り直さない。透視の窓では向きは常に 0）。
        std::array<double, 2> hallEye{};
        bool hallValid = false;
        std::vector<PartRequest> hallParts;
        // 作り直しても部品の組を減らさないための、系列ごとの今までの数（背景・地形と大広間）。
        StagePartBuilder::SeriesCounts series, hallSeries;
        // Play で残しておく要求（retain）として出した部品（持ち主・ID）。作り直すまで出し直さない。
        std::vector<std::pair<uint64_t, uint64_t>> retained;
        // 最後に出した中身（ID ごと）。作り直しても変わっていない部品は出し直さない（隣の部屋が動いて鎧戸が数枚
        // 変わっただけで、部屋の数百の部品をエンジンに比べ直させていた）。
        std::unordered_map<uint64_t, PartRequest> submitted;
        bool retainedValid = false;
    };
    struct Assigned { const StageRoom* room = nullptr; ViewKind kind = ViewKind::Sharp; };
    // Play の覗き窓：一台のテレビの部屋だけを、外のカメラがその画面を見る角度で撮る視点（目は部屋の正面から一定の距離、
    // 視錐台は部屋の正面の矩形を通る）。画面ごとに角度がカメラだけで決まるので、どのテレビもほかの画面の都合で跳ばない。
    // tier の段のキャプチャの中の矩形（texel）は部屋の周りに余白を含む（描き方が周りの texel を読むため）。
    struct Portal {
        const StageRoom* room = nullptr;
        int tier = 0;
        uint32_t x = 0, y = 0, width = 0, height = 0;
        uint32_t left = 0, top = 0;     // 余白（矩形の左上から部屋の左上までの texel）
        double tanX = 0, tanY = 0;      // 舞台で部屋を見る角度の正接（右・上）
        bool always = false;            // 毎フレーム描き直す（操作中・切り替え中の部屋）
        bool shown = false;             // 今の画に写っている（写っていなければ前の絵のまま残してよい）
    };
    // イベント ID で一度だけ出す。画面外で止めた後もイベントが消えるまで ID を保持する。
    struct ActiveFx { uint64_t eventId = 0; HIKARI::GameProjectVfxPlaybackHandle handle; };

    bool AcquireCameras(HIKARI::WorldView& world, const ScreenStyleSettings& stage);
    // 砂嵐の窓の透視カメラ。無ければ false（オープニングの砂嵐は出ない）。
    bool AcquireNoiseCamera(HIKARI::WorldView& world, const ScreenStyleSettings& stage);
    void RestoreCamera(HIKARI::WorldView& world, StageCamera& camera);
    // Play の覗き窓：写っている（画の縁の少し外も含む）テレビを目立つ順に選び、画面の大きさで細かさの段を決めて、
    // 段ごとのキャプチャに並べる。収まらなければ一つ粗い段へ回す。
    void PlacePortals(const std::vector<const StageRoom*>& rooms, const StageFrame& frame, double now);
    // 部屋の画面の 1 マスがゲームの画に写る画素（1080p 換算）。段の 1 マスの texel がこれを大きく下回ると拡大されてぼける。
    double CellPixels(const StageRoom& room) const;
    // オープニングの砂嵐：ノイズの板を撮る窓と、各テレビを砂嵐の段へ結び直す。
    void PlaceNoise();
    void ApplyIntro(HIKARI::WorldView& world, std::map<uint64_t, Binding>& next) const;
    // 編集中は全テレビを一枚の舞台像へ収める。
    void PlaceEditorWindow(const std::vector<const StageRoom*>& rooms);
    // 窓のキャプチャ（編集中の二枚と砂嵐）と、Play の段のキャプチャ（覗き窓の視点を並べる）を合わせ、画面を結び直す。
    bool SyncCaptures(HIKARI::WorldView& world, const std::vector<Assigned>& screens, double flash, double shakeX, double shakeY);
    // 覗き窓一つの視点（舞台の目・視錐台）。shakeX/Y は画面の揺れ（舞台の長さ）。
    HIKARI::GameProjectSceneCaptureViewDesc PortalView(const Portal& portal, double shakeX, double shakeY) const;
    // ゲームカメラから見た各部屋の画面（目立ち具合・向き）。Play 中は毎フレーム測り直す。
    void Inspect(HIKARI::WorldView& world, const std::vector<const StageRoom*>& rooms, const StageFrame& frame);
    // 読みやすさの保護を掛ける画面（エンジンの上限 12）。保護の有無で色調が変わるので、入れ替えは控えめにする。
    void ChooseProtected(std::map<uint64_t, Binding>& next) const;
    void SubmitParts(HIKARI::WorldView& world, const std::vector<const StageRoom*>& rooms, const std::vector<Assigned>& shown,
                     const Rules::WorldSnapshot& snapshot, int palette, bool smear, double now, bool animate);
    void PlayEffects(HIKARI::WorldView& world, const ScreenFxSettings& fx, bool available, double now);
    const Window& WindowOf(ViewKind kind) const { return windows_[static_cast<size_t>(kind)]; }

    std::array<StageCamera, kViewKinds> cameras_{};
    std::array<Window, kViewKinds> windows_{};
    // 窓の向こうの大広間：使うか、視差の強さ、部屋ごとのなめらかにした目の向きの正接（区域 ID → 右・上）。
    bool hall_ = true;
    // 奥行きの格子の列数（0 で無し）、透視の窓、受光（Play だけ）。
    int depthRows_ = 0;
    bool perspective_ = false, lit_ = false, solid_ = false;
    bool thinScreens_ = true;    // 遠い画面を間引いて描く（TD2.ScreenStyle の thinScreens）
    double actorScale_ = 1;
    double playDepth_ = 0;
    // 舞台の灯：写っている部屋へ役ごとに一つずつ置き、使わない灯は消す。灯の物体 → 点けているか。
    void SyncStageLights(HIKARI::WorldView& world, const std::vector<Assigned>& shown, double now);
    void ReleaseStageLights(HIKARI::WorldView& world);
    // 舞台灯ごと：今照らしている部屋（room）と次に照らす部屋（target）、明るさの重み、作者の強さ。
    struct StageLightTrack {
        HIKARI::GameProjectObjectHandle handle{};
        std::string room, target;
        double weight = 0;
        float base = 0, sent = -1;
        bool known = false, enabled = false, shadows = false, shadowOff = false;
    };
    std::map<uint64_t, StageLightTrack> stageLights_;
    double stageLightTime_ = 0;
    StageCamera noiseCamera_{};
    // Play の覗き窓と、テレビごとの前のフレームの段・最後に画に入った実時刻・最後の角度（区域 ID ごと）。
    bool play_ = false;   // Play の撮り方（覗き窓）か
    std::vector<Portal> portals_;
    ScreenStyleSettings portalStyle_;   // 覗き窓の段のキャプチャの描き方（操作中の部屋の描き方）
    std::map<std::string, int, std::less<>> portalTier_;
    std::map<std::string, double, std::less<>> portalSeen_;
    std::map<std::string, std::array<double, 2>, std::less<>> portalTan_;
    // 覗き窓の角度の上限（右・左・上の正接。プレイヤーの周りの地形で絞る）と、最後に追った時刻。
    std::map<std::string, std::array<double, 3>, std::less<>> portalLimit_;
    double portalLimitTime_ = -1;
    // 覗き窓の記録：部屋ごとの段と矩形と、覗き窓があった最後のフレーム（遠くの画面を間引いて描く時、前のフレームと同じ
    // 矩形に続けて置いた画面だけ前の絵を残す）。
    struct PortalDrawn { int tier = -1; uint32_t x = 0, y = 0, width = 0, height = 0; uint64_t frame = 0; };
    std::map<std::string, PortalDrawn, std::less<>> portalDrawn_;
    // 前のフレームの覗き窓の置き場所（段と段のキャプチャの中の矩形）。同じ段・同じ大きさならそのまま残す。
    struct PortalPlace { int tier = -1; uint32_t x = 0, y = 0, width = 0, height = 0; };
    std::map<std::string, PortalPlace, std::less<>> portalPlace_;
    uint64_t captureFrame_ = 0, portalRedrawn_ = 0, portalKept_ = 0;
    std::set<std::string, std::less<>> redrawnRooms_;   // このフレームに覗き窓を描き直す部屋
    // プレイヤーのいる部屋で目がプレイヤーの後を少し追う分（正接。部屋を出ると 0 へなめらかに戻る）。
    std::map<std::string, std::array<double, 2>, std::less<>> follow_;
    // このフレームのオープニング（無ければ null）と、砂嵐の窓を使っているか。砂嵐を出す区域のテレビは全部の部屋から選ぶ。
    const IntroScreens* intro_ = nullptr;
    bool introStatic_ = false;
    // オープニングで主人公を隠すか、現れる途中の伸び（0〜1）、登場のエフェクトを足したか。
    bool heroShown_ = true, introArrived_ = false;
    double heroGrow_ = 1, arrivePlayedAt_ = -1;   // 登場のエフェクトが出たオープニングの時刻（まだなら -1）
    std::vector<const StageRoom*> introRooms_;
    // テレビごとの砂嵐の始まりとパソコンからの遠さ（距離で決め、オープニングの間は変えない）。
    mutable std::map<uint64_t, std::array<double, 2>> introOnset_;
    double parallaxGain_ = 1.0, lastParallaxTime_ = -1;
    std::map<std::string, std::array<double, 2>, std::less<>> parallax_;
    std::map<std::string, WindowSpan, std::less<>> windowSpans_;   // 区域 ID → 窓の上下（操作の札を避けた位置）
    std::array<std::string, 2> travel_{};          // 切り替え中の二部屋（無ければ空）
    StageProjection projection_;
    std::vector<View> views_;
    std::map<uint64_t, Binding> bound_;            // 画面の物体 → 今の割り当て
    std::map<std::string, ScreenOnCamera, std::less<>> seen_;   // このフレームの見え方（区域 ID → 値。ゲームカメラが無い時は空）
    std::map<std::string, RoomParts> roomParts_;   // 区域 ID → 背景と地形の描画要求
    uint64_t terrainKey_ = 0;                       // Play で前のフレームに地形を確かめた時の入力の指紋（0 は未確認）
    // 180° 回されている部屋の世界の範囲（左・上・右・下）。エフェクトを点対称に置くのに使う。
    std::vector<std::pair<std::string, std::array<double, 4>>> flippedRooms_;
    std::vector<PartRequest> frameParts_;          // このフレームの装置・人物・演出（毎フレーム作る）
    std::vector<PartRequest> introPlanes_;         // オープニングの砂嵐・タイトルの板（終わった後も退けて出し続ける）
    StagePuppets puppets_;                         // Play で人物を演じる骨つきの人形
    StageDevices devices_;                         // Play で装置を演じる骨つきの物体
    // 装置ごとの通電・実体と、それが最後に変わった実時刻（扉・橋の出入りと通電灯の点灯を動かす）。
    struct DeviceMemory { bool powered = false, solid = false, ready = false, checkpoint = false; double changed = -1e9; };
    std::map<std::string, DeviceMemory> deviceMemory_;
    std::vector<std::pair<uint64_t, uint64_t>> submitted_;
    size_t accepted_ = 0, failed_ = 0;
    ScreenEffects worldEffects_;
    std::vector<ActiveFx> activeFx_;
    size_t fxRejected_ = 0;
    // 演出：最後に起きた揺れ・閃光の強さと開始時刻（実時間）。減衰は Render で計算する。
    double shakeAmplitude_ = 0, shakeStart_ = -1, flashAmount_ = 0, flashStart_ = -1;
    std::string status_ = "3D の舞台カメラとモデルを待っている";
    size_t frame_ = 0;   // Play のフレーム数（オープニングの砂嵐の乱れに使う）
};
}
