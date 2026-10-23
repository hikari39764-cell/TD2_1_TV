#include "TD2RegionAuthoring.h"
#include "../Common/TD2DeviceText.h"
#include <utility>

namespace TD2 {
static_assert(static_cast<size_t>(Rules::DeviceKind::Goal) + 1 == kDeviceKindNames.size(), "装置の種類名の並びはルールの列挙と同じにする");
static_assert(static_cast<size_t>(Rules::Direction::Right) + 1 == kDirectionNames.size(), "向きの名前の並びはルールの列挙と同じにする");

bool ParseDevices(std::string_view text, std::vector<Rules::DeviceDefinition>& devices, std::string& error) {
    devices.clear();
    std::vector<DeviceLine> lines;
    if (!DeviceText::Parse(text, lines, error)) { return false; }
    for (auto& line : lines) {
        // 種類名と向きの名前の並びはルールの列挙と同じ（書式の検査は Parse で済んでいる）。
        Rules::DeviceDefinition device;
        device.id = std::move(line.id);
        device.kind = static_cast<Rules::DeviceKind>(DeviceText::IndexOf(kDeviceKindNames, line.kind));
        device.rect = {line.x, line.y, line.w, line.h};
        device.targets = std::move(line.targets);
        for (const auto& [key, value] : line.options) {
            double number = 0;
            (void)DeviceText::Number(value, number);
            if (key == "dir") { device.direction = static_cast<Rules::Direction>(DeviceText::IndexOf(kDirectionNames, value)); }
            else if (key == "invert") { device.inverted = value == "1"; }
            else if (key == "latch") { device.latch = value == "1"; }
            else if (key == "all") { device.all = value == "1"; }
            else if (key == "range") { device.range = number; }
            // バネの強さ（マス/秒）・溜めと冷却の秒数、貼り絵の図案と不透明度。
            else if (key == "power") { device.power = number; }
            else if (key == "delay") { device.delay = number; }
            else if (key == "cooldown") { device.cooldown = number; }
            else if (key == "opacity") { device.opacity = number; }
            else if (key == "depth") { device.depth = number; }
            else if (key == "variant") { device.variant = value; }
        }
        devices.push_back(std::move(device));
    }
    return true;
}

}
