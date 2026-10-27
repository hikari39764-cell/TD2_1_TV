#pragma once
// 仕掛けの 3D の動き（TD2MachineViews.cpp と TD2MachineRacks.cpp）で共有する小さな関数。ほかのファイルからは読まない。
#include "TD2MachineViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <string_view>
#include <utility>
#include <vector>

namespace TD2::MachineDetail {
using F3 = HIKARI::GameProjectFloat3;
// 動いた部屋の画面が、止まった後も砂嵐のままでいる秒（信号が合うまでの間）。
inline constexpr double kSnowTail = .3;
// 種類名の部品を全部たどる。
template <class Component, class Visit>
void ForEach(HIKARI::WorldView& world, std::string_view type, Visit&& visit) {
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(type)};
    (void)world.ForEachComponents<Component>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm, std::forward<Visit>(visit));
}
// 書き込みの束へ一つの物体の姿勢を足す（まとめて SetLocalTransforms する）。
inline void Push(std::vector<HIKARI::GameProjectWorldTransformWrite>& writes, HIKARI::GameProjectObjectHandle object, const HIKARI::GameProjectTransform& pose) {
    writes.push_back({}); writes.back().object = object; writes.back().localTransform = pose;
}
}
