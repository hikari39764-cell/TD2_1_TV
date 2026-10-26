#pragma once
// 画面の外の仕掛けの 3D 表示。規則が決めた仕掛けの進みに合わせて、テレビ・柱・区域カメラ・信号線を動かす。
#include "TD2CameraDirector.h"
#include "TD2Mechanism.h"
#include "HIKARI/Project/WorldView.h"
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace TD2 {
// 規則へ渡す仕掛け。
struct MachineSources {
    std::vector<Rules::MechanismSource> mechanisms;
    std::vector<Rules::SlideSource> slides;
    std::vector<Rules::StackSource> stacks;
    std::vector<Rules::CarouselSource> carousels;
};

class MachineViews {
public:
    // 部品を読み、規則へ渡す仕掛けと 3D の動かし方を作る。位置は作者が置いた今の配置から求める。
    // 世界地図に置いていない回転柱の面は預け場所へ置く（regions の配置を書き換える）。
    MachineSources Sync(HIKARI::WorldView& world, const RegionObjects& objects, std::vector<Rules::RegionSource>& regions);
    // 仕掛けの進みを物体へ書く。操作中のテレビが動いたら、追従カメラの基準も同じだけ運ぶ。
    void Update(HIKARI::WorldView& world, Rules::Exploration& exploration, const RegionObjects& objects, CameraDirector& camera, double seconds);
    // 次の Update で、視野の外や止まっている物も含めて全部を今の場所へ書く（続きから戻した・世界を最初へ戻した時）。
    void Resync() noexcept { resync_ = true; }
    // 機構・回転柱で動いている部屋（操作中の部屋を除く）。画面は砂嵐にする（回る・落ちる間の覗き窓の角度の跳びと
    // 段の入れ替わりを見せない）。止まってから kSnowTail 秒は砂嵐のまま（信号が合うまで）。
    std::vector<std::string> SnowRooms() const;

private:
    bool resync_ = false;
    double clock_ = 0;                                          // Update に渡された秒の合計
    std::map<std::string, double, std::less<>> snowUntil_;     // 区域 ID → 砂嵐をやめる時刻（clock_）
    std::map<uint64_t, std::string> tvRooms_;                   // 部屋のテレビの物体 → 区域 ID
    using Transform = HIKARI::GameProjectTransform;
    using Handle = HIKARI::GameProjectObjectHandle;
    using Writes = std::vector<HIKARI::GameProjectWorldTransformWrite>;
    // 区域 ID → 作者が置いた配置（相対で書いた仕掛けを絶対へ直す基準）。
    using Placements = std::map<std::string, RegionPlacement, std::less<>>;

    // 機構で動かす物体の状態 0・1・-1（天秤のもう一方）の姿勢。pivot はその点を中心に回す。
    // from / to は全体の進みのうちこの物体が動く区間（作者の ~始め-終わり）。
    // reach は見えているかの判定に使う大きさ（m）、shown は最後に書いた姿勢。
    struct MechanismObject { uint64_t sceneId = 0; Handle handle{}; Transform a{}, b{}, c{}; bool pivot = false; HIKARI::GameProjectFloat3 center{}; float from = 0, to = 1, lift = 0, corner = 0; bool drop = false;
                             float reach = 1.5f; Transform shown{}; };
    // 動くテレビに付いて行く区域カメラ。turn は大きく回る転台（途中で向こう側へ切り替える）、reach はテレビまでの距離。
    // ride は乗り物（群れの外から自分の画面を狙う）、aim はテレビから見た狙う点、side は途中で膨らむ向き。
    struct MechanismAttachment { Handle handle{}; size_t parent = 0; Transform relative{}; bool turn = false, ride = false; float reach = 0; HIKARI::GameProjectFloat3 aim{}, side{}; };
    struct MechanismView {
        std::vector<MechanismObject> objects;
        std::vector<MechanismAttachment> attached;
        double written = -1;                    // 最後に進めた進み（変わった時だけ書く）
        double drawn = std::numeric_limits<double>::quiet_NaN();   // 最後に模型へ書いた進み（画の外にある間は書かずに置く）
        float cameraLift = 0, cameraPull = 0;   // 動きの途中だけカメラを持ち上げ・引く量（転台以外）
        float shake = 0;                        // 動き出す時と着いた時の画面の揺れ
    };
    // スロット棚・テレビの塔のメンバー。カメラはテレビの塔だけが一緒に動かす。
    struct Member { Handle tv{}, camera{}; Transform tvPose{}, cameraPose{}; };
    struct SlideView {
        std::vector<HIKARI::GameProjectFloat3> slots;   // スロットごとのテレビの位置
        std::vector<Member> members;
        int lastMoving = -1; uint64_t blockedCount = 0; int shaking = -1; double shake = 0;
    };
    struct StackView {
        std::vector<HIKARI::GameProjectFloat3> slots, docks;   // スロット・置き場ごとのテレビの位置
        std::vector<Member> members;
        int lastPhase = 0; uint64_t blockedCount = 0; double shake = -1;
    };
    // 回転柱と一緒に回る物（面のテレビ・柱の胴・回転盤）と、面に付いた区域カメラ。柱の軸のまわりに回す。
    struct CarouselPart { Handle handle{}, tv{}; Transform pose{}; bool camera = false; };
    struct CarouselView { HIKARI::GameProjectFloat3 pivot{}; size_t count = 4; std::vector<CarouselPart> parts; double written = -1; };
    // 信号線：つながった圧力板の通電で、材質の色と発光を変える。glow は今の光り具合（0 素 〜 1 点灯）。
    struct SignalCable { Handle handle{}; std::string plate; float glow = 0; };
    // メンバーの世界地図の位置とテレビの今の位置。
    struct MemberPlace { int x = 0, y = 0; HIKARI::GameProjectFloat3 position{}; };

    // 世界地図のどことも重ならない預け場所（抜いたテレビ・回転柱の行き止まり）。呼ぶたびに別の位置。
    std::pair<int, int> Park() { const int k = parked_++; return {-100000 - 128 * k, -100000}; }
    bool MemberPlaces(HIKARI::WorldView& world, const RegionObjects& objects, const Placements& placements,
                      const std::vector<std::string>& members, std::vector<MemberPlace>& out, std::string& missing) const;
    Member ReadMember(HIKARI::WorldView& world, const RegionObjects& objects, const std::string& region, bool withCamera) const;
    void SyncMechanisms(HIKARI::WorldView& world, const RegionObjects& objects, const Placements& placements, MachineSources& out);
    void SyncSlides(HIKARI::WorldView& world, const RegionObjects& objects, const Placements& placements, MachineSources& out);
    bool DeriveSlideSlots(Rules::SlideSource& source, const std::vector<MemberPlace>& members, const std::vector<std::pair<int, int>>& empty,
                          std::vector<HIKARI::GameProjectFloat3>& poses, std::string& error) const;
    void SyncStacks(HIKARI::WorldView& world, const RegionObjects& objects, const Placements& placements, MachineSources& out);
    void SyncCarousels(HIKARI::WorldView& world, const RegionObjects& objects, std::vector<Rules::RegionSource>& regions, MachineSources& out);
    void SyncCables(HIKARI::WorldView& world);
    // ゲームカメラの視野（描いた姿勢と操作中の姿勢）。取れない時は何でも見えている扱い。
    struct Sight {
        HIKARI::GameProjectCameraPose rendered{}, control{};
        bool hasRendered = false, hasControl = false;
        bool Sees(const HIKARI::GameProjectFloat3& center, float radius) const;
    };
    // 一フレームの更新で共有するもの：規則の状態、操作中のテレビ、運ぶ先のカメラ、視野。
    struct Frame { const Rules::Exploration& exploration; Handle activeTv; CameraDirector& camera; const Sight& sight; };
    // 物体の今の姿勢を覚え、操作中のテレビなら追従カメラの基準を前の姿勢から運ぶ。first は初めて動く時の元の姿勢。
    void Moved(const Frame& frame, uint64_t key, Handle tv, const Transform& first, const Transform& pose);
    void UpdateMechanisms(const Frame& frame, Writes& writes);
    void UpdateSlides(const Frame& frame, Writes& writes, double seconds);
    void UpdateStacks(const Frame& frame, Writes& writes, double seconds);
    void UpdateCarousels(const Frame& frame, Writes& writes);
    void UpdateCables(HIKARI::WorldView& world, Rules::Exploration& exploration, double seconds);

    std::vector<MechanismView> mechanisms_;
    std::vector<SlideView> slides_;
    std::vector<StackView> stacks_;
    std::vector<CarouselView> carousels_;
    std::vector<SignalCable> cables_;
    std::map<uint64_t, Transform> movedPose_;   // 動かした物体の今の姿勢
    int parked_ = 0;
};
}
