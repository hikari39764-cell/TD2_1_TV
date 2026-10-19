#pragma once
// テレビの装置欄の書式（一行一つ：id kind x y w h [key=value ...]、# 以降は注釈）。
// ランタイムの読み込みとエディタの読み書きで共有する。
#include <array>
#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TD2 {
// 装置の種類名。並びはルールの DeviceKind と同じ（受け口の Mechanism は作者が書かない）。
inline constexpr std::array<std::string_view, 12> kDeviceKindNames{"Solid", "OneWay", "Plate", "Door", "Laser", "Spikes", "Checkpoint", "LockWall",
                                                                   "Spring", "Bridge", "Collectible", "Decal"};
// 向きの名前。並びはルールの Direction と同じ。
inline constexpr std::array<std::string_view, 4> kDirectionNames{"Up", "Down", "Left", "Right"};

// 一行分。targets 以外の可選項は書いた順に文字列のまま持つ（値は Parse で検査済み）。
struct DeviceLine {
    std::string id, kind;
    double x = 0, y = 0, w = 1, h = 1;
    std::vector<std::string> targets;
    std::vector<std::pair<std::string, std::string>> options;
};

namespace DeviceText {
template <size_t N>
int IndexOf(const std::array<std::string_view, N>& names, std::string_view name) {
    for (size_t i = 0; i < N; ++i) { if (names[i] == name) { return static_cast<int>(i); } }
    return -1;
}
// 文字列全体が有限の数なら true。
inline bool Number(std::string_view text, double& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
}

// 読めない行があれば「设备第 N 行：…」を error に書いて false を返す。
inline bool Parse(std::string_view text, std::vector<DeviceLine>& out, std::string& error) {
    out.clear(); error.clear();
    std::istringstream lines{std::string(text)};
    unsigned lineNumber = 0;
    for (std::string line; std::getline(lines, line);) {
        ++lineNumber;
        if (const auto comment = line.find('#'); comment != std::string::npos) { line.resize(comment); }
        std::istringstream values(line);
        values.imbue(std::locale::classic());
        DeviceLine device;
        if (!(values >> device.id)) { continue; }
        const auto fail = [&error, lineNumber](std::string_view message) {
            error = "设备第 " + std::to_string(lineNumber) + " 行：" + std::string(message);
            return false;
        };
        if (!(values >> device.kind >> device.x >> device.y >> device.w >> device.h) || IndexOf(kDeviceKindNames, device.kind) < 0) {
            return fail("需要 id kind x y w h；类型使用规则枚举名称");
        }
        for (std::string option; values >> option;) {
            const size_t equals = option.find('=');
            if (equals == std::string::npos) { return fail("可选项使用 key=value"); }
            std::string key = option.substr(0, equals), value = option.substr(equals + 1);
            double number = 0;
            if (key == "targets") {
                if (value.empty() || value.back() == ',') { return fail("连接目标 ID 不能为空"); }
                std::istringstream ids(value);
                for (std::string id; std::getline(ids, id, ',');) {
                    if (id.empty()) { return fail("连接目标 ID 不能为空"); }
                    device.targets.push_back(id);
                }
                continue;
            }
            if (key == "dir") {
                if (IndexOf(kDirectionNames, value) < 0) { return fail("dir 使用 Up/Down/Left/Right"); }
            } else if (key == "invert" || key == "latch") {
                if (value != "0" && value != "1") { return fail(key + " 使用 0 或 1"); }
            } else if (key == "range" || key == "power" || key == "delay" || key == "cooldown" || key == "opacity") {
                if (!Number(value, number)) { return fail(key + " 必须为有限数字"); }
            } else if (key != "variant") {
                return fail("未知可选字段：" + key);
            }
            device.options.emplace_back(std::move(key), std::move(value));
        }
        out.push_back(std::move(device));
    }
    return true;
}
}
}
