#pragma once
#include "TD2EditorSupport.h"

namespace TD2::Editor {

// 編集用の表現だけを持つ。実行時の状態は持たない。
struct DeviceDraft {
    std::string id, kind = "Solid", direction = "Up";
    double x = 0, y = 0, w = 1, h = 1, range = 20;
    bool inverted = false, latch = false;
    std::vector<std::string> targets;
    // 種類ごとの追加の項目（バネの強さ・溜め・冷却、貼り絵の図案、元へ戻すボタンの形など）は「key=value」のまま持つ。
    std::vector<std::string> extra;
    // 追加の項目を読む・書く（空の値で消す）。
    std::string Extra(std::string_view key) const;
    void SetExtra(std::string_view key, std::string_view value);
};
// 圧力板の目標の種類：同じテレビの扉・レーザー・橋、別のテレビの装置（区域ID/装置ID）、画面の外の仕掛け（mech/・slide/・stack/・carousel/）。
enum class TargetKind { Local, OtherRegion, Machine, Invalid };
TargetKind ClassifyTarget(std::string_view target);

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
    // 目標の文字列をそのまま付け外しする（別のテレビの装置・仕掛け）。
    bool ToggleTarget(std::string_view sourceId, const std::string& target);
    bool IsLoaded() const { return error_.empty(); }
    const std::string& Error() const { return error_; }
private:
    std::vector<DeviceDraft> devices_;
    int selected_ = -1;
    std::string error_;
};

}
