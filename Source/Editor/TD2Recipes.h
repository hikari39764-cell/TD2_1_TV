#pragma once
// 仕掛けの配方：よく使う機構（吊り台・転台・天平・往復・倒立）を、テレビを選んで地図で動いた先を決めるだけで作る。
// 3D の動き（物体のずれ）と部品（鎖・卷扬箱・转台…）は地図と今の 3D の配置から作り、普通の機構の欄に書く。
#include "TD2StageAttach.h"
#include "TD2StageTools.h"

namespace TD2::Editor {
enum class RecipeKind { Custom, Lift, Turn, Balance, Shuttle, Flip, Slide, Stack, Carousel };
struct RecipeInfo { RecipeKind kind; std::string_view id, label; int televisions; };
const std::vector<RecipeInfo>& RecipeList();
RecipeKind RecipeOf(const Machine&);
std::string RecipeLabel(const Machine&);
// 圧力板を仕掛けへつなぐ時の受け口（天平はどちら側か、电视塔は抽出か复位か）。
std::string PlateTargetFor(const Machine&, const std::string& plateRegion, const std::string& plateId, const std::string& devicesText);

// 新しい機構の初めの形。regions は押したテレビ（天平は二台）。
void InitRecipe(Machine& m, RecipeKind kind, const std::vector<std::string>& regions, const LayoutSnapshot& layout);
// 地図のずれ（状態 B・C）から 3D の動きを作り直す。配方の部品もテレビと一緒に動かす。作者が足した物体の行は残す。
void RebuildMotion(Machine& m, const LayoutSnapshot& layout);
// 配方の部品で、まだ置いていないもの。
struct RecipePart { std::string label, path, region, role; Pose pose; bool moves = false; };
std::vector<RecipePart> PlanParts(const Machine& m, const LayoutSnapshot& layout);
// 動く範囲に合わせて置き直す部品（卷扬箱の高さ・鎖の長さ）。
std::vector<std::pair<uint64_t, Pose>> AdjustParts(const Machine& m, const LayoutSnapshot& layout);

// 配方の調整値（転台の角度、往復の距離）。テレビの物体のずれから読み書きする。
double RecipeAngle(const Machine& m, const LayoutSnapshot& layout);
void SetRecipeAngle(Machine& m, const LayoutSnapshot& layout, double degrees);
double RecipeDistance(const Machine& m, const LayoutSnapshot& layout);
void SetRecipeDistance(Machine& m, const LayoutSnapshot& layout, double meters);
}
