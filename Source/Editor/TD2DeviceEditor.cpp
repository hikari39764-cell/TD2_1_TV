#include "TD2DeviceEditor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace TD2::Editor {
namespace {
constexpr std::array<std::string_view, 8> kKinds{"Solid", "OneWay", "Plate", "Door", "Laser", "Spikes", "Checkpoint", "LockWall"};
constexpr std::array<std::string_view, 4> kDirections{"Up", "Down", "Left", "Right"};
constexpr std::array<std::string_view, 8> kLabels{"实体块", "单向平台", "压板", "门", "激光", "尖刺", "存档点", "锁墙"};
bool StableId(std::string_view id) {
    return !id.empty() && id.size() <= 95 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}
}

void DeviceEditor::Load(std::string_view text) {
    const std::string selectedId = HasSelection() ? devices_[selected_].id : "";
    devices_.clear(); selected_ = -1; error_.clear();
    std::istringstream input{std::string(text)};
    std::string line;
    unsigned lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (const auto comment = line.find('#'); comment != std::string::npos) { line.resize(comment); }
        std::istringstream values(line); values.imbue(std::locale::classic());
        DeviceDraft device;
        if (!(values >> device.id)) { continue; }
        const auto invalid = [this, lineNumber] {
            error_ = "设备第 " + std::to_string(lineNumber) + " 行格式无效；原文保留，未覆盖场景。";
        };
        if (!(values >> device.kind >> device.x >> device.y >> device.w >> device.h) ||
            std::find(kKinds.begin(), kKinds.end(), device.kind) == kKinds.end()) { invalid(); return; }
        std::string option;
        while (values >> option) {
            const size_t separator = option.find('=');
            if (separator == std::string::npos) { invalid(); return; }
            const auto key = option.substr(0, separator), value = option.substr(separator + 1);
            if (key == "targets") {
                if (value.empty() || value.back() == ',') { invalid(); return; }
                std::istringstream ids(value); std::string id;
                while (std::getline(ids, id, ',')) {
                    if (id.empty()) { invalid(); return; }
                    device.targets.push_back(id);
                }
            } else if (key == "dir") {
                if (std::find(kDirections.begin(), kDirections.end(), value) == kDirections.end()) { invalid(); return; }
                device.direction = value;
            } else if (key == "range") {
                std::istringstream number(value); number.imbue(std::locale::classic());
                if (!(number >> device.range) || !number.eof() || !std::isfinite(device.range)) { invalid(); return; }
            } else if (key == "invert") {
                if (value != "0" && value != "1") { invalid(); return; }
                device.inverted = value == "1";
            } else { invalid(); return; }
        }
        if (device.id == selectedId) { selected_ = static_cast<int>(devices_.size()); }
        devices_.push_back(std::move(device));
    }
}

int DeviceEditor::Hit(double x, double y) const {
    for (size_t i = devices_.size(); i > 0; --i) {
        const auto& d = devices_[i - 1];
        if (x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h) { return static_cast<int>(i - 1); }
        // 薄い装置は表示用の一格高スプライトも選択できる。物理矩形そのものは変更しない。
        if ((d.kind == "Plate" || d.kind == "Checkpoint") && x >= d.x && x < d.x + d.w && y >= d.y + d.h - 1 && y < d.y + d.h) { return static_cast<int>(i - 1); }
        if (d.kind == "OneWay" && x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + 1) { return static_cast<int>(i - 1); }
    }
    return -1;
}

bool DeviceEditor::Add(std::string_view kind, int col, int row, int cols, int rows) {
    if (!IsLoaded() || std::find(kKinds.begin(), kKinds.end(), kind) == kKinds.end() || col < 0 || row < 0 || col >= cols || row >= rows) { return false; }
    DeviceDraft d; d.kind = kind; d.x = col; d.y = row;
    if (kind == "Door" || kind == "LockWall") { d.h = (std::min)(3, rows - row); }
    if (kind == "OneWay") { d.w = (std::min)(3, cols - col); }
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
    // 新 ID は配置先で採番する。外部 targets は切り、コピー内の対応だけを復元する。
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

bool DeviceEditor::ToggleLink(std::string_view sourceId, int targetIndex) {
    if (!IsLoaded() || targetIndex < 0 || targetIndex >= static_cast<int>(devices_.size())) { return false; }
    const auto& target = devices_[targetIndex];
    if (target.kind != "Door" && target.kind != "Laser") { return false; }
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
        // SDK std::string Inspector の追加容量は 256 bytes。既定値を冗長に出力して編集を拒否させない。
        if (device.direction != "Up") { text << " dir=" << device.direction; }
        if (device.range != 20) { text << " range=" << device.range; }
        if (device.inverted) { text << " invert=1"; }
        text << '\n';
    }
    return text.str();
}

std::string DeviceEditor::Validate(int cols, int rows) const {
    if (!IsLoaded()) { return error_; }
    std::set<std::string> ids;
    for (const auto& device : devices_) {
        if (!StableId(device.id) || !ids.insert(device.id).second) { return "机关 ID 须唯一，仅使用 ASCII 字母、数字、点、短横线和下划线。"; }
        if (!std::isfinite(device.x) || !std::isfinite(device.y) || !std::isfinite(device.w) || !std::isfinite(device.h) ||
            device.x < 0 || device.y < 0 || device.w <= 0 || device.h <= 0 || device.x + device.w > cols || device.y + device.h > rows) {
            return device.id + " 的矩形必须完整位于区块内。";
        }
        if (!std::isfinite(device.range) || device.range <= 0) { return device.id + " 的射程须大于零。"; }
    }
    for (const auto& device : devices_) for (const auto& target : device.targets) {
        const auto item = std::find_if(devices_.begin(), devices_.end(), [&target](const auto& value) { return value.id == target; });
        if (device.kind != "Plate" || item == devices_.end() || (item->kind != "Door" && item->kind != "Laser")) {
            return device.id + " 的连接必须指向本区块的门或激光。";
        }
    }
    return {};
}

} // namespace TD2::Editor
