#pragma once

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

// Scene の TD2.Region から作る値の写し。別の保存先や配置インスタンスを増やさない。
// 未配置の部屋も編集可能だが、実行世界の地形・隣接・開始領域には含めない。
struct RegionPlacement {
    std::string regionId;
    int cols = 4;
    int rows = 3;
    bool worldPlaced = false;
    int worldX = 0;
    int worldY = 0;

    bool operator==(const RegionPlacement&) const = default;
};

// 右端と下端を含まない格矩形。作者の int 原点と寸法の加算は必ず int64_t で行う。
struct WorldGridRect {
    int64_t left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const WorldGridRect&) const = default;
};

// 物理位置は連続値。整数に丸めるのは Editor の部屋配置だけである。
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

// 任意の演出だけを表す。通路の許可や着地点を持たず、隣接判定にも渡さない。
// A→B は正再生、B→A は逆再生。空 GUID は現カメラを保持する。
struct RegionSequenceBinding {
    std::string regionA;
    std::string regionB;
    std::string sequenceAssetGuid;
    std::string sequenceSlotName;
    uint64_t cameraObject = 0;
    // 制作用の任意参照。既存の集約初期化を保ち、実行時の姿勢は Sequence の端点だけで決める。
    uint64_t sourceCameraObject = 0;
    uint64_t targetCameraObject = 0;

    bool operator==(const RegionSequenceBinding&) const = default;
};

namespace WorldLayoutDetail {
inline bool ValidRegionId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 95) return false;
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}
inline bool HexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
} // namespace WorldLayoutDetail

inline WorldGridRect WorldBounds(const RegionPlacement& region) noexcept {
    return {region.worldX, region.worldY,
            static_cast<int64_t>(region.worldX) + static_cast<int64_t>(region.cols),
            static_cast<int64_t>(region.worldY) + static_cast<int64_t>(region.rows)};
}

inline WorldLayoutValidation ValidateRegionPlacement(const RegionPlacement& region) {
    if (!WorldLayoutDetail::ValidRegionId(region.regionId))
        return {false, "区域 ID 必须为 1–95 个 ASCII 字母、数字、点、短横线或下划线", region.regionId, {}};
    if (region.cols <= 0 || region.rows <= 0)
        return {false, "电视地图的整数行列必须为正", region.regionId, {}};
    return {true, {}, {}, {}};
}

// 正の面積だけを重複とする。共有辺や角の接触は面積重複ではない。
inline bool WorldRectsOverlap(const WorldGridRect& a, const WorldGridRect& b) noexcept {
    return (std::max)(a.left, b.left) < (std::min)(a.right, b.right) &&
           (std::max)(a.top, b.top) < (std::min)(a.bottom, b.bottom);
}

inline WorldLayoutValidation ValidateWorldLayout(std::span<const RegionPlacement> regions) {
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (auto validation = ValidateRegionPlacement(regions[i]); !validation.valid) return validation;
        for (std::size_t j = 0; j < i; ++j) {
            if (regions[i].regionId == regions[j].regionId)
                return {false, "区域 ID 重复；布置不能复制同一房间身份", regions[j].regionId, regions[i].regionId};
            if (regions[i].worldPlaced && regions[j].worldPlaced &&
                WorldRectsOverlap(WorldBounds(regions[i]), WorldBounds(regions[j])))
                return {false, "已布置电视地图发生面积重叠", regions[j].regionId, regions[i].regionId};
        }
    }
    return {true, {}, {}, {}};
}

// 整数境界を厳密に比較する。部分辺は接続できるが、角接触・隙間は接続しない。
// 得られるのは地形的な隣接だけであり、身体の幅・高さや装置の通行可否は物理側で調べる。
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

inline WorldPoint LocalToWorld(const RegionPlacement& region, WorldPoint local) noexcept {
    return {local.x + region.worldX, local.y + region.worldY};
}
inline WorldPoint WorldToLocal(const RegionPlacement& region, WorldPoint world) noexcept {
    return {world.x - region.worldX, world.y - region.worldY};
}
inline bool ContainsWorldPoint(const RegionPlacement& region, WorldPoint point) noexcept {
    if (!region.worldPlaced || region.cols <= 0 || region.rows <= 0 ||
        !std::isfinite(point.x) || !std::isfinite(point.y)) return false;
    const auto rect = WorldBounds(region);
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

// 画面の所属判定には身体中心を渡す。衝突用の全身矩形の判定には流用しない。
// 配置全体は事前検証する。重複する点を見つけても勝手に最初の部屋を選ばない。
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

// アセットの存在・スロット名の一致・SceneObject の解決は SDK adapter の責務。
// この検証は演出の値だけを検査し、部屋が現在隣接していることを通行条件に追加しない。
inline WorldLayoutValidation ValidateRegionSequenceBinding(const RegionSequenceBinding& binding) {
    if (!WorldLayoutDetail::ValidRegionId(binding.regionA) || !WorldLayoutDetail::ValidRegionId(binding.regionB) ||
        binding.regionA == binding.regionB)
        return {false, "Sequence 绑定需要两个不同的有效区域 ID", binding.regionA, binding.regionB};
    if (!binding.sequenceAssetGuid.empty() &&
        (binding.sequenceAssetGuid.size() != 32 ||
         !std::all_of(binding.sequenceAssetGuid.begin(), binding.sequenceAssetGuid.end(), WorldLayoutDetail::HexDigit)))
        return {false, "Sequence 引用必须是已有资产的 32 位 GUID", binding.regionA, binding.regionB};
    if (binding.sequenceSlotName.size() > 63 || binding.sequenceSlotName.find('\0') != std::string::npos)
        return {false, "Sequence 槽名不得超过 63 个 UTF-8 字节或包含空字符", binding.regionA, binding.regionB};
    if (binding.sequenceSlotName.empty() != (binding.cameraObject == 0))
        return {false, "可选 Sequence 槽名与摄像机对象必须同时指定或同时留空", binding.regionA, binding.regionB};
    return {true, {}, {}, {}};
}

} // namespace TD2
