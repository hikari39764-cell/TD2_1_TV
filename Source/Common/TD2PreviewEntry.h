#pragma once

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace TD2 {
// Scene entry の名前は項目が解釈する論理入口。位置は区域内の左上座標で、保存資産には残さない。
inline constexpr std::string_view kPreviewEntryPrefix = "td2.preview:";
struct PreviewSpawn { double x = 0, y = 0; };

inline std::string EncodePreviewSpawn(PreviewSpawn spawn) {
    char buffer[48]{};
    auto x = std::to_chars(buffer, buffer + sizeof(buffer), spawn.x, std::chars_format::general, 8);
    if (x.ec != std::errc{} || x.ptr == buffer + sizeof(buffer)) { return {}; }
    *x.ptr++ = ',';
    const auto y = std::to_chars(x.ptr, buffer + sizeof(buffer), spawn.y, std::chars_format::general, 8);
    return y.ec == std::errc{} ? std::string(kPreviewEntryPrefix) + std::string(buffer, y.ptr) : std::string{};
}

inline std::optional<PreviewSpawn> DecodePreviewSpawn(std::string_view entry) {
    if (!entry.starts_with(kPreviewEntryPrefix)) { return {}; }
    entry.remove_prefix(kPreviewEntryPrefix.size());
    const auto comma = entry.find(',');
    if (comma == std::string_view::npos) { return {}; }
    PreviewSpawn spawn;
    const auto x = std::from_chars(entry.data(), entry.data() + comma, spawn.x);
    const auto y = std::from_chars(entry.data() + comma + 1, entry.data() + entry.size(), spawn.y);
    if (x.ec != std::errc{} || x.ptr != entry.data() + comma || y.ec != std::errc{} ||
        y.ptr != entry.data() + entry.size() || !std::isfinite(spawn.x) || !std::isfinite(spawn.y)) { return {}; }
    return spawn;
}
} // namespace TD2
