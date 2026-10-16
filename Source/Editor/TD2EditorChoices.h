#pragma once
#include "TD2EditorSupport.h"
#include <algorithm>
#include <array>

namespace TD2::Editor {
inline constexpr std::array<std::string_view, 12> kEditorDeviceKinds{"Solid", "OneWay", "Plate", "Door", "Laser", "Spikes", "Checkpoint", "LockWall",
                                                                     "Spring", "Bridge", "Collectible", "Decal"};
inline constexpr std::array<std::string_view, 12> kEditorDeviceLabels{"▧ 实体块", "━ 单向平台", "▰ 压板", "▥ 门", "→ 激光", "▲ 尖刺", "⚑ 存档点", "▣ 锁墙",
                                                                      "⤒ 弹簧", "═ 伸缩桥", "◆ 收集物", "✎ 教程贴画"};

inline std::string DeviceLabel(std::string_view kind) {
    const auto it = std::find(kEditorDeviceKinds.begin(), kEditorDeviceKinds.end(), kind);
    return it == kEditorDeviceKinds.end() ? std::string(kind) : std::string(kEditorDeviceLabels[static_cast<size_t>(it - kEditorDeviceKinds.begin())]);
}

// 狭い欄でも UTF-8 の途中を切らず、可視ラベルを短く保つ。識別には別の stable ID を使う。
inline std::string ShortChoiceLabel(std::string_view value, size_t maximum = 18) {
    size_t end = 0, count = 0;
    while (end < value.size() && count < maximum) {
        ++end; while (end < value.size() && (static_cast<unsigned char>(value[end]) & 0xc0u) == 0x80u) { ++end; }
        ++count;
    }
    return std::string(value.substr(0, end)) + (end < value.size() ? "…" : "");
}

struct EditorChoice { std::string id, label; bool selected = false; };

// 候補を全部並べる（頁送りなし。面板自体が縦に流れる）。長い一覧は呼び手が検索欄で絞る。
inline int DrawChoices(IEditorWorkspaceUi& ui, const std::vector<EditorChoice>& choices) {
    if (choices.empty()) { ui.Text("没有可选项"); return -1; }
    int chosen = -1;
    for (size_t i = 0; i < choices.size(); ++i) {
        if (ui.Selectable(choices[i].id, ShortChoiceLabel(choices[i].label, 24), choices[i].selected)) { chosen = static_cast<int>(i); }
    }
    return chosen;
}

// 一度に表示する候補数を固定し、資産・相機・装置の長い一覧で下の保存操作を隠さない。
inline int DrawPagedChoices(IEditorWorkspaceUi& ui, const std::vector<EditorChoice>& choices, int& page,
                            std::string_view group, int pageSize = 5) {
    if (choices.empty()) { ui.Text("没有可选项"); return -1; }
    const int count = static_cast<int>(choices.size());
    const int lastPage = (count - 1) / pageSize;
    page = std::clamp(page, 0, lastPage);
    int chosen = -1;
    for (int i = page * pageSize; i < (std::min)(count, (page + 1) * pageSize); ++i) {
        const auto& choice = choices[static_cast<size_t>(i)];
        if (ui.Button(choice.id, ShortChoiceLabel(choice.label) + (choice.selected ? " ●" : ""))) { chosen = i; }
    }
    if (lastPage > 0) {
        const std::string key(group);
        if (ui.Button(key + ".previous", "上一页") && page > 0) { --page; }
        ui.SameLine(); if (ui.Button(key + ".next", "下一页") && page < lastPage) { ++page; }
        ui.Text(std::to_string(page + 1) + " / " + std::to_string(lastPage + 1));
    }
    return chosen;
}
}
