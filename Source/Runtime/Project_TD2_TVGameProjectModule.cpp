#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/GameProjectModule.h"
#include "TD2RegionComponent.h"
#include "TD2Mechanism.h"
#include "TD2SignalCable.h"
#include "TD2CameraRig.h"
#include "TD2GameSession.h"
#include "TD2LightBudget.h"
#include "TD2ScreenPresenter.h"
#include "TD2TravelComponents.h"
#include "TD2ScreenFx.h"
#include "TD2ScreenStyle.h"
#include "TD2ScreenTone.h"
#include "TD2Sound.h"
#include "TD2StageLight.h"
#include "TD2StageActor.h"
#include "TD2DeviceActor.h"
#include "TD2TvPopups.h"
#include "TD2Intro.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>

namespace Project_TD2_TV {

// システムは通常どおり World に Attach され、ゲーム状態は Play session から取得する。
class Game final : public HIKARI::IGameSystem {
public:
    void Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame) override {
        auto* session = dynamic_cast<TD2::GameSession*>(world.GetGameSession());
        if (session == nullptr) { return; }
        const auto started = std::chrono::steady_clock::now();
        session->Update(world, frame);
        TD2::GameUpdateMilliseconds() += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
};

// 以下はエンジンへの登録。日常のゲーム処理は上の Game に書く。
// Register で登録したコンポーネントとシステムは自動的にこの Feature の所有になる。
// テレビの区域はモデルと一緒に使う。付けると ModelComponent も付き、区域がある間は外せない。
inline constexpr std::string_view kTelevisionNeeds[]{"ModelComponent"};

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
        // Region があるシーンには表示システムを自動で入れる。
        return context.RegisterSystem<Game>({.systemId = "Project_TD2_TV.Game", .displayName = "Game"}) &&
            context.RegisterComponent<TD2::RegionComponent>({.typeName = TD2::kRegionComponentType, .requiredComponents = kTelevisionNeeds,
                .displayName = "TD2 テレビの区域", .category = "TD2"}) &&
            context.RegisterComponent<TD2::RegionSequenceComponent>({.typeName = TD2::kRegionSequenceComponentType, .displayName = "TD2 区域の演出", .category = "TD2"}) &&
            context.RegisterComponent<TD2::MechanismComponent>({.typeName = TD2::kMechanismComponentType, .displayName = "TD2 画面外の仕掛け", .category = "TD2"}) &&
            context.RegisterComponent<TD2::SlideRackComponent>({.typeName = TD2::kSlideRackComponentType, .displayName = "TD2 スライド棚", .category = "TD2"}) &&
            context.RegisterComponent<TD2::StackColumnComponent>({.typeName = TD2::kStackColumnComponentType, .displayName = "TD2 テレビの塔", .category = "TD2"}) &&
            context.RegisterComponent<TD2::CarouselComponent>({.typeName = TD2::kCarouselComponentType, .displayName = "TD2 回転柱", .category = "TD2"}) &&
            context.RegisterComponent<TD2::SignalCableComponent>({.typeName = TD2::kSignalCableComponentType, .displayName = "TD2 信号線", .category = "TD2"}) &&
            context.RegisterComponent<TD2::CameraAreaComponent>({.typeName = TD2::kCameraAreaComponentType, .displayName = "TD2 カメラの大区域", .category = "TD2"}) &&
            context.RegisterComponent<TD2::ScreenStyleComponent>({.typeName = TD2::kScreenStyleComponentType, .displayName = "TD2 画面の描き方", .category = "TD2"}) &&
            context.RegisterComponent<TD2::ScreenToneComponent>({.typeName = TD2::kScreenToneComponentType, .displayName = "TD2 画面の色調", .category = "TD2"}) &&
            context.RegisterComponent<TD2::SoundComponent>({.typeName = TD2::kSoundComponentType, .displayName = "TD2 音", .category = "TD2"}) &&
            context.RegisterComponent<TD2::StageLightComponent>({.typeName = TD2::kStageLightComponentType, .displayName = "TD2 舞台の灯", .category = "TD2"}) &&
            context.RegisterComponent<TD2::StageActorComponent>({.typeName = TD2::kStageActorComponentType, .displayName = "TD2 舞台の人形", .category = "TD2"}) &&
            context.RegisterComponent<TD2::DeviceActorComponent>({.typeName = TD2::kDeviceActorComponentType, .displayName = "TD2 舞台の装置", .category = "TD2"}) &&
            context.RegisterComponent<TD2::PopupBoardComponent>({.typeName = TD2::kPopupBoardComponentType, .displayName = "TD2 弾窓の板", .category = "TD2"}) &&
            context.RegisterComponent<TD2::PopupScriptComponent>({.typeName = TD2::kPopupScriptComponentType, .displayName = "TD2 弾窓の台本", .category = "TD2"}) &&
            context.RegisterComponent<TD2::IntroComponent>({.typeName = TD2::kIntroComponentType, .displayName = "TD2 オープニング", .category = "TD2"}) &&
            context.RegisterComponent<TD2::ScreenFxComponent>({.typeName = TD2::kScreenFxComponentType, .displayName = "TD2 画面のエフェクト", .category = "TD2"}) &&
            context.RegisterSystem<TD2::ScreenPresentationSystem>({.systemId = TD2::kScreenSystemId, .displayName = "TD2 画面の表示"}) &&
            context.RegisterComponent<TD2::LightCullComponent>({.typeName = TD2::kLightCullComponentType, .displayName = "TD2 灯の節約", .category = "TD2"}) &&
            context.RegisterSystem<TD2::LightBudgetSystem>({.systemId = TD2::kLightBudgetSystemId, .displayName = "TD2 灯の節約"}) &&
            context.BindComponentSystem({TD2::kLightCullComponentType, TD2::kLightBudgetSystemId, 120}) &&
            context.BindComponentSystem({TD2::kRegionComponentType, "Project_TD2_TV.Game", 100}) &&
            context.BindComponentSystem({TD2::kRegionComponentType, TD2::kScreenSystemId, 110}) &&
            context.BindComponentSystem({TD2::kScreenStyleComponentType, TD2::kScreenSystemId, 110}) &&
            context.BindComponentSystem({TD2::kScreenFxComponentType, TD2::kScreenSystemId, 110});
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
}

namespace HIKARI::RUNTIME {
std::unique_ptr<IGameProjectModule> CreateGameProjectModule() {
    return std::make_unique<::Project_TD2_TV::GameProjectModule>();
}
}
