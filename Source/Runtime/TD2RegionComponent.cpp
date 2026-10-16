#include "TD2RegionComponent.h"
#include "TD2RegionAuthoring.h"

namespace TD2 {

bool RegionComponent::BuildDefinition(Rules::WorldDefinition& definition, std::string& error) const {
    definition.region = region_;
    definition.spawn = {spawnX_, spawnY_};
    if (!ParseDevices(devices_, definition.devices, error)) { return false; }
    error = Rules::ValidateWorld(definition);
    return error.empty();
}

const Rules::WorldDefinition& RegionComponent::AuthoredDefinition() {
    auto& cache = authored_;
    if (cache.valid && cache.region == region_ && cache.devices == devices_ && cache.spawnX == spawnX_ && cache.spawnY == spawnY_) {
        return cache.definition;
    }
    cache.region = region_; cache.devices = devices_; cache.spawnX = spawnX_; cache.spawnY = spawnY_; cache.valid = true;
    std::string error;
    if (!BuildDefinition(cache.definition, error)) {
        // 装置の行が不正でも画面は消さず、地形を見ながら直せるようにする。
        cache.definition.region = region_;
        cache.definition.spawn = {spawnX_, spawnY_};
        cache.definition.devices.clear();
    }
    return cache.definition;
}

}
