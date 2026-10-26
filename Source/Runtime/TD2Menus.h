#pragma once
// 窓：起動時の続きの問い（画の上の全画面）、タイトル待ちのパソコンのメールと、遊んでいる間の ESC の窓（設定と
// ゲームをやめる）・やめる前の保存の問い（どちらもテレビの画面の画布へ描き、画面の絵として 3D で写す）。
// A・D（W・S）で選び、スペースで決める。描くのは TD2Ui、何を選んだかを返すだけで、続きの読み込み・オープニング・
// 終了はゲームのセッションが行う。設定は選んだ時に合わせ、保存の枠へ書く。
#include "TD2SaveData.h"
#include "TD2Ui.h"
#include "HIKARI/Project/WorldView.h"

namespace TD2 {
class MenuDirector {
public:
    enum class Choice { None, Yes, No };
    enum class PauseResult { None, Closed, Quit };

    // 起動の最初：設定を読み、音量と画面を合わせる（設定が無ければ今の画面の設定を写す）。
    void Start(HIKARI::WorldView& world);
    // 毎フレームの最初：キーの押下と長押しの繰り返しを読む。
    void ReadKeys(HIKARI::WorldView& world, double seconds);
    // 毎フレームの最後（窓を描いた後）：画布の更新を終える（このフレームに描かなかったら画面を放す）。窓の矢印の
    // カーソルを描いたなら、OS のマウスポインターを隠す（二つ並ばない）。
    void EndFrame(HIKARI::WorldView& world);
    // 画布を貼っているテレビの物体（ハンドルの値。無ければ 0）。舞台はこの画面を 3D の絵へ結ばない（放せば結び直す）。
    uint64_t CanvasScreen() const noexcept { return canvas_.screen.value; }
    // UI がスペースを使った後、離すまで遊びへ渡さない（決めた瞬間にダッシュや交換にならない）。
    bool HoldsAction() const noexcept { return holdAction_; }

    // 起動の直後の毎フレーム：窓・メール・ESC の窓で使う素材と字を見えないほど薄く描き、初めて出す時の読み込みを済ませておく。
    void Warm(HIKARI::WorldView& world, double seconds);
    // 起動時の続きの問い（全画面）。決めた後も 0.4 秒、閉じていく姿を描く（その下で遊びかタイトルが始まっている）。
    void OpenContinue() { mode_ = Mode::Continue; choice_ = 0; opened_ = 0; closing_ = -1; warm_ = warmReal_ = 0; stable_ = 0; }
    bool ContinueOpen() const noexcept { return mode_ == Mode::Continue; }
    Choice UpdateContinue(HIKARI::WorldView& world, double seconds);
    // 毎フレーム：閉じていく続きの問いと ESC の窓を描く。
    void DrawClosing(HIKARI::WorldView& world, double seconds);

    // タイトル待ちのパソコンのメール。screen はパソコンのテレビ（その画面に画布を貼って描く）、alpha は出方（通関の目覚め）。
    // interactive の時だけ選べる。はい は押した時に Yes（消える動きはその後も描く）、いいえ は消えて暗転し終えてから No。
    Choice UpdateTitle(HIKARI::WorldView& world, HIKARI::ObjectView screen, float alpha, bool interactive, double seconds);
    void ResetTitle() { titleChoice_ = 0; titleCursor_ = 0; leaving_ = -1; leavingQuit_ = false; }
    // 選んで消えていく途中か（はい ならオープニングの最初の間も描き続ける）。
    bool TitleLeaving() const noexcept { return leaving_ >= 0; }

    // ESC の窓。screen は今いる部屋のテレビ（その画面に画布を貼って描く）。canSave が false（保存しない遊び方）なら、
    // やめる時に保存を問わない。
    bool MenuPressed() const noexcept { return keys_.menu; }
    void OpenPause() { mode_ = Mode::Pause; row_ = 0; opened_ = 0; pauseClosing_ = -1; }
    bool PauseOpen() const noexcept { return mode_ == Mode::Pause || mode_ == Mode::Confirm || mode_ == Mode::Quitting; }
    PauseResult UpdatePause(HIKARI::WorldView& world, HIKARI::ObjectView screen, double seconds, bool canSave);
    // やめる時に保存するか（Quit を返した後に読む）。Quit は保存の問いに答えて暗転し終えた時に返す。
    bool QuitSave() const noexcept { return quitSave_; }

private:
    enum class Mode { None, Continue, Pause, Confirm, Quitting };
    struct Keys { bool left = false, right = false, up = false, down = false, confirm = false, menu = false; };
    struct Repeat { bool held = false; double time = 0; };
    // 長押しは 0.38 秒後から 0.09 秒ごとに繰り返す。
    static bool Step(Repeat& repeat, bool down, double seconds);
    void Apply(HIKARI::WorldView& world, bool display);
    void SaveSettings(HIKARI::WorldView& world);
    // はい・いいえ の二つのボタンとカーソル。choice は選んでいる方、cursor はカーソルの今の位置（0〜1、なめらかに追う）。
    void Buttons(Ui::Painter& paint, Ui::Rect area, int choice, float& cursor, double seconds, bool pressed);
    // 続きの問いの姿。alpha は全体、windowAlpha は窓だけ。pressed は決めた後（選んだボタンを押した姿）。
    void DrawContinue(Ui::Painter& paint, float alpha, float windowAlpha, double seconds, bool pressed);
    // ESC の窓の姿。appear は出方（0〜1、閉じる時は逆にたどる）。answered は保存の問いに答えた後（ボタンを押した姿）。
    void DrawPause(HIKARI::WorldView& world, HIKARI::ObjectView screen, float appear, bool confirming, double seconds, bool answered = false);
    // テレビの画面の画布：このフレームの更新を始め、その画面の材質へ結ぶ。region は画面の比に合わせた画布の中の描く範囲。
    // 画布が使えない時は false（呼び手は画の上へ平らに描く）。
    bool BeginCanvas(HIKARI::WorldView& world, HIKARI::ObjectView screen, Ui::Rect& region);
    struct Canvas {
        HIKARI::GameProjectUiCanvasHandle handle{};
        uint64_t revision = 0;
        HIKARI::GameProjectObjectHandle screen{};   // 今貼っている画面
        bool drawing = false, failed = false;
    };

    Mode mode_ = Mode::None;
    Keys keys_;
    Repeat leftRepeat_, rightRepeat_, upRepeat_, downRepeat_;
    bool holdAction_ = false;
    // このフレームに窓のカーソルを描いたか。最後に使った入力が手柄か（案内の字を替える）。
    bool cursorDrawn_ = false, gamepad_ = false;
    int choice_ = 0, row_ = 0, confirmChoice_ = 1;
    float cursor_ = 0, confirmCursor_ = 1, rowCursor_ = 0;
    double opened_ = 0, closing_ = -1, pauseClosing_ = -1, quitting_ = 0;
    // 続きの問いを開く前の待ち：進めた秒（1 フレーム 1/30 秒まで）・実際の秒・続けて短かったフレームの数。起動の後の経過秒。
    double warm_ = 0, warmReal_ = 0, warmTime_ = 0;
    int stable_ = 0;
    HIKARI::GameProjectObjectHandle pauseScreen_{};
    Canvas canvas_;
    // タイトル
    int titleChoice_ = 0;
    float titleCursor_ = 0;
    double leaving_ = -1, titleTime_ = 0;
    bool leavingQuit_ = false, quitReported_ = false, quitAsked_ = false;
    // 設定
    UserSettings settings_, saved_;
    bool settingsLoaded_ = false, quitSave_ = false;
    // 画面の設定を変えられるか（ゲームの窓で遊んでいる時だけ。エディタの中の Play では変えられない）。
    bool displayAvailable_ = false;
    // 画面の設定を変えてから合わせるまでの残り秒（0 なら待っていない）。
    double displayPending_ = 0;
};
}
