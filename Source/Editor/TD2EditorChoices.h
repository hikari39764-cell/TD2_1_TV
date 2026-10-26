#pragma once
#include "TD2EditorSupport.h"
#include "TD2DeviceEditor.h"
#include "../Common/TD2DeviceText.h"
#include <algorithm>
#include <array>

namespace TD2::Editor {
// 装置の種類（並びと名前は TD2DeviceText.h）と、エディタでの表示名。
inline constexpr const auto& kEditorDeviceKinds = kDeviceKindNames;
inline constexpr std::array<std::string_view, 14> kEditorDeviceLabels{"▧ ブロック", "━ 片側床", "▰ 圧力板", "▥ 扉", "→ レーザー", "▲ トゲ", "⚑ チェックポイント", "▣ ロック壁",
                                                                      "⤒ バネ", "═ 伸びる橋", "◆ 収集物", "✎ 説明の貼り紙", "◎ ゴール", "⇉ 押し刺"};

inline std::string DeviceLabel(std::string_view kind) {
    const auto it = std::find(kEditorDeviceKinds.begin(), kEditorDeviceKinds.end(), kind);
    return it == kEditorDeviceKinds.end() ? std::string(kind) : std::string(kEditorDeviceLabels[static_cast<size_t>(it - kEditorDeviceKinds.begin())]);
}

// 記号を除いた種類の名前（「圧力板」）。
inline std::string DeviceKindName(std::string_view kind) {
    const auto label = DeviceLabel(kind);
    const auto space = label.find(' ');
    return space == std::string::npos ? label : label.substr(space + 1);
}
// 作者に見せる装置の名前：「圧力板 A」。古い ID の装置は ID を添える。
inline std::string DeviceName(const DeviceDraft& d) {
    const auto letter = DeviceLetter(d);
    return DeviceKindName(d.kind) + " " + (letter.empty() ? d.id : letter);
}

// 狭い欄でも UTF-8 の途中で切らずに短くする。識別は別の ID で行う。
inline std::string ShortChoiceLabel(std::string_view value, size_t maximum = 18) {
    size_t end = 0, count = 0;
    while (end < value.size() && count < maximum) {
        ++end; while (end < value.size() && (static_cast<unsigned char>(value[end]) & 0xc0u) == 0x80u) { ++end; }
        ++count;
    }
    return std::string(value.substr(0, end)) + (end < value.size() ? "…" : "");
}

// 画布の文字の幅の目安（英数字は狭く、漢字は広い）。名前の下地の大きさに使う。
inline float TextWidth(std::string_view text) {
    float width = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if ((c & 0xc0u) == 0x80u) { continue; }
        width += c < 0x80u ? 7.0f : 13.0f;
    }
    return width;
}

}
