#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/GameProjectModule.h"
#include "TD2RegionComponent.h"
#include "TD2GameSession.h"
#include "TD2TravelComponents.h"
#include <array>
#include <cstdint>
#include <memory>

namespace Project_TD2_TV {

// システムは通常どおり World に Attach され、ゲーム状態は Play session から取得する。
class Game final : public HIKARI::IGameSystem {
public:
    void Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame) override {
        auto* session = dynamic_cast<TD2::GameSession*>(world.GetGameSession());
        if (session != nullptr) { session->Update(world, frame); }
    }
};

// 以下はエンジンへの登録。日常のゲーム処理は上の Game に書く。
// Register で登録したコンポーネントとシステムは自動的にこの Feature の所有になる。
class GameplayFeature final : public HIKARI::IRuntimeFeature {
public:
    std::string_view GetFeatureId() const noexcept override { return "Project_TD2_TV.Gameplay"; }
    std::string_view GetDisplayName() const noexcept override { return "Gameplay"; }
    std::string_view GetDescription() const noexcept override { return "Project game logic"; }
    std::span<const HIKARI::RuntimeSystemDefault> GetDefaultSystems() const noexcept override {
        static constexpr std::array<HIKARI::RuntimeSystemDefault, 1> systems{{{"Project_TD2_TV.Game", true, 100}}};
        return systems;
    }
    bool Register(HIKARI::RuntimeFeatureContext& context) override {
        // 既存シーンでも Region の存在に応じて表示システムを導入する。ゲーム進行とは分離する。
        return context.RegisterSystem<Game>({.systemId = "Project_TD2_TV.Game", .displayName = "Game"}) &&
            context.RegisterComponent<TD2::RegionComponent>({
                .typeName = TD2::kRegionComponentType,
                .displayName = "TD2 电视区块",
                .category = "TD2",
                .description = "电视网格作者数据与屏幕引用",
            }) &&
            context.RegisterComponent<TD2::PassageComponent>({
                .typeName = TD2::kPassageComponentType, .displayName = "TD2 旧通路（停用）", .category = "TD2 Legacy",
                .description = "仅保留旧数据，不参与连续世界",
            }) &&
            context.RegisterComponent<TD2::CameraPerformanceComponent>({
                .typeName = TD2::kCameraPerformanceComponentType, .displayName = "TD2 旧镜头路径（停用）", .category = "TD2 Legacy",
                .description = "仅保留旧数据，不参与 Sequence 播放",
            }) &&
            context.RegisterComponent<TD2::RegionSequenceComponent>({
                .typeName = TD2::kRegionSequenceComponentType, .displayName = "TD2 区域演出", .category = "TD2",
                .description = "相邻电视的可选引擎 Sequence 绑定",
            }) &&
            context.RegisterSystem<TD2::ScreenPresentationSystem>({
                .systemId = TD2::kScreenSystemId,
                .displayName = "TD2 屏幕呈现",
            }) &&
            context.BindComponentSystem({TD2::kRegionComponentType, "Project_TD2_TV.Game", 100}) &&
            context.BindComponentSystem({TD2::kRegionComponentType, TD2::kScreenSystemId, 110});
    }
};

class GameProjectModule final : public HIKARI::RUNTIME::IGameProjectModule {
public:
    std::string_view GetModuleId() const noexcept override { return "Project_TD2_TV"; }
    bool RegisterRuntimeFeatures(HIKARI::RuntimeFeatureCatalog& catalog) override {
        return HIKARI::RegisterRuntimeFeature<GameplayFeature>(catalog);
    }
    std::unique_ptr<HIKARI::IGameProjectSession> CreateGameSession() override {
        return std::make_unique<TD2::GameSession>();
    }
};
} // namespace

namespace HIKARI::RUNTIME {
std::unique_ptr<IGameProjectModule> CreateGameProjectModule() {
    return std::make_unique<::Project_TD2_TV::GameProjectModule>();
}
} // namespace HIKARI::RUNTIME
