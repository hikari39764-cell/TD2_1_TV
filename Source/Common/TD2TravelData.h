#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kPassageComponentType = "TD2.Passage";
inline constexpr std::string_view kCameraPerformanceComponentType = "TD2.CameraPerformance";

// 各端点はそのテレビの格座標。landing はその領域に入るプレイヤーの左上位置。
struct PassageEndpoint {
    std::string regionId;
    double x = 0, y = 0, w = 1, h = 1, landingX = 1, landingY = 1;
    int direction = 1;
};
struct PassageDefinition {
    std::string passageId;
    PassageEndpoint a, b;
    bool bidirectional = true;
    std::string performanceId;
};

struct PassageValidation { bool valid = false; std::string_view message; };

// Editor と runtime で同じ静的契約を使う。格数だけを受け取り、3D 変換や動的装置は参照しない。
inline PassageValidation ValidatePassageEndpoint(const PassageEndpoint& e, int cols, int rows) noexcept {
    if (e.regionId.empty() || cols <= 0 || rows <= 0)
        return {false, "端口需要有效区域 ID 与正整数行列"};
    if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.w) || !std::isfinite(e.h) ||
        !std::isfinite(e.landingX) || !std::isfinite(e.landingY))
        return {false, "端口与落点坐标必须为有限数值"};
    if (e.direction != -1 && e.direction != 1)
        return {false, "端口方向仅支持 -1（左）或 +1（右）"};
    if (e.w <= 0 || e.h <= 0 || e.x < 0 || e.y < 0 || e.x + e.w > cols || e.y + e.h > rows)
        return {false, "端口触发区必须完整位于区域内且宽高为正"};
    // 作者値は float を通るため、境界一致だけに小さな許容差を設ける。
    if (std::abs(e.direction < 0 ? e.x : e.x + e.w - cols) > 1e-5)
        return {false, "左出口必须接触左边界，右出口必须接触右边界"};
    if (e.landingX < 0 || e.landingY < 0 || e.landingX + .625 > cols || e.landingY + .875 > rows)
        return {false, "落点必须容纳完整的 0.625×0.875 格玩家"};
    return {true, {}};
}

inline PassageValidation ValidatePassage(const PassageDefinition& p, int aCols, int aRows,
                                         int bCols, int bRows) noexcept {
    if (p.passageId.empty() || p.a.regionId == p.b.regionId)
        return {false, "通路需要非空 ID 和两个不同区域"};
    const auto a = ValidatePassageEndpoint(p.a, aCols, aRows);
    if (!a.valid) return a;
    const auto b = ValidatePassageEndpoint(p.b, bCols, bRows);
    if (!b.valid) return b;
    // 隣室接続は右出口と左出口を対にする。片道でも到着端の向きは変えない。
    if (p.a.direction != -p.b.direction)
        return {false, "相邻区域的两个端口必须方向相反（右接左或左接右）"};
    return {true, {}};
}
struct CameraPoint { double x = 0, y = 0, z = 0; };
struct CameraPerformanceDefinition {
    std::string performanceId;
    uint64_t cameraObject = 0;
    double durationSeconds = 1, fovDegrees = 50;
    CameraPoint startEye{0, 2, -5}, startTarget{0, 2, 0};
    CameraPoint endEye{3, 2, -5}, endTarget{3, 2, 0}, up{0, 1, 0};
};
struct CameraTravelState {
    CameraPerformanceDefinition definition;
    double progress = 0;
    bool reverse = false, moving = false;
};
struct CameraTravelPose { CameraPoint eye, target, up; double fovDegrees = 50; };

// 逆再生は進捗の向きだけを変える。物理時間と入力履歴を巻き戻さない。
inline CameraTravelPose EvaluateCameraTravel(const CameraTravelState& state) {
    const double t = std::clamp(state.progress, 0.0, 1.0), u = t * t * (3.0 - 2.0 * t);
    const auto mix = [u](CameraPoint a, CameraPoint b) {
        return CameraPoint{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u};
    };
    return {mix(state.definition.startEye, state.definition.endEye),
            mix(state.definition.startTarget, state.definition.endTarget), state.definition.up, state.definition.fovDegrees};
}
} // namespace TD2
