#pragma once
#include "../Common/TD2RegionData.h"
#include "Rules/TD2Rules.h"
#include "TD2ScreenEffects.h"
#include "TD2ScreenFx.h"
#include "TD2ScreenModels.h"
#include "TD2ScreenStyle.h"
#include "HIKARI/Project/SceneCaptureView.h"
#include "HIKARI/Project/WorldView.h"
#include <array>
#include <map>
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
// 舞台に並べる一部屋。x, y は舞台上の格位置：世界に置いた部屋は世界の位置、置いていない作者地図（編集中だけ映す）は
// 呼び手が世界の外に並べた位置。region は描く間だけ借りる（Play は Session の地図、編集中は ownedRegion へ写した値）。
// 電視の模型の局所空間での画面（模型の目録の値）。可読性の保護を、镜头に向いて遮られていない画面だけに掛けるのに使う。
struct ScreenSurface { float centerX = 0, centerY = .4f, centerZ = .28f, width = .84f, height = .49f; };
// 画面が游戏镜头の画にどう写っているか。coverage は画の外へ出た分を除いた面積の割合（裏向きは 0）、depth は画面の中心
// までの奥行き、centerX/Y は中心の画の位置（-1〜1）。演出や追従の途中でも「実際に目立っている画面」を選ぶのに使う
// （清晰に撮る部屋・可読性の保護・焦点）。
struct ScreenOnCamera { double coverage = 0, depth = 0, centerX = 0, centerY = 0, facing = 0; };
ScreenOnCamera ProjectScreen(const HIKARI::GameProjectCameraPose& pose, const HIKARI::GameProjectTransform& tv, const ScreenSurface& surface);
struct StageRoom {
    HIKARI::GameProjectObjectHandle owner;
    std::string regionId;
    int x = 0, y = 0;
    bool placed = true;
    const RegionDefinition* region = nullptr;
    RegionDefinition ownedRegion;
    ScreenConfiguration configuration;
    ScreenSurface surface;
    ScreenStyleSettings style;
    const RegionDefinition& Region() const { return region ? *region : ownedRegion; }
};
// 舞台を動かす世界座標の状態。装置の ID は「区域ID/装置ID」。activeRegion は清晰に撮る操作中の部屋。
// simulated は Play の snapshot（光線・残影・事件の特効がある）。編集中は呼び手が作者の装置と出生位置から作る。
struct StageFrame {
    const Rules::WorldSnapshot* snapshot = nullptr;
    std::string activeRegion;
    bool simulated = false;
    bool authoring = false;
    // 大区域どうしの切り替え（演出）の途中なら、出る部屋と入る部屋。镜头が大きく動く間は、清晰の窓・保護・焦点を
    // この二部屋に絞り、画で目立つ画面が替わるたびに切り替わらないようにする。
    std::string travelFrom, travelTo;
};

// 一つの World に一つの連続した舞台。世界地図の並びのまま部屋を置き、主角・光線・特効は世界に一つずつ。
// 舞台カメラ（清晰・周り・未配置）と描き方の組ごとに捕捉を一つ作り、各電視は自分の部屋の範囲を切り出して映す。
class Screen3DStage {
public:
    // stage は共有舞台（舞台カメラに付けた）設定。配色・カメラの角度・舞台カメラの指定はここだけが効く。
    // fx は事件ごとの特効（VFX 資産）。舞台の上で鳴らし、捕捉が VFX ごと写す。
    void Render(HIKARI::WorldView& world, const std::vector<StageRoom>& rooms, const StageFrame& frame,
                const ScreenStyleSettings& stage, const ScreenFxSettings& fx);
    bool Release(HIKARI::WorldView& world);
    const std::string& Status() const noexcept { return status_; }

private:
    // 舞台を撮る窓の種類。清晰は操作中の部屋（1 格 32 texel）、周りは操作中の部屋の周り一部屋分（3×3、16 texel）、
    // 未配置は編集中だけ世界の外に並べた作者地図。
    enum class ViewKind : int { Sharp = 0, Wide = 1, Unplaced = 2 };
    static constexpr size_t kViewKinds = 3;
    // 窓は世界の格の範囲（行数は舞台カメラの正交の高さが決める）と、捕捉の大きさ（texel）。
    struct Window {
        double x = 0, y = 0, cols = 0, rows = 0;
        int density = 16;
        uint32_t width = 0, height = 0;
        bool valid = false;
        std::string regionId;   // 一部屋だけを撮る窓（Play の清晰）の部屋
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
        bool protect = false;   // 後処理から画面を守る（镜头に向き、遮られず、目立つ上位 12 画面まで）
        std::string regionId;
    };
    // 部屋の背景と地形の提出要求。部屋と周り 1 格の占有・置き場所・舞台の写し方・配色が同じ間は作り直さない。
    struct RoomParts {
        std::vector<char> occupancy;
        int x = 0, y = 0, palette = -1;
        double scale = 0, pitch = 0, yaw = 0;
        std::vector<PartRequest> parts;
    };
    struct Assigned { const StageRoom* room = nullptr; ViewKind kind = ViewKind::Sharp; };
    // 事件 ID で一度だけ鳴らす。画面外で止めた後も事件が消えるまで ID を保持する。
    struct ActiveFx { uint64_t eventId = 0; HIKARI::GameProjectVfxPlaybackHandle handle; };

    bool AcquireCameras(HIKARI::WorldView& world, const ScreenStyleSettings& stage);
    void RestoreCamera(HIKARI::WorldView& world, StageCamera& camera);
    void PlaceWindows(const std::vector<const StageRoom*>& rooms, const StageFrame& frame, int maxCols);
    // 編集中は全テレビを一枚の有限サイズの舞台像へ収める。Play の近傍窓とは分離する。
    void PlaceEditorWindow(const std::vector<const StageRoom*>& rooms);
    bool SyncCaptures(HIKARI::WorldView& world, const std::vector<Assigned>& screens, int palette, double flash);
    // 游戏镜头から見た各部屋の画面（目立ち具合・向き・他の電視に遮られた標本の数）。Play の間、毎フレーム測り直す。
    struct Seen { ScreenOnCamera view; int blocked = 0; };
    void Inspect(HIKARI::WorldView& world, const std::vector<const StageRoom*>& rooms, const StageFrame& frame);
    // Play で清晰に撮る二部屋を目立つ順に選ぶ。今の二部屋は、他の部屋が十分に大きく写るまで入れ替えない
    // （部屋ごとの解像度が行き来しないように）。
    void ChooseFocus(const StageFrame& frame);
    // 可読性の保護を掛ける画面（引擎の上限 12）。条件を外れるか、より大きい画面に大差を付けられるまで保つ
    // （保護の有無で画面の色調が変わるので、入れ替えは点滅として見える）。
    void ChooseProtected(std::map<uint64_t, Binding>& next) const;
    void SubmitParts(HIKARI::WorldView& world, const std::vector<const StageRoom*>& rooms, const std::vector<Assigned>& shown,
                     const Rules::WorldSnapshot& snapshot, int palette, bool smear, double now, bool animate);
    void PlayEffects(HIKARI::WorldView& world, const ScreenFxSettings& fx, bool available, double now);
    const Window& WindowOf(ViewKind kind) const { return windows_[static_cast<size_t>(kind)]; }

    std::array<StageCamera, kViewKinds> cameras_{};
    std::array<Window, kViewKinds> windows_{};
    std::array<std::string, 2> travel_{};          // 切り替えの演出の途中の二部屋（無ければ空）
    StageProjection projection_;
    std::vector<View> views_;
    std::map<uint64_t, Binding> bound_;            // 画面の物体 → 今の結び
    std::map<std::string, Seen, std::less<>> seen_;   // このフレームの見え方（区域 ID → 値。游戏镜头が無い時は空）
    std::array<std::string, 2> focus_{};          // Play で清晰に撮る二部屋（区域 ID）
    std::string wideCenter_;                      // 周りの窓の中心の部屋（区域 ID）
    std::map<std::string, RoomParts> roomParts_;   // 区域 ID → 背景と地形の提出要求
    std::vector<PartRequest> frameParts_;          // このフレームの装置・人物・演出（毎フレーム作る）
    // 装置ごとの通電・実体と、それが最後に変わった実時刻（扉・橋の出入りと通電灯の光の輪を動かす）。
    struct DeviceMemory { bool powered = false, solid = false, ready = false; double changed = -1e9; };
    std::map<std::string, DeviceMemory> deviceMemory_;
    std::vector<std::pair<uint64_t, uint64_t>> submitted_;
    size_t accepted_ = 0, failed_ = 0;
    ScreenEffects worldEffects_;
    std::vector<ActiveFx> activeFx_;
    size_t fxRejected_ = 0;
    // 演出：最後に起きた揺れ・閃光の強さと開始時刻（実時間）。減衰は Render で計算する。
    double shakeAmplitude_ = 0, shakeStart_ = -1, flashAmount_ = 0, flashStart_ = -1;
    std::string status_ = "等待3D舞台镜头与模型";
    // 診断用（環境変数 TD2_SCREEN_TRACE がある時だけ）：窓・保護・結びが変わったフレームを記録する。
    void TraceFrame();
    size_t traceFrame_ = 0; std::string traceLast_; std::map<uint64_t, HIKARI::GameProjectSceneCaptureSourceRect> traceRects_;
    size_t frame_ = 0;   // Play のフレーム数（診断の記録用）
    std::map<std::string, bool, std::less<>> traceProtect_;   // 診断用：前のフレームの保護
};
} // namespace TD2
