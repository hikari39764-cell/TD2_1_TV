#pragma once
// 暗転の中のタイトル「ビジョン / Vision」。白い画に周りの碎片が炸裂し、标题から碎片が越喷越多に噴き出し、
// 最後に全部が巻き戻して吸い込まれ、ブラウン管のように消える。撕裂（乱れ）の瞬間は負片と赤青のずれ。
// 絵は全部が演出の秒と事件の時刻だけで決まる（前のフレームを覚えない）ので、演出ワークスペースのプレビューで時間軸を
// 動かしても Play と同じ絵になる。UI の画の上（後処理の後）、黒帯の下に描く。
#include "HIKARI/Project/WorldView.h"
#include <string_view>
#include <utility>
#include <vector>

namespace TD2 {
// タイトルカードの時刻（演出の秒）。事件が置かれていなければ呼び手がカードの範囲から既定の割合で埋める。
struct TitleCardTimes {
    double in = 0, seconds = 4.2;              // 点灯の始まりと、熄灭し終えるまでの長さ
    double burst = 0, burstSeconds = .48;      // 炸裂：周りの碎片が中心から飛び出して落ち着くまで
    double spray = 0, sprayEnd = 0;            // 噴出：标题から碎片が噴き出し始め、一番激しくなるまで
    double rewind = 0, rewindSeconds = .54;    // 巻き戻し：全部が中心へ吸い戻される
    std::vector<std::pair<double, double>> glitches;   // 乱れ（始まりと長さ）。一つごとに負片 1 コマ、あとは赤青のずれ
};
// 噴出の強さ（TD2.Intro の外観の値）。
struct TitleCardLook {
    float sprayPeak = 170;     // 一番激しい時の 1 秒あたりの数
    float spraySize = 1;       // 碎片の大きさに掛ける
    float spraySpeed = 1;      // 飛ぶ速さに掛ける
};

// 黒帯を退ける割合（0〜1）。カードが開いている間は 1（全画面の白い画にする）。
double TitleCardBarsAway(const TitleCardTimes& times, double t);
// 時刻 t のカードを描く。範囲の外なら何もしない。
void DrawTitleCard(HIKARI::WorldView& world, const TitleCardTimes& times, const TitleCardLook& look, double t);
// 使う貼り絵（最初の表示で引っかからないよう、タイトル待ちの間に読ませておく）。
const std::vector<std::string_view>& TitleCardTextures();
}
