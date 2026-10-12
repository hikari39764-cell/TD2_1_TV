#pragma once

#include "../Common/TD2RegionData.h"
#include "Rules/TD2Rules.h"
#include "HIKARI/Project/GameSystem.h"
#include "HIKARI/Project/UiCanvasTypes.h"
#include "HIKARI/Project/UiCanvasView.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TD2 {

// シーン内参照を使い、モデルの名前検索やユーザーの選択変更を必要としない。
struct ScreenConfiguration {
    uint64_t screenObject = 0;
    std::string screenSlot = "TD2_Screen";
    // 旧設定との読み込み互換性を保つ。実際の表示は各 PNG フレームを直接参照する。
    std::string texturePath = "generated://solid/ffffffff";
    int outputWidth = 256;
    int outputHeight = 192;
    // 対象モデルとスロットが一致した時だけ、Canvas の実行時材質へ有限の輝度を適用する。
    float brightness = 2.2f;
    int materialIndex = -1;
    std::string modelAssetGuid;
};

// WorldSnapshot の状態変化から作る、一時的で判定に影響しない表示情報。
struct ScreenPresentationFeedback {
    std::string family;
    Rules::Rect rect;
    double startedAt = 0.0;
    double duration = 0.0;
    bool reversed = false;
};

// dash 残像の対象を、実際に観測した描画位置だけから選ぶ履歴。
struct ScreenPresentationMotionSample {
    double realTime = 0.0;
    Rules::Rect rect;
    int facing = 1;
};

// World ごとの非保存リソース。作者データの編集やゲームの進行は担当しない。
class ScreenPresenter final {
public:
    void Refresh(HIKARI::WorldView& world, const Rules::WorldDefinition& definition,
                 const ScreenConfiguration& configuration, const Rules::WorldSnapshot* snapshot = nullptr,
                 bool showPlayer = true);
    bool Release(HIKARI::WorldView& world);
    const std::string& Status() const noexcept { return status_; }

private:
    bool Cancel(HIKARI::UiCanvasView canvases, std::string_view reason);
    void UpdateFeedback(const Rules::WorldSnapshot& snapshot);

    HIKARI::GameProjectUiCanvasHandle canvas_{};
    HIKARI::GameProjectObjectHandle boundObject_{};
    std::string boundSlot_{};
    uint32_t outputWidth_ = 0;
    uint32_t outputHeight_ = 0;
    uint64_t contentHash_ = 0;
    uint64_t committedRevision_ = 0;
    bool surfaceBound_ = false;
    std::optional<Rules::WorldSnapshot> previousSnapshot_;
    std::optional<double> pendingEchoStartedAt_;
    std::vector<ScreenPresentationFeedback> feedback_;
    std::vector<ScreenPresentationMotionSample> motionSamples_;
    std::string status_ = "尚未执行呈现；需要已加载模块、区域组件与屏幕对象";
};

// 編集中にも表示を更新する。Play 専用の GameSession や dt には依存させない。
class ScreenPresentationSystem final : public HIKARI::IGameSystem {
public:
    void Update(HIKARI::WorldView& world, const HIKARI::FrameContext&) override;
    void DescribeSettings(HIKARI::PropertyFields& fields) override { fields.Note(queryStatus_); }

private:
    std::string queryStatus_ = "尚未执行区域组件查询";
};

} // namespace TD2
