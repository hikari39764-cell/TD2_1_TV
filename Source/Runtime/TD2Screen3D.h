#pragma once
#include "../Common/TD2RegionData.h"
#include "Rules/TD2Rules.h"
#include "TD2ScreenEffects.h"
#include "TD2ScreenFx.h"
#include "TD2ScreenModels.h"
#include "TD2ScreenStyle.h"
#include "TD2StagePuppet.h"
#include "TD2Intro.h"
#include "HIKARI/Project/SceneCaptureView.h"
#include "HIKARI/Project/WorldView.h"
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace TD2 {
struct ScreenConfiguration {
    uint64_t screenObject = 0;
    std::string screenSlot = "TD2_Screen";
    int outputWidth = 256, outputHeight = 192;
    float brightness = 2.2f;
    int materialIndex = -1;
    std::string modelAssetGuid;
};
// 舞台に並べる一部屋。x, y は舞台上のマス位置（世界に置いた部屋は世界の位置、未配置の地図は世界の外に並べた位置）。
// region は描く間だけ借りる（Play は Session の地図、編集中は ownedRegion へ写した値）。
// テレビのモデルのローカル空間での画面。カメラに向いた画面の判定に使う。
struct ScreenSurface { float centerX = 0, centerY = .4f, centerZ = .28f, width = .84f, height = .49f; };
// 画面がゲームカメラの画にどう写っているか。coverage は画の中に入った面積の割合（裏向きは 0）、depth は画面の中心
// までの奥行き、centerX/Y は中心の画の位置（-1〜1）。鮮明に撮る部屋・保護・焦点の選択に使う。
// eyeX/Y は画面の右・上に対する目の向きの正接（画面の中の奥の層を見る角度）。
struct ScreenOnCamera { double coverage = 0, depth = 0, centerX = 0, centerY = 0, facing = 0, eyeX = 0, eyeY = 0; };
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
    // 大区域の切り替え中なら、出る部屋と入る部屋。鮮明な窓・保護・焦点をこの二部屋に絞り、
    // 画で目立つ画面が替わるたびに切り替わらないようにする。
    std::string travelFrom, travelTo;
    // 操作中の部屋と同じ大区域の部屋。周りの窓はこの部屋たちを覆う位置に固定する（部屋を移るたびに窓を動かさない）。
    std::vector<std::string> areaRooms;
    // オープニングの演出（タイトルと砂嵐）。無ければ普段どおり。
    const IntroScreens* intro = nullptr;
};

// 編集中の並べ直し：全部の部屋を一枚ずつの窓に収めるため、描く位置（x, y）だけを二つの組に詰めて並べる。
// 世界の並びのまま写すと、大きな地図（500 マス四方など）では 1 マス 4 texel ほどになり画面がひどくぼやける。
// 呼ぶ前に linkX, linkY へ世界の位置を入れておく。
void PackRoomsForEditor(std::vector<StageRoom>& rooms);

// 一つの World に一つの連続した舞台。世界地図どおりに部屋を置き、プレイヤー・光線・エフェクトは世界に一つずつ。
// 舞台カメラ（鮮明・周り・未配置）と描き方の組ごとにキャプチャを一つ作り、各テレビは自分の部屋の範囲を切り出して映す。
class Screen3DStage {
public:
    // stage は舞台カメラに付けた共有設定。配色・カメラの角度・舞台カメラの指定はここだけが効く。
    // fx はイベントごとのエフェクト（VFX 資産）。舞台の上で出し、キャプチャがそのまま写す。
    void Render(HIKARI::WorldView& world, const std::vector<StageRoom>& rooms, const StageFrame& frame,
                const ScreenStyleSettings& stage, const ScreenFxSettings& fx);
    bool Release(HIKARI::WorldView& world);
    const std::string& Status() const noexcept { return status_; }

private:
    // 舞台を撮る窓の種類。Play ではどれも透視の窓で、外のカメラが画面を見る角度へ目を寄せる（テレビが窓を移っても角度と
    // 細かさが跳ばない）：鮮明は操作中の部屋、大区域は同じ镜头大区域と世界地図で近い残りの部屋の組、遠く・遠く2 は背景に
    // 写るほかのテレビを地図で近いものどうしに分けた二組。編集中は鮮明と大区域の二枚を正投影で撮る（詰めて並べた地図の二組）。
    enum class ViewKind : int { Sharp = 0, Area = 1, Far = 2, Far2 = 3 };
    static constexpr size_t kViewKinds = 4;
    // 正投影カメラを持つ窓の種類（編集中だけ使う）。透視カメラは窓の種類ごとに一台（番号は窓の種類と同じ）。
    static constexpr size_t kOrthoKinds = 2;
    static constexpr std::array<ViewKind, 2> kFarKinds{ViewKind::Far, ViewKind::Far2};
    // 窓は世界のマスの範囲（行数は正投影の高さで決まる）と、キャプチャの大きさ（texel）。
    struct Window {
        double x = 0, y = 0, cols = 0, rows = 0;
        int density = 16;
        uint32_t width = 0, height = 0;
        bool valid = false;
        std::string regionId;   // 一部屋だけを撮る窓（Play の鮮明）の部屋
        // 透視の窓：使う透視カメラ（-1 なら正投影）と、目から正面の面までの距離（舞台の長さ）。
        int lens = -1;
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
    struct View {
        ViewKind kind = ViewKind::Sharp;
        ScreenStyleSettings style;
        HIKARI::GameProjectSceneCaptureHandle handle{};
        HIKARI::GameProjectObjectHandle camera{};
        uint32_t width = 0, height = 0;
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
    };
    struct Assigned { const StageRoom* room = nullptr; ViewKind kind = ViewKind::Sharp; };
    // イベント ID で一度だけ出す。画面外で止めた後もイベントが消えるまで ID を保持する。
    struct ActiveFx { uint64_t eventId = 0; HIKARI::GameProjectVfxPlaybackHandle handle; };

    bool AcquireCameras(HIKARI::WorldView& world, const ScreenStyleSettings& stage);
    // 鮮明と大区域の透視カメラ。鮮明が使えなければ false（透視を使わない）。
    bool AcquireLensCameras(HIKARI::WorldView& world, const ScreenStyleSettings& stage);
    // 遠くの二組の透視カメラ。一台目が無ければ false（遠くのテレビは映らない）。
    bool AcquireFarCameras(HIKARI::WorldView& world, const ScreenStyleSettings& stage);
    void RestoreCamera(HIKARI::WorldView& world, StageCamera& camera);
    // Play の鮮明と大区域の窓。
    void PlaceWindows(const std::vector<const StageRoom*>& rooms, const StageFrame& frame, int maxCols);
    // 大区域の窓：細かさ固定の透視の窓。目（窓の中心）は操作中の部屋と接する部屋・目立つ部屋の組へなめらかに動き、
    // 窓にまるごと写る部屋を撮る。
    void PlaceArea(const std::vector<const StageRoom*>& rooms, const StageRoom* active, const StageFrame& frame);
    // 遠くの窓：鮮明・大区域に入らず画に写っているテレビ（目立つ順に kFarRooms 台まで）を、世界地図で近いものどうしの
    // 二組に分けて収める。
    void PlaceFar(const std::vector<const StageRoom*>& rooms);
    // 部屋の画面の 1 マスがゲームの画に写る画素（1080p 換算）。窓の 1 マスの texel がこれを下回ると拡大されてぼける。
    double CellPixels(const StageRoom& room) const;
    // 部屋の組を一枚の透視の窓に収める。窓の大きさはキャプチャの大きさ（固定）と組の外接で決まり、目は組の部屋を
    // 外のカメラが見る角度に合わせる。
    void PlaceGroup(ViewKind kind, const std::vector<const StageRoom*>& members, uint32_t width, uint32_t height);
    // 透視の窓の目の位置（外接 box の中心からのずれ。マス）：部屋ごとに外のカメラと同じ角度でその部屋を見る目を、
    // 写っている割合で平均する。
    std::array<double, 2> EyeOffset(const Window& window, const std::array<double, 4>& box,
                                    const std::vector<const StageRoom*>& members) const;
    // 余白を取った窓の中心（目の真正面）を目の位置へずらし、切り出しの位置をゲーム画素の格子に合わせて決める。
    void AimWindow(Window& window, const std::array<double, 4>& box, std::array<double, 2> offset) const;
    // オープニングの砂嵐：ノイズの板を撮る窓（遠くの窓の透視カメラを借りる）と、各テレビを砂嵐の段へ結び直す。
    void PlaceNoise();
    void ApplyIntro(HIKARI::WorldView& world, std::map<uint64_t, Binding>& next) const;
    // 編集中は全テレビを一枚の舞台像へ収める。
    void PlaceEditorWindow(const std::vector<const StageRoom*>& rooms);
    bool SyncCaptures(HIKARI::WorldView& world, const std::vector<Assigned>& screens, double flash);
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
    std::array<StageCamera, kViewKinds> lensCameras_{};
    // 組の窓（大区域・遠く）に入れる部屋（区域 ID）。
    std::array<std::vector<std::string>, kViewKinds> members_;
    // 遠くの組の目（地図のマス）、その行き先と、行き先を決めた時の外接。
    std::array<std::array<double, 2>, kViewKinds> groupEye_{};
    std::array<std::array<double, 2>, kViewKinds> groupTarget_{};
    std::array<std::array<double, 4>, kViewKinds> groupEyeBox_{};
    std::array<bool, kViewKinds> groupEyeValid_{};
    // 今の大区域で写ったテレビ、入れた遠くの組（大区域が替わるまで外さない）、要る細かさ（ゆっくり下げる）。
    std::set<std::string, std::less<>> farSeen_;
    std::map<std::string, double, std::less<>> farNeed_;
    std::map<std::string, size_t, std::less<>> farGroup_;
    std::string groupArea_;                       // 組の窓を決めた大区域（その大区域の最初の部屋の ID）
    // 大区域の窓に入れている部屋と、窓の中心に置く目（地図のマス）。視差と同じ速さ（parallaxFollow_）でなめらかに追う。
    std::vector<std::string> areaMembers_;
    std::array<double, 2> areaEye_{};
    bool areaEyeValid_ = false;
    double parallaxFollow_ = 1;
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
    // 180° 回されている部屋の世界の範囲（左・上・右・下）。エフェクトを点対称に置くのに使う。
    std::vector<std::pair<std::string, std::array<double, 4>>> flippedRooms_;
    std::vector<PartRequest> frameParts_;          // このフレームの装置・人物・演出（毎フレーム作る）
    StagePuppets puppets_;                         // Play で人物を演じる骨つきの人形
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
    std::string status_ = "等待3D舞台镜头与模型";
    // 診断用（環境変数 TD2_SCREEN_TRACE がある時だけ）：窓・保護・割り当てが変わったフレームを記録する。
    void TraceFrame();
    size_t traceFrame_ = 0; std::string traceLast_; std::map<uint64_t, HIKARI::GameProjectSceneCaptureSourceRect> traceRects_;
    size_t frame_ = 0;   // Play のフレーム数（診断の記録用）
    std::map<std::string, bool, std::less<>> traceProtect_;   // 診断用：前のフレームの保護
};
}
