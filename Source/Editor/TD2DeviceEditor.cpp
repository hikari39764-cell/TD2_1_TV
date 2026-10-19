#include "TD2DeviceEditor.h"
#include "../Common/TD2DeviceText.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace TD2::Editor {
std::string DeviceDraft::Extra(std::string_view key) const {
    for (const auto& option : extra) {
        if (option.size() > key.size() && option.compare(0, key.size(), key) == 0 && option[key.size()] == '=') { return option.substr(key.size() + 1); }
    }
    return {};
}
void DeviceDraft::SetExtra(std::string_view key, std::string_view value) {
    std::erase_if(extra, [key](const std::string& option) { return option.size() > key.size() && option.compare(0, key.size(), key) == 0 && option[key.size()] == '='; });
    if (!value.empty()) { extra.push_back(std::string(key) + "=" + std::string(value)); }
}

TargetKind ClassifyTarget(std::string_view target) {
    const auto parts = [](std::string_view text) {
        std::vector<std::string_view> result;
        for (size_t start = 0;;) {
            const auto slash = text.find('/', start);
            result.push_back(text.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
            if (slash == std::string_view::npos) { return result; }
            start = slash + 1;
        }
    };
    const auto p = parts(target);
    if (std::any_of(p.begin(), p.end(), [](std::string_view s) { return !IsStableId(s); })) { return TargetKind::Invalid; }
    if (p.size() == 1) { return TargetKind::Local; }
    if (p[0] == "mech" && (p.size() == 2 || (p.size() == 3 && p[2] == "r"))) { return TargetKind::Machine; }
    if (p[0] == "slide" && p.size() == 2) { return TargetKind::Machine; }
    if (p[0] == "stack" && p.size() == 3 && (p[2] == "pull" || p[2] == "reset")) { return TargetKind::Machine; }
    if (p[0] == "carousel" && p.size() == 3 && (p[2] == "cw" || p[2] == "ccw")) { return TargetKind::Machine; }
    if (p.size() == 2 && p[0] != "mech" && p[0] != "slide" && p[0] != "stack" && p[0] != "carousel") { return TargetKind::OtherRegion; }
    return TargetKind::Invalid;
}

void DeviceEditor::Load(std::string_view text) {
    const std::string selectedId = HasSelection() ? devices_[selected_].id : "";
    devices_.clear(); selected_ = -1; error_.clear();
    // 読めない行があれば、その前までを表示し、編集はさせない（原文を場景に残す）。
    std::vector<DeviceLine> lines;
    if (!DeviceText::Parse(text, lines, error_)) { error_ += "；原文保留，未覆盖场景。"; }
    for (auto& line : lines) {
        DeviceDraft device;
        device.id = std::move(line.id); device.kind = std::move(line.kind);
        device.x = line.x; device.y = line.y; device.w = line.w; device.h = line.h;
        device.targets = std::move(line.targets);
        for (const auto& [key, value] : line.options) {
            if (key == "dir") { device.direction = value; }
            else if (key == "range") { (void)DeviceText::Number(value, device.range); }
            else if (key == "invert") { device.inverted = value == "1"; }
            else if (key == "latch") { device.latch = value == "1"; }
            else { device.extra.push_back(key + "=" + value); }
        }
        if (device.id == selectedId) { selected_ = static_cast<int>(devices_.size()); }
        devices_.push_back(std::move(device));
    }
}

int DeviceEditor::Hit(double x, double y) const {
    for (size_t i = devices_.size(); i > 0; --i) {
        const auto& d = devices_[i - 1];
        if (x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h) { return static_cast<int>(i - 1); }
        // 薄い装置は 1 マスの高さの記号でも選べる。物理矩形は変えない。
        if ((d.kind == "Plate" || d.kind == "Checkpoint") && x >= d.x && x < d.x + d.w && y >= d.y + d.h - 1 && y < d.y + d.h) { return static_cast<int>(i - 1); }
        if (d.kind == "OneWay" && x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + 1) { return static_cast<int>(i - 1); }
    }
    return -1;
}

bool DeviceEditor::Add(std::string_view kind, int col, int row, int cols, int rows) {
    if (!IsLoaded() || DeviceText::IndexOf(kDeviceKindNames, kind) < 0 || col < 0 || row < 0 || col >= cols || row >= rows) { return false; }
    DeviceDraft d; d.kind = kind; d.x = col; d.y = row;
    if (kind == "Door" || kind == "LockWall") { d.h = (std::min)(3, rows - row); }
    if (kind == "OneWay" || kind == "Bridge") { d.w = (std::min)(3, cols - col); }
    if (kind == "Laser") { d.direction = "Right"; }
    unsigned suffix = 1;
    do { d.id = "device_" + std::to_string(suffix++); }
    while (std::any_of(devices_.begin(), devices_.end(), [&d](const auto& item) { return item.id == d.id; }));
    devices_.push_back(std::move(d)); selected_ = static_cast<int>(devices_.size()) - 1;
    return true;
}

bool DeviceEditor::RemoveSelected() {
    if (!IsLoaded() || !HasSelection()) { return false; }
    const std::string id = devices_[selected_].id;
    devices_.erase(devices_.begin() + selected_); selected_ = -1;
    for (auto& d : devices_) { std::erase(d.targets, id); }
    return true;
}

bool DeviceEditor::DuplicateSelected(int cols, int rows) {
    if (!IsLoaded() || !HasSelection()) { return false; }
    auto copy = devices_[selected_];
    if (!Add(copy.kind, 0, 0, cols, rows)) { return false; }
    copy.id = Selected()->id; copy.targets.clear();
    copy.x = std::clamp(copy.x + 1, 0.0, (std::max)(0.0, cols - copy.w));
    copy.y = std::clamp(copy.y, 0.0, (std::max)(0.0, rows - copy.h));
    *Selected() = std::move(copy);
    return true;
}

bool DeviceEditor::Paste(const std::vector<DeviceDraft>& copied, double x, double y, int cols, int rows,
                         std::vector<std::string>& pastedIds) {
    if (!IsLoaded() || copied.empty()) { return false; }
    double left = copied.front().x, top = copied.front().y, right = left, bottom = top;
    for (const auto& d : copied) {
        left = (std::min)(left, d.x); top = (std::min)(top, d.y);
        right = (std::max)(right, d.x + d.w); bottom = (std::max)(bottom, d.y + d.h);
    }
    if (right - left > cols || bottom - top > rows) { return false; }
    x = std::clamp(x, 0.0, cols - (right - left)); y = std::clamp(y, 0.0, rows - (bottom - top));
    // ID は貼り付け先で振り直す。外への接続は切り、コピー内の接続だけ残す。
    const size_t first = devices_.size();
    pastedIds.clear();
    for (const auto& d : copied) {
        if (!Add(d.kind, 0, 0, cols, rows)) { devices_.resize(first); selected_ = -1; pastedIds.clear(); return false; }
        const std::string id = Selected()->id;
        *Selected() = d; Selected()->id = id; Selected()->x = d.x - left + x; Selected()->y = d.y - top + y; Selected()->targets.clear();
        pastedIds.push_back(id);
    }
    for (size_t i = 0; i < copied.size(); ++i) for (const auto& target : copied[i].targets) {
        const auto match = std::find_if(copied.begin(), copied.end(), [&target](const auto& d) { return d.id == target; });
        if (match != copied.end()) { devices_[first + i].targets.push_back(pastedIds[static_cast<size_t>(match - copied.begin())]); }
    }
    return true;
}

bool DeviceEditor::ToggleTarget(std::string_view sourceId, const std::string& target) {
    if (!IsLoaded() || ClassifyTarget(target) == TargetKind::Invalid) { return false; }
    auto source = std::find_if(devices_.begin(), devices_.end(), [sourceId](const auto& d) { return d.id == sourceId && d.kind == "Plate"; });
    if (source == devices_.end()) { return false; }
    const auto found = std::find(source->targets.begin(), source->targets.end(), target);
    if (found == source->targets.end()) { source->targets.push_back(target); }
    else { source->targets.erase(found); }
    return true;
}

bool DeviceEditor::ToggleLink(std::string_view sourceId, int targetIndex) {
    if (!IsLoaded() || targetIndex < 0 || targetIndex >= static_cast<int>(devices_.size())) { return false; }
    const auto& target = devices_[targetIndex];
    if (target.kind != "Door" && target.kind != "Laser" && target.kind != "Bridge") { return false; }
    auto source = std::find_if(devices_.begin(), devices_.end(), [sourceId](const auto& d) { return d.id == sourceId && d.kind == "Plate"; });
    if (source == devices_.end()) { return false; }
    const auto found = std::find(source->targets.begin(), source->targets.end(), target.id);
    if (found == source->targets.end()) { source->targets.push_back(target.id); }
    else { source->targets.erase(found); }
    return true;
}

std::string DeviceEditor::Serialize() const {
    std::ostringstream text; text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const auto& device : devices_) {
        text << device.id << ' ' << device.kind << ' ' << device.x << ' ' << device.y << ' ' << device.w << ' ' << device.h;
        if (!device.targets.empty()) {
            text << " targets=";
            for (size_t i = 0; i < device.targets.size(); ++i) { if (i) { text << ','; } text << device.targets[i]; }
        }
        // Inspector の文字列の追加容量は 256 バイトなので、既定値は書き出さない。
        if (device.direction != "Up") { text << " dir=" << device.direction; }
        if (device.range != 20) { text << " range=" << device.range; }
        if (device.inverted) { text << " invert=1"; }
        if (device.latch) { text << " latch=1"; }
        for (const auto& option : device.extra) { text << ' ' << option; }
        text << '\n';
    }
    return text.str();
}

std::string DeviceEditor::Validate(int cols, int rows) const {
    if (!IsLoaded()) { return error_; }
    std::set<std::string> ids;
    for (const auto& device : devices_) {
        if (!IsStableId(device.id) || !ids.insert(device.id).second) { return "机关 ID 须唯一，仅使用 ASCII 字母、数字、点、短横线和下划线。"; }
        if (!std::isfinite(device.x) || !std::isfinite(device.y) || !std::isfinite(device.w) || !std::isfinite(device.h) ||
            device.x < 0 || device.y < 0 || device.w <= 0 || device.h <= 0 || device.x + device.w > cols || device.y + device.h > rows) {
            return device.id + " 的矩形必须完整位于区块内。";
        }
        if (!std::isfinite(device.range) || device.range <= 0) { return device.id + " 的射程须大于零。"; }
    }
    // 同じテレビの目標は扉・レーザー・橋であること。別のテレビの装置と仕掛けは書式だけ確かめる（あるかどうかは世界地図で示す）。
    for (const auto& device : devices_) for (const auto& target : device.targets) {
        if (device.kind != "Plate") { return device.id + " 不是压板，不能连接目标。"; }
        const auto kind = ClassifyTarget(target);
        if (kind == TargetKind::Invalid) { return device.id + " 的连接目标格式无效：" + target; }
        if (kind != TargetKind::Local) { continue; }
        const auto item = std::find_if(devices_.begin(), devices_.end(), [&target](const auto& value) { return value.id == target; });
        if (item == devices_.end() || (item->kind != "Door" && item->kind != "Laser" && item->kind != "Bridge")) {
            return device.id + " 的本屏连接必须指向门、激光或伸缩桥。";
        }
    }
    return {};
}

}
