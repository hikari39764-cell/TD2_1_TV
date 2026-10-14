#include "TD2RegionAuthoring.h"
#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>
#include <utility>

namespace TD2 {
namespace {

bool DeviceKind(std::string_view name, Rules::DeviceKind& kind) {
    constexpr std::pair<std::string_view, Rules::DeviceKind> kinds[]{
        {"Solid", Rules::DeviceKind::Solid}, {"OneWay", Rules::DeviceKind::OneWay},
        {"Plate", Rules::DeviceKind::Plate}, {"Door", Rules::DeviceKind::Door},
        {"Laser", Rules::DeviceKind::Laser}, {"Spikes", Rules::DeviceKind::Spikes},
        {"Checkpoint", Rules::DeviceKind::Checkpoint}, {"LockWall", Rules::DeviceKind::LockWall},
        {"Spring", Rules::DeviceKind::Spring}, {"Bridge", Rules::DeviceKind::Bridge},
        {"Collectible", Rules::DeviceKind::Collectible}, {"Decal", Rules::DeviceKind::Decal},
    };
    for (const auto& [id, value] : kinds) { if (name == id) { kind = value; return true; } }
    return false;
}

bool Direction(std::string_view name, Rules::Direction& direction) {
    constexpr std::pair<std::string_view, Rules::Direction> directions[]{
        {"Up", Rules::Direction::Up}, {"Down", Rules::Direction::Down},
        {"Left", Rules::Direction::Left}, {"Right", Rules::Direction::Right},
    };
    for (const auto& [id, value] : directions) { if (name == id) { direction = value; return true; } }
    return false;
}

} // namespace

bool ParseDevices(std::string_view text, std::vector<Rules::DeviceDefinition>& devices, std::string& error) {
    devices.clear();
    std::istringstream lines{std::string(text)};
    std::string line;
    unsigned lineNumber = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (const auto comment = line.find('#'); comment != std::string::npos) { line.resize(comment); }
        std::istringstream values(line);
        values.imbue(std::locale::classic());
        Rules::DeviceDefinition device;
        std::string kind;
        if (!(values >> device.id)) { continue; }
        const auto fail = [&error, lineNumber](std::string_view message) {
            error = "设备第 " + std::to_string(lineNumber) + " 行：" + std::string(message);
            return false;
        };
        if (!(values >> kind >> device.rect.x >> device.rect.y >> device.rect.w >> device.rect.h) ||
            !DeviceKind(kind, device.kind)) { return fail("需要 id kind x y w h；类型使用规则枚举名称"); }
        std::string option;
        while (values >> option) {
            const size_t equals = option.find('=');
            if (equals == std::string::npos) { return fail("可选项使用 key=value"); }
            const std::string_view key(option.data(), equals);
            const std::string_view value(option.data() + equals + 1, option.size() - equals - 1);
            if (key == "targets") {
                if (value.empty()) { return fail("targets 不能为空"); }
                std::istringstream ids{std::string(value)};
                std::string id;
                while (std::getline(ids, id, ',')) {
                    if (id.empty()) { return fail("连接目标 ID 不能为空"); }
                    device.targets.push_back(id);
                }
            } else if (key == "dir") {
                if (!Direction(value, device.direction)) { return fail("dir 使用 Up/Down/Left/Right"); }
            } else if (key == "range") {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), device.range);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    !std::isfinite(device.range)) { return fail("range 必须为有限数字"); }
            } else if (key == "invert") {
                if (value != "0" && value != "1") { return fail("invert 使用 0 或 1"); }
                device.inverted = value == "1";
            } else if (key == "power" || key == "delay" || key == "cooldown" || key == "opacity") {
                // 弾簧の弾力（格/秒）・溜め秒・冷却秒、貼り絵の不透明度。
                double number = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(number)) {
                    return fail(std::string(key) + " 必须为有限数字");
                }
                (key == "power" ? device.power : key == "delay" ? device.delay : key == "cooldown" ? device.cooldown : device.opacity) = number;
            } else if (key == "variant") {
                device.variant = std::string(value);
            } else { return fail("未知可选字段：" + std::string(key)); }
        }
        devices.push_back(std::move(device));
    }
    error.clear();
    return true;
}

} // namespace TD2
