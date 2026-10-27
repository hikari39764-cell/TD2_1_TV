#pragma once
// 弾窓：古いパソコンの「メッセージ」の窓を一つ描くのと、決まった乱数でだんだん速く増えていく弾窓の嵐。
// どちらも時刻だけで姿が決まる（同じ秒なら同じ絵）。描く先は TD2Ui の描き手（画の上でもテレビの画面の画布でも）。
#include "TD2Ui.h"
#include <cstdint>
#include <span>
#include <string_view>

namespace TD2::Popups {
inline constexpr std::string_view kIconQuestion = "UI/TD2/UI_Icon_Question.png";
inline constexpr std::string_view kIconWarning = "UI/TD2/UI_Icon_Warning.png";
inline constexpr std::string_view kIconMail = "UI/TD2/UI_Icon_MailNew.png";

// 一つの弾窓。text は「\n」で行を分ける。typed は打ち出した字の数（負なら全部）。
struct Look {
    std::string_view title = "メッセージ";
    std::string_view text;
    std::string_view icon;
    float appear = 1;      // 0〜1：出る途中（縦に伸びて少し行き過ぎる）
    float alpha = 1;
    double typed = -1;
    bool dark = false;     // 地を暗くした窓（嵐の中に時々混ぜる）
};
// 窓の幅 width で中心 (cx, cy) に描く。高さは行の数とボタンの段（buttons が true）から決める。
// 窓の中の、ボタンを置く段の矩形を返す（ボタンが無い時も最下段の矩形）。
Ui::Rect Draw(Ui::Painter& paint, float cx, float cy, float width, const Look& look, bool buttons = false);
// 弾窓の高さ（出きった時）。配置の計算に使う。
float Height(float width, std::string_view text, bool buttons);
// 出る動き：始まってからの秒 u で 0〜1（0.09 秒で縦に伸び、少し行き過ぎて戻る）。
float Appear(double u);

// 右寄せのボタンを並べる（labels の順に左から）。choice は選んでいる方、pressed は押した姿、hover は指している方（-1 で無し）。
// 各ボタンの矩形を rects へ返す。
void Buttons(Ui::Painter& paint, Ui::Rect area, std::span<const std::string_view> labels, int choice, bool pressed, int hover,
             std::span<Ui::Rect> rects);

// 弾窓の嵐。seconds の間にだんだん速く増え、終わりで peak 個になる。texts と titles から決まった乱数で選ぶ。
struct Storm {
    std::span<const std::string_view> texts;
    uint32_t seed = 1;
    double seconds = 2;
    int peak = 120;
    float size = 1;        // 窓の幅（area の幅に対する割合）に掛ける
};
// u 秒までに出た弾窓の数。
int Count(const Storm& storm, double u);
// u 秒の嵐の姿。tear は横に裂ける揺れの強さ（0 で無し）。新しい物ほど上に重なる（古い物は数を絞って描く）。
void DrawStorm(Ui::Painter& paint, Ui::Rect area, const Storm& storm, double u, float tear = 0);
}
