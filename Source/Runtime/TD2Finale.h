#pragma once
// 通関の弾窓：ゴールの扉が閉じてゴールのテレビが消えた後、その画面に「クリア おめでとう！」の弾窓が溢れ、画全体へ
// はみ出して（画面の枠を破って）埋め尽くす。その上に一つずつ問いかけの弾窓が出る。
// ・普通の通関：はい を選ぶと「アハハハ」の嵐の後に眠らされ、目覚めてタイトルへ。いいえ を選び続けると「マウスをロックした」
//   問いになる。窓の矢印（ゲームの中のカーソル）は はい から動かないが、その時だけ見える本当のマウスで いいえ を押すと隠しの道へ。
// ・隠しの道の通関：語りかけの弾窓が続いた後に眠らされ、目覚めると机のパソコンだけが残っている。
// 描くのは画の上（UiView）と、ゴールのテレビの画面の画布（MenuDirector が持つ）。時刻は IntroDirector の通関の秒。
#include "TD2Menus.h"
#include "TD2Ui.h"
#include "HIKARI/Project/WorldView.h"
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace TD2 {
class FinaleDirector {
public:
    enum class Kind { Ending, TrueEnding };
    // 終わり方：目覚めてタイトルへ・隠しの道へ移る・隠しの道を通した目覚め。
    enum class Outcome { None, WakeNormal, EnterHidden, WakeTrue };
    // ゴールの扉に入った時（通関の秒の 0）。goalScreen はゴールのテレビ。
    void Begin(Kind kind, HIKARI::GameProjectObjectHandle goalScreen);
    bool Active() const noexcept { return active_; }
    // 通関の毎フレーム。t は通関の秒。暗転を始めたフレームに blackOut を true にする（呼び手が IntroDirector へ伝える）。
    Outcome Update(HIKARI::WorldView& world, MenuDirector& menu, double t, double seconds, bool& blackOut);
    // 新しく出た弾窓の数（このフレームまでの分。読むと 0 に戻る）。効果音に使う。
    int TakeSounds() { return std::exchange(sounds_, 0); }

    // 問いかけの一つ。buttons の数が 0 なら、hold 秒（スペースでも）で次へ。答えの先は、次の問い・「アハハハ」・隠しの道。
    enum class Next { Line, Laugh, Hidden };
    struct Line {
        std::string_view text;
        std::array<std::string_view, 2> buttons{};
        int buttonCount = 0;
        double hold = 0;
        std::string_view icon;
        bool locked = false;          // 窓の矢印が はい から動かない（本当のマウスでだけ いいえ を押せる）
        Next yes = Next::Line, no = Next::Line;
        bool shake = false;           // 出ている間、山ごと震える
    };

private:
    // 画に残っている弾窓（答えた後も山の中に残す）。line は表の項目。
    struct Shown { const Line* line = nullptr; float cx = 0, cy = 0; double born = 0; int answered = -1; };
    void StartLines(const Line* lines, size_t count, double t);
    void Draw(HIKARI::WorldView& world, MenuDirector& menu, double t, double seconds);
    void Answer(int choice, double t);

    bool active_ = false;
    Kind kind_ = Kind::Ending;
    HIKARI::GameProjectObjectHandle goal_{};
    const Line* lines_ = nullptr;
    size_t lineCount_ = 0, current_ = 0;
    double lineStart_ = 0;           // 今の問いが出た時刻（通関の秒）
    double laughStart_ = -1;         // 「アハハハ」の嵐の始まり（無ければ負）
    double blackStart_ = -1;         // 暗転の始まり（無ければ負）
    Outcome pending_ = Outcome::None;
    std::vector<Shown> shown_;
    int choice_ = 0;
    float cursor_ = 0, lockShake_ = 0;
    bool pointerDown_ = false;
    int sounds_ = 0;
    std::array<Ui::Rect, 2> buttonRects_{};
};
}
