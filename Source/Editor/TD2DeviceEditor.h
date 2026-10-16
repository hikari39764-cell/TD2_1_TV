#pragma once
#include "TD2EditorSupport.h"

namespace TD2::Editor {

// 編集用の表現だけを持つ。実行時の状態は持たない。
struct DeviceDraft {
    std::string id, kind = "Solid", direction = "Up";
    double x = 0, y = 0, w = 1, h = 1, range = 20;
    bool inverted = false, latch = false;
    std::vector<std::string> targets;
    // 一覧で編集しない項目（バネの強さ、貼り絵の図案など）は原文のまま持ち、書き出しで戻す。
    std::vector<std::string> extra;
};

class DeviceEditor {
public:
    void Load(std::string_view text);
    std::string Serialize() const;
    std::string Validate(int cols, int rows) const;
    bool HasSelection() const { return selected_ >= 0 && selected_ < static_cast<int>(devices_.size()); }
    // キャンバスの選択・移動・接続は一覧と同じ草稿を使う。
    const std::vector<DeviceDraft>& Items() const { return devices_; }
    DeviceDraft* Selected() { return HasSelection() ? &devices_[selected_] : nullptr; }
    const DeviceDraft* Selected() const { return HasSelection() ? &devices_[selected_] : nullptr; }
    int SelectedIndex() const { return HasSelection() ? selected_ : -1; }
    int Hit(double x, double y) const;
    void Select(int index) { selected_ = index; }
    bool Add(std::string_view kind, int col, int row, int cols, int rows);
    bool RemoveSelected();
    bool DuplicateSelected(int cols, int rows);
    bool Paste(const std::vector<DeviceDraft>& copied, double x, double y, int cols, int rows,
               std::vector<std::string>& pastedIds);
    bool ToggleLink(std::string_view sourceId, int targetIndex);
    bool IsLoaded() const { return error_.empty(); }
    const std::string& Error() const { return error_; }
private:
    std::vector<DeviceDraft> devices_;
    int selected_ = -1;
    std::string error_;
};

}
