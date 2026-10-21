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
#include "TD2StageLight.h"
#include "TD2StageActor.h"
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
            context.RegisterComponent<TD2::RegionComponent>({
                .typeName = TD2::kRegionComponentType,
                .displayName = "TD2 电视区块",
                .category = "TD2",
                .description = "电视网格作者数据与屏幕引用",
            }) &&
            context.RegisterComponent<TD2::RegionSequenceComponent>({
                .typeName = TD2::kRegionSequenceComponentType, .displayName = "TD2 区域演出", .category = "TD2",
                .description = "相邻电视的可选引擎 Sequence 绑定",
            }) &&
            context.RegisterComponent<TD2::MechanismComponent>({
                .typeName = TD2::kMechanismComponentType, .displayName = "TD2 屏外机构", .category = "TD2",
                .description = "被压板驱动的转台、吊台等：在两个状态之间移动物体与区域在世界地图上的位置",
            }) &&
            context.RegisterComponent<TD2::SlideRackComponent>({
                .typeName = TD2::kSlideRackComponentType, .displayName = "TD2 滑块货架", .category = "TD2",
                .description = "压板让所在电视滑进相邻的空槽",
            }) &&
            context.RegisterComponent<TD2::StackColumnComponent>({
                .typeName = TD2::kStackColumnComponentType, .displayName = "TD2 电视塔", .category = "TD2",
                .description = "抽出最下面的电视，上面的电视整摞下落",
            }) &&
            context.RegisterComponent<TD2::CarouselComponent>({
                .typeName = TD2::kCarouselComponentType, .displayName = "TD2 转柱", .category = "TD2",
                .description = "四面挂电视的方柱，每次转一面",
            }) &&
            context.RegisterComponent<TD2::SignalCableComponent>({
                .typeName = TD2::kSignalCableComponentType, .displayName = "TD2 信号线", .category = "TD2",
                .description = "连接的压板通电时换成发光的模型",
            }) &&
            context.RegisterComponent<TD2::CameraAreaComponent>({
                .typeName = TD2::kCameraAreaComponentType, .displayName = "TD2 镜头大区域", .category = "TD2",
                .description = "多个区域组成一个大区域：区域内镜头以区域机位为基准动态跟随，跨大区域才播放演出",
            }) &&
            context.RegisterComponent<TD2::ScreenStyleComponent>({
                .typeName = TD2::kScreenStyleComponentType, .displayName = "TD2 屏幕3D风格", .category = "TD2",
                .description = "电视独立3D捕获与漫画素描像素风格，不改变规则或厂房",
            }) &&
            context.RegisterComponent<TD2::StageLightComponent>({
                .typeName = TD2::kStageLightComponentType, .displayName = "TD2 舞台灯", .category = "TD2",
                .description = "屏幕风格开「受光」时，Play 中按房间摆放照亮电视里的舞台",
            }) &&
            context.RegisterComponent<TD2::StageActorComponent>({
                .typeName = TD2::kStageActorComponentType, .displayName = "TD2 舞台人形", .category = "TD2",
                .description = "屏幕舞台里 Play 中代替烘好姿势的骨骼人形（主角或残影）",
            }) &&
            context.RegisterComponent<TD2::IntroComponent>({
                .typeName = TD2::kIntroComponentType, .displayName = "TD2 开场演出", .category = "TD2",
                .description = "从头 Play 时先显示电脑上的标题，按空格播放开场：灯光频闪、闪电、电视雪花、被吸进屏幕",
            }) &&
            context.RegisterComponent<TD2::ScreenFxComponent>({
                .typeName = TD2::kScreenFxComponentType, .displayName = "TD2 屏幕特效", .category = "TD2",
                .description = "电视内游戏事件特效（VFX 资产，可在 VFX 工作区编辑），不改变规则",
            }) &&
            context.RegisterSystem<TD2::ScreenPresentationSystem>({
                .systemId = TD2::kScreenSystemId,
                .displayName = "TD2 屏幕呈现",
            }) &&
            context.RegisterComponent<TD2::LightCullComponent>({
                .typeName = TD2::kLightCullComponentType, .displayName = "TD2 灯光预算", .category = "TD2",
                .description = "Play 中视野外的灯光暂时关闭（不改场景数值）",
            }) &&
            context.RegisterSystem<TD2::LightBudgetSystem>({
                .systemId = TD2::kLightBudgetSystemId,
                .displayName = "TD2 灯光预算",
            }) &&
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
