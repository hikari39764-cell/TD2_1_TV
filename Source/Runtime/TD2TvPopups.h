#pragma once
// 遊んでいる間にテレビの中へ出る弾窓（「？？？」からのメッセージ）。言葉ときっかけはシーンの台本（TD2.PopupScript）から
// 読む。画面の中の舞台の奥（人物・地形・装置より後ろ、後ろの壁より手前）に板を立て、板の材質へ画布を結んで窓を描く。
// 窓は空いたマス（地形・装置とその通り道・人物から離れた所）にだけ出し、前景を隠さない。きっかけは出来事（大区域に初めて
// 入る、チェックポイント、宝石、扉、機構、何度も死ぬ）、決まった部屋に初めて入った時、遊んでいる間の時々（その時の様子と
// おまけを含む）。演出・窓・大区域の切り替えの間は出さない。
// 板はシーンに置いた三枚（TD2.PopupBoard）を使い回し、使わない板は舞台の外へ退ける（板を出し入れすると GPU のシーンが
// 作り直しになる）。舞台が板を置き、このクラスは中身と時間を決める。
#include "Rules/TD2Exploration.h"
#include "../Common/TD2PopupText.h"
#include "HIKARI/Project/GameComponent.h"
#include "HIKARI/Project/UiCanvasTypes.h"
#include "HIKARI/Project/WorldView.h"
#include <array>
#include <deque>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TD2 {
inline constexpr std::string_view kPopupBoardComponentType = "TD2.PopupBoard";

// 弾窓の板の印。モデルは白い板（SM_TD2_StaticWhite）、材質の枠は TD2_StaticWhite。
class PopupBoardComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields&) override {}
};

// 弾窓の台本：言葉ときっかけ（「弾窓」欄、書式は Common/TD2PopupText.h）と頻度の決まり。シーンに一つ置く（無ければ
// 既定の台本で出す）。編集は「弾窓」工作区で行う。欄の ID はエディタが読み書きするので変えない。
class PopupScriptComponent final : public HIKARI::IGameComponent {
public:
    const std::string& Entries() const noexcept { return entries_; }
    const PopupSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        auto& s = settings_;
        fields.Float("firstDelay", "最初まで", s.firstDelay, 0, 600);
        fields.Float("intervalMax", "間隔の最長", s.intervalMax, 1, 600);
        fields.Float("intervalMin", "間隔の最短", s.intervalMin, 1, 600);
        fields.Float("intervalPerArea", "大区域ごとに縮める", s.intervalPerArea, 0, 120);
        fields.Float("moodChance", "様子の確率", s.moodChance, 0, 1);
        fields.Float("moodCooldown", "様子の間", s.moodCooldown, 0, 1800);
        fields.Float("idleSeconds", "止まったまま", s.idleSeconds, 1, 120);
        fields.Float("diedSeconds", "死んだすぐ後", s.diedSeconds, 1, 120);
        fields.Float("stuckSeconds", "長居", s.stuckSeconds, 5, 1800);
        fields.Float("eventGap", "出来事の間", s.eventGap, 0, 600);
        fields.Int("history", "重ねない数", s.history, 0, 60);
        fields.Int("deathsToWorry", "心配する死の数", s.deathsToWorry, 1, 99);
        fields.Bool("checkpointPerArea", "チェックポイントは大区域ごと", s.checkpointPerArea);
        fields.Bool("mechanismPerArea", "機構は大区域ごと", s.mechanismPerArea);
        fields.Bool("echoPerArea", "残影の様子は大区域ごと", s.echoPerArea);
        // 長い欄：Inspector の既定の追加容量（256 バイト）では足りないので広げる（256 KiB まで）。
        if (!fields.IsInspecting()) { fields.Text("entries", "弾窓", entries_); return; }
        std::string buffer = entries_;
        buffer.resize((std::max)(entries_.size(), size_t{256u * 1024u}) + 1u, '\0');
        if (fields.Text("entries", "弾窓", HIKARI::GameProjectTextBuffer(buffer.data(), buffer.size()))) entries_.assign(buffer.c_str());
    }
private:
    std::string entries_{PopupText::DefaultScript()};
    PopupSettings settings_;
};

// 板一枚の置き場所。local は部屋の左上からのマス（規則の向き。回された部屋では舞台が点対称に写す）。
struct TvPopupBoard {
    HIKARI::GameProjectObjectHandle object{};
    std::string region;
    Rules::Rect local{};
    bool shown = false;
};

// このフレームの遊びの様子。allowed は新しい弾窓を出してよいか（遊んでいて、演出・窓・切り替えの最中でない）。
// visible は出ている弾窓を見せてよいか（通関の演出などでは全部退ける）。areaKey は大区域の印（同じ大区域なら同じ値）。
struct TvPopupContext {
    const Rules::Exploration* exploration = nullptr;
    const Rules::WorldSnapshot* snapshot = nullptr;
    std::string_view areaKey;
    std::vector<std::string> snowRooms;
    bool allowed = false, visible = true;
    double seconds = 0;
};

class TvPopupDirector {
public:
    // 規則の出来事（区域 ID 付き）。Update の前に、そのフレームの分を渡す。
    void OnEvent(std::string_view region, const Rules::RuleEvent& event);
    void Update(HIKARI::WorldView& world, const TvPopupContext& context);
    // 舞台が読む板の置き場所（三枚。使わない板は shown が false）。
    const std::vector<TvPopupBoard>& Boards() const noexcept { return boards_; }
    // 出た弾窓の音を鳴らす数（読むと 0 に戻る）。出来事の弾窓も時々の弾窓も鳴らす。
    int TakeSounds() { return std::exchange(sounds_, 0); }
    // 一度だけの弾窓のうち出したもの（保存に残す）と、続きから始めた時に戻す。
    std::vector<std::string> Said() const { return {said_.begin(), said_.end()}; }
    void Restore(const std::vector<std::string>& said);
    // 世界を最初へ戻した時：出ている弾窓と、一度だけの記録・数え方を全部消す。
    void Reset();

private:
    static constexpr size_t kNone = static_cast<size_t>(-1);
    // 出す順を待つ弾窓：出来事（event の言葉から、最初に出せる時に選んで entry に覚える）か、部屋の言葉（entry と room）。
    // important は出来事どうしの間を空けない。
    struct Line {
        size_t entry = kNone;
        int event = -1;
        bool important = false;
        double queued = 0;
        std::string room;
    };
    struct Shown {
        size_t board = 0;
        std::string title, text, icon, region;
        Rules::Rect local{};
        double born = 0, life = 3;
        bool flipped = false;   // local を測った時に部屋が 180° 回されていたか
        int guide = -1;         // 操作の案内（PopupEvent の Move・Dash・Swap）。覚えるまで閉じない
    };
    bool Acquire(HIKARI::WorldView& world);
    void LoadScript(HIKARI::WorldView& world);
    // 台本の行が今の部屋で出せるか（この部屋だけの決まりと、一度だけの決まり）。最近出た言葉か。
    bool Eligible(const PopupEntry& entry, std::string_view region) const;
    bool Recent(std::string_view text) const;
    // 候補（台本の行の番号）から、組 pool ごとに一巡するまで重ねずに一つ選ぶ。候補が無ければ kNone。
    size_t Pick(const std::string& pool, const std::vector<size_t>& candidates);
    // 出来事の弾窓を待たせる。objectKey は一つの物に一度、areaKey は大区域に一度の鍵（空なら無し）。
    void QueueEvent(PopupEvent event, const std::string& objectKey, const std::string& areaKey, bool important, std::string_view region);
    // 今の部屋で窓を出せる所を探す（無ければ false）。
    bool FindSpot(const TvPopupContext& context, const std::string& region, Rules::Rect& out);
    void Paint(HIKARI::WorldView& world);

    std::vector<TvPopupBoard> boards_;
    std::vector<Shown> shown_;
    std::vector<Line> queue_;
    std::vector<PopupEntry> entries_;
    PopupSettings settings_;
    std::set<std::string, std::less<>> said_, echoAreas_;
    std::map<std::string, int, std::less<>> deaths_;
    std::vector<std::pair<PopupEvent, std::string>> pending_;   // このフレームの規則の出来事（種類と物の ID）
    std::string lastArea_, lastRoom_, enteredRoom_;
    std::map<std::string, std::vector<size_t>, std::less<>> bags_;   // 組ごとの、まだ出していない行の番号
    std::deque<std::string> history_;                               // 最近出た言葉
    std::array<double, 4> moodLast_{};                              // 様子ごとに最後に出した遊んだ秒
    // その時の様子：止まっている秒、同じ部屋にいる秒、最後に死んだ遊んだ秒。最後に出来事の弾窓を出した遊んだ秒。
    double idleTime_ = 0, roomTime_ = 0, lastDeath_ = -1e9, lastEvent_ = -1e9;
    // 操作を覚えたかを確かめる：歩いた秒、このフレームにダッシュした・入れ替えたか（規則の出来事から）。
    double walkTime_ = 0;
    bool dashed_ = false, swapped_ = false;
    HIKARI::GameProjectUiCanvasHandle canvas_{};
    uint64_t revision_ = 0;
    bool acquired_ = false, failed_ = false, dirty_ = false;
    // clock_ は弾窓の動きの秒（止めている間は進まない）、playTime_ は遊んだ秒（時々の弾窓を数える）。
    double clock_ = 0, playTime_ = 0, nextRandom_ = 45;
    uint32_t seed_ = 0x5EED1u, picks_ = 0;
    int sounds_ = 0;
};
}
