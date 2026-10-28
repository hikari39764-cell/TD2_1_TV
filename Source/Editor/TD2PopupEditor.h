#pragma once
// 弾窓：テレビの中に出る弾窓の台本を直すワークスペース。左は一覧、中央は選んだ弾窓の見本と属性、右は頻度の設定。
// データはシーンの TD2.PopupScript（「弾窓」欄は Common/TD2PopupText.h の書式）。書き込みはシーンの取り消しに一回ずつ積む。
#include "TD2EditorSupport.h"
#include "../Common/TD2PopupText.h"
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace TD2::Editor::Popup {
inline constexpr const char* kPopupWorkspace = "td2.popup_editor";

// 一覧の絞り込み。おまけは確率のある言葉。
enum class PopupFilter { All, Random, Mood, Event, Room, Egg };

// シーンから読んだ台本。比べるのは物体・原文・設定だけ（言葉は原文から読む）。
struct ScriptSnapshot {
    RegionTarget target;          // 台本の物体と部品（無ければ 0）
    size_t count = 0;             // シーンにある台本の数
    std::string text;             // 「弾窓」欄の原文
    std::vector<PopupEntry> entries;
    std::string parseError;       // 読めない行があればその理由
    PopupSettings settings;
    bool operator==(const ScriptSnapshot& o) const { return target == o.target && text == o.text && settings == o.settings; }
};

class PopupEditor final : public IEditorWorkspace {
public:
    PopupEditor() = default;
    EditorWorkspaceDescriptor GetDescriptor() const override;
    // レベル編集と同じシーンを直すので、行き来で保存を問わない。
    std::string_view GetDocumentIdentity() const override { return "td2.scene.maps"; }
    void OnActivate(EditorWorkspaceHostContext& ctx) override;
    void OnDeactivate(EditorWorkspaceHostContext& ctx) override;
    void Draw(EditorWorkspaceHostContext& ctx) override;
    bool IsDirty() const override;
    EditorWorkspaceSaveResult Save(EditorWorkspaceHostContext& ctx) override;
    void DiscardChanges(EditorWorkspaceHostContext&) override { DropDraft(); error_.clear(); }
    EditorWorkspaceCloseDisposition QueryClose(EditorWorkspaceCloseReason) const override {
        return IsDirty() ? EditorWorkspaceCloseDisposition::RequiresSave : EditorWorkspaceCloseDisposition::Allow;
    }

private:
    // ---- シーンとのやり取り
    bool Pulse(EditorWorkspaceHostContext& ctx, bool force);
    void Poll(EditorWorkspaceHostContext& ctx);
    bool Read(EditorWorkspaceHostContext& ctx, ScriptSnapshot& out, std::string& error);
    void Adopt(ScriptSnapshot fresh);
    void DropDraft();
    void LoadRegions(EditorWorkspaceHostContext& ctx);
    bool Commit(EditorWorkspaceHostContext& ctx);
    bool Create(EditorWorkspaceHostContext& ctx);
    void Publish(EditorWorkspaceHostContext& ctx);

    // ---- 草稿
    // 台本があり、全部の行が読めている（読めない時は原文だけを直す）。
    bool Editable() const { return loaded_ && read_.target.objectId && read_.parseError.empty(); }
    bool HasSelection() const { return selected_ >= 0 && selected_ < static_cast<int>(entries_.size()); }
    void NormalizeDraft();
    void Validate();
    void MarkPending() { pending_ = true; ++version_; error_.clear(); }
    void Select(int index);
    std::string UniqueId(std::string base, int first) const;
    void AddEntry();
    void DuplicateEntry();
    void DeleteEntry();

    // ---- 欄
    void DrawList(EditorWorkspaceHostContext& ctx);
    void DrawEntry(EditorWorkspaceHostContext& ctx);
    void DrawRaw(EditorWorkspaceHostContext& ctx);
    void DrawSettings(EditorWorkspaceHostContext& ctx);

    ScriptSnapshot read_;                 // 最後に読んだ姿
    std::vector<PopupEntry> entries_;     // 草稿の言葉
    PopupSettings settings_;              // 草稿の設定
    std::string raw_;                     // 読めない時に直す原文
    std::set<std::string, std::less<>> regions_;   // シーンにある区域 ID
    PopupFilter filter_ = PopupFilter::All;
    int selected_ = -1, historyFrames_ = 0, problemIndex_ = -1;
    // serial_ は選んだ弾窓が変わるたびに進め、入力欄の ID に入れる（別の弾窓へ打ち込まない）。
    uint64_t serial_ = 0, version_ = 1, validated_ = 0, normalized_ = 0;
    bool loaded_ = false, stale_ = true, pending_ = false, confirmDelete_ = false, regionsLoaded_ = false, regionsOk_ = false;
    float clock_ = 1, elapsed_ = 0;
    std::string mark_, problem_, error_, readError_, published_;
};

std::vector<std::unique_ptr<IEditorWorkspace>> MakePopupWorkspaces();
}
