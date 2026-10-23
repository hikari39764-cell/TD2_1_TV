#pragma once

#include "TD2RegionData.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace TD2 {

inline constexpr std::string_view kRegionSequenceComponentType = "TD2.RegionSequence";

// Scene の TD2.Region から作る写し。保存先は増やさない。
// 未配置の部屋は編集できるが、実行時の世界には含めない。
struct RegionPlacement {
    std::string regionId;
    int cols = 4;
    int rows = 3;
    bool worldPlaced = false;
    int worldX = 0;
    int worldY = 0;

    bool operator==(const RegionPlacement&) const = default;
};

// 右端・下端を含まないマスの矩形。原点と寸法の加算は int64_t で行う。
struct WorldGridRect {
    int64_t left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const WorldGridRect&) const = default;
};

// 物理位置は連続値。整数に丸めるのはエディタの部屋配置だけ。
struct WorldPoint {
    double x = 0, y = 0;
    bool operator==(const WorldPoint&) const = default;
};

enum class WorldSide { Left, Right, Top, Bottom };

struct SharedWorldEdge {
    WorldSide sideA = WorldSide::Right;
    WorldSide sideB = WorldSide::Left;
    // 左右辺なら coordinate=X / overlap=Y、上下辺なら coordinate=Y / overlap=X。
    int64_t coordinate = 0;
    int64_t overlapBegin = 0;
    int64_t overlapEnd = 0;
    bool operator==(const SharedWorldEdge&) const = default;
};

struct WorldLayoutValidation {
    bool valid = false;
    std::string message;
    std::string regionA;
    std::string regionB;
};

// 演出だけを表す。通行や隣接の判定には使わない。
// A→B は正再生、B→A は逆再生。GUID が空なら今のカメラのまま。
struct RegionSequenceBinding {
    std::string regionA;
    std::string regionB;
    std::string sequenceAssetGuid;
    std::string sequenceSlotName;
    uint64_t cameraObject = 0;
    // 再生の倍速（エンジンの演出は最短 1 秒なので、短い切り替えは倍速で流す）。
    float speed = 1.0f;
    // 歩いて越えても A/D で止めず、最後まで自動で流す（超現実の切り替え）。演出の間も画面の中は動き続ける。
    bool autoPlay = false;

    bool operator==(const RegionSequenceBinding&) const = default;
};

namespace WorldLayoutDetail {
inline bool HexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
}

inline WorldGridRect WorldBounds(const RegionPlacement& region) noexcept {
    return {region.worldX, region.worldY,
            static_cast<int64_t>(region.worldX) + static_cast<int64_t>(region.cols),
            static_cast<int64_t>(region.worldY) + static_cast<int64_t>(region.rows)};
}

inline WorldLayoutValidation ValidateRegionPlacement(const RegionPlacement& region) {
    if (!IsStableId(region.regionId))
        return {false, "区域 ID は ASCII の英数字・点・ハイフン・下線で 1～95 文字", region.regionId, {}};
    if (region.cols <= 0 || region.rows <= 0)
        return {false, "テレビの地図の行と列は正の整数", region.regionId, {}};
    return {true, {}, {}, {}};
}

// 正の面積だけを重なりとする。辺や角の接触は重なりではない。
inline bool WorldRectsOverlap(const WorldGridRect& a, const WorldGridRect& b) noexcept {
    return (std::max)(a.left, b.left) < (std::min)(a.right, b.right) &&
           (std::max)(a.top, b.top) < (std::min)(a.bottom, b.bottom);
}

inline WorldLayoutValidation ValidateWorldLayout(std::span<const RegionPlacement> regions) {
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (auto validation = ValidateRegionPlacement(regions[i]); !validation.valid) return validation;
        for (std::size_t j = 0; j < i; ++j) {
            if (regions[i].regionId == regions[j].regionId)
                return {false, "区域 ID が重なっています", regions[j].regionId, regions[i].regionId};
            if (regions[i].worldPlaced && regions[j].worldPlaced &&
                WorldRectsOverlap(WorldBounds(regions[i]), WorldBounds(regions[j])))
                return {false, "置いたテレビの地図が重なっています", regions[j].regionId, regions[i].regionId};
        }
    }
    return {true, {}, {}, {}};
}

// 整数の境界で比べる。辺の一部が接していれば接続し、角の接触や隙間は接続しない。
// 地形の隣接だけを返す。実際に通れるかは物理側で調べる。
inline std::optional<SharedWorldEdge> FindSharedWorldEdge(const RegionPlacement& a,
                                                         const RegionPlacement& b) noexcept {
    if (!a.worldPlaced || !b.worldPlaced || a.regionId == b.regionId ||
        a.regionId.empty() || b.regionId.empty() || a.cols <= 0 || a.rows <= 0 || b.cols <= 0 || b.rows <= 0) return {};
    const auto ar = WorldBounds(a), br = WorldBounds(b);
    const auto y0 = (std::max)(ar.top, br.top), y1 = (std::min)(ar.bottom, br.bottom);
    if (y0 < y1) {
        if (ar.right == br.left) return SharedWorldEdge{WorldSide::Right, WorldSide::Left, ar.right, y0, y1};
        if (ar.left == br.right) return SharedWorldEdge{WorldSide::Left, WorldSide::Right, ar.left, y0, y1};
    }
    const auto x0 = (std::max)(ar.left, br.left), x1 = (std::min)(ar.right, br.right);
    if (x0 < x1) {
        if (ar.bottom == br.top) return SharedWorldEdge{WorldSide::Bottom, WorldSide::Top, ar.bottom, x0, x1};
        if (ar.top == br.bottom) return SharedWorldEdge{WorldSide::Top, WorldSide::Bottom, ar.top, x0, x1};
    }
    return {};
}

inline bool ContainsWorldPoint(const RegionPlacement& region, WorldPoint point) noexcept {
    if (!region.worldPlaced || region.cols <= 0 || region.rows <= 0 ||
        !std::isfinite(point.x) || !std::isfinite(point.y)) return false;
    const auto rect = WorldBounds(region);
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

// 所属の判定には体の中心を渡す。衝突判定には使わない。
// 重なっている点では部屋を選ばない（配置は事前に検証する）。
inline const RegionPlacement* FindRegionAtWorldPoint(std::span<const RegionPlacement> regions,
                                                      WorldPoint point) noexcept {
    const RegionPlacement* found = nullptr;
    for (const auto& region : regions) {
        if (!ContainsWorldPoint(region, point)) continue;
        if (found != nullptr) return nullptr;
        found = &region;
    }
    return found;
}

inline bool SameRegionPair(const RegionSequenceBinding& a, const RegionSequenceBinding& b) noexcept {
    return (a.regionA == b.regionA && a.regionB == b.regionB) ||
           (a.regionA == b.regionB && a.regionB == b.regionA);
}

// 資産の存在やスロット名、SceneObject の解決は SDK 側で確かめる。
// ここでは演出の値だけを調べ、隣接を通行の条件にはしない。
inline WorldLayoutValidation ValidateRegionSequenceBinding(const RegionSequenceBinding& binding) {
    if (!IsStableId(binding.regionA) || !IsStableId(binding.regionB) ||
        binding.regionA == binding.regionB)
        return {false, "演出の結び付けには二つの違う区域 ID が要ります", binding.regionA, binding.regionB};
    if (!binding.sequenceAssetGuid.empty() &&
        (binding.sequenceAssetGuid.size() != 32 ||
         !std::all_of(binding.sequenceAssetGuid.begin(), binding.sequenceAssetGuid.end(), WorldLayoutDetail::HexDigit)))
        return {false, "演出は資産の 32 桁の GUID で指定します", binding.regionA, binding.regionB};
    if (binding.sequenceSlotName.size() > 63 || binding.sequenceSlotName.find('\0') != std::string::npos)
        return {false, "演出のスロット名は UTF-8 で 63 バイトまで、空文字を含めない", binding.regionA, binding.regionB};
    if (binding.sequenceSlotName.empty() != (binding.cameraObject == 0))
        return {false, "演出のスロット名とカメラは両方指定するか、両方空にします", binding.regionA, binding.regionB};
    return {true, {}, {}, {}};
}

}
