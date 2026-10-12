#include "TD2RegionComponent.h"
#include "TD2GameSession.h"
#include "TD2RegionAuthoring.h"
#include "HIKARI/Project/GameFlowViews.h"

namespace TD2 {

bool RegionComponent::BuildDefinition(Rules::WorldDefinition& definition, std::string& error) const {
    definition.region = region_;
    definition.spawn = {spawnX_, spawnY_};
    if (!ParseDevices(devices_, definition.devices, error)) { return false; }
    error = Rules::ValidateWorld(definition);
    return error.empty();
}

void RegionComponent::RefreshScreen(HIKARI::WorldView& world) {
    // Session が持つ判定と補間のスナップショットだけを表示する。Presenter は状態を進めない。
    auto* session = dynamic_cast<GameSession*>(world.GetGameSession());
    if (session != nullptr && world.GetRuntimeSession().IsPlayActive()) {
        if (const auto* runtime = session->FindRegion(region_.regionId)) {
            const auto& snapshot = runtime->snapshot;
            runtimeStatus_ = "Play tick=" + std::to_string(snapshot.tick) +
                " player=(" + std::to_string(snapshot.player.rect.x) + "," + std::to_string(snapshot.player.rect.y) +
                ") echo=" + std::to_string(snapshot.echo.has_value()) + " deaths=" + std::to_string(snapshot.deaths);
            presenter_.Refresh(world, runtime->definition, screen_, &snapshot, snapshot.playerPresent);
            return;
        }
    }
    Rules::WorldDefinition definition;
    std::string error;
    if (!BuildDefinition(definition, error)) {
        runtimeStatus_ = error;
        // 作者の無効な装置行を画面の消失に変えず、基本地形を見ながら修正できるようにする。
        definition.region = region_;
        definition.spawn = {spawnX_, spawnY_};
        definition.devices.clear();
    } else { runtimeStatus_ = "编辑态地图与出生标记；A/D/Space 仅在 Play 时推进规则"; }
    presenter_.Refresh(world, definition, screen_);
}

} // namespace TD2
