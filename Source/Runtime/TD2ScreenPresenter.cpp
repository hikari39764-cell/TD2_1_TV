#include "TD2ScreenPresenter.h"
#include "../Common/TD2SpritePalette.h"
#include "TD2RegionComponent.h"
#include "HIKARI/Project/ComponentViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TD2 {
namespace {

struct SpriteDraw {
    std::string texturePath;
    HIKARI::GameProjectUiSpriteRequest request{};
};

// フレーム PNG のパスと SDK 描画リクエストを一組にし、同じ Canvas へ順番に送る。
HIKARI::GameProjectUiColor UiTint(RegionColor color, float alphaScale = 1.0f) {
    return {color.r, color.g, color.b, color.a * alphaScale};
}

void AddFrame(std::vector<SpriteDraw>& draws, const RegionDefinition& region, std::string_view frame,
              float x, float y, float width, float height, RegionColor tint = SpritePalette::kSpriteTint) {
    SpriteDraw draw;
    draw.texturePath = SpritePalette::FramePath(frame);
    auto& sprite = draw.request;
    sprite.sourceId = static_cast<uint64_t>(draws.size()) + 1u;
    sprite.order = static_cast<int32_t>(draws.size());
    sprite.referenceWidth = static_cast<float>(region.cols) * static_cast<float>(region.tilePixels);
    sprite.referenceHeight = static_cast<float>(region.rows) * static_cast<float>(region.tilePixels);
    sprite.x = x;
    sprite.y = y;
    sprite.width = width;
    sprite.height = height;
    sprite.tint = UiTint(tint);
    draws.push_back(std::move(draw));
}

// 部品をタイル単位で反復する時の枚数を数え、画布上限の事前検査にも同じ値を使う。
size_t ExtentTiles(double extent) {
    if (!std::isfinite(extent) || extent <= 0.0) { return 0u; }
    const double count = std::ceil(extent);
    if (count >= static_cast<double>((std::numeric_limits<size_t>::max)())) {
        return (std::numeric_limits<size_t>::max)();
    }
    return static_cast<size_t>(count);
}

size_t SaturatingAdd(size_t a, size_t b) {
    if (b > (std::numeric_limits<size_t>::max)() - a) { return (std::numeric_limits<size_t>::max)(); }
    return a + b;
}

size_t SaturatingMultiply(size_t a, size_t b) {
    if (a != 0u && b > (std::numeric_limits<size_t>::max)() / a) {
        return (std::numeric_limits<size_t>::max)();
    }
    return a * b;
}

size_t BeamDrawCount(const Rules::Rect& rect) {
    return ExtentTiles((std::max)(rect.w, rect.h));
}

size_t DeviceDrawCount(Rules::DeviceKind kind, const Rules::Rect& rect) {
    if (kind == Rules::DeviceKind::Solid) {
        return SaturatingMultiply(ExtentTiles(rect.w), ExtentTiles(rect.h));
    }
    if (kind == Rules::DeviceKind::OneWay) { return (std::max)(size_t{1}, ExtentTiles(rect.w)); }
    return 1u;
}

bool Occupied(const RegionDefinition& region, int col, int row) {
    if (col < 0 || row < 0 || col >= region.cols || row >= region.rows) { return false; }
    return region.cells[CellIndex(region, col, row)] == '1';
}

// 隣接地形の露出辺から単一フレームを選び、地形の穴や空セルは塗り潰さない。
std::string TerrainFrame(const RegionDefinition& region, int col, int row) {
    const bool top = !Occupied(region, col, row - 1);
    const bool bottom = !Occupied(region, col, row + 1);
    const bool left = !Occupied(region, col - 1, row);
    const bool right = !Occupied(region, col + 1, row);
    if (top && left) { return "terrain.top_left"; }
    if (top && right) { return "terrain.top_right"; }
    if (bottom && left) { return "terrain.bottom_left"; }
    if (bottom && right) { return "terrain.bottom_right"; }
    if (top) { return "terrain.top"; }
    if (bottom) { return "terrain.bottom"; }
    if (left) { return "terrain.left"; }
    if (right) { return "terrain.right"; }
    return "terrain.fill";
}

std::string_view DirectionName(Rules::Direction direction) {
    switch (direction) {
    case Rules::Direction::Up: return "up";
    case Rules::Direction::Down: return "down";
    case Rules::Direction::Left: return "left";
    case Rules::Direction::Right: return "right";
    }
    return "up";
}

void DrawSolidDevice(std::vector<SpriteDraw>& draws, const RegionDefinition& region,
                     const Rules::Rect& rect, float tile) {
    for (double y = 0.0; y < rect.h; y += 1.0) {
        const double height = (std::min)(1.0, rect.h - y);
        for (double x = 0.0; x < rect.w; x += 1.0) {
            const double width = (std::min)(1.0, rect.w - x);
            AddFrame(draws, region, "terrain.fill",
                static_cast<float>((rect.x + x) * tile), static_cast<float>((rect.y + y) * tile),
                static_cast<float>(width * tile), static_cast<float>(height * tile), SpritePalette::kSpriteTint);
        }
    }
}

void DrawPlatform(std::vector<SpriteDraw>& draws, const RegionDefinition& region,
                  const Rules::Rect& rect, float tile) {
    // 端と中央を一枚ずつ組み、幅の端数だけを端部へ収めて一つの画像を横伸ばししない。
    const float x = static_cast<float>(rect.x) * tile;
    const float y = static_cast<float>(rect.y) * tile;
    const float width = static_cast<float>(rect.w) * tile;
    if (rect.w <= 1.0) {
        AddFrame(draws, region, "platform.middle", x, y, width, tile);
    } else if (rect.w < 2.0) {
        const float half = width * 0.5f;
        AddFrame(draws, region, "platform.left", x, y, half, tile);
        AddFrame(draws, region, "platform.right", x + half, y, width - half, tile);
    } else {
        AddFrame(draws, region, "platform.left", x, y, tile, tile);
        const float rightStart = x + width - tile;
        for (float middleX = x + tile; middleX < rightStart; middleX += tile) {
            const float segmentWidth = (std::min)(tile, rightStart - middleX);
            AddFrame(draws, region, "platform.middle", middleX, y, segmentWidth, tile);
        }
        AddFrame(draws, region, "platform.right", rightStart, y, tile, tile);
    }
}

// DeviceKind の見た目だけを割り当てる。座標と当たり判定は Rules の矩形をそのまま使う。
void DrawDevice(std::vector<SpriteDraw>& draws, const RegionDefinition& region, float tile,
                Rules::DeviceKind kind, const Rules::Rect& rect, Rules::Direction direction,
                bool active, bool pressed, bool solid, bool checkpoint) {
    const float x = static_cast<float>(rect.x) * tile;
    const float y = static_cast<float>(rect.y) * tile;
    const float width = static_cast<float>(rect.w) * tile;
    const float height = static_cast<float>(rect.h) * tile;
    std::string frame;
    switch (kind) {
    case Rules::DeviceKind::Solid:
        DrawSolidDevice(draws, region, rect, tile);
        return;
    case Rules::DeviceKind::OneWay:
        DrawPlatform(draws, region, rect, tile);
        return;
    case Rules::DeviceKind::Plate:
        // センサーの薄い判定高へ縮小せず、作者矩形の底に 1 格高の絵を置く。
        frame = pressed || active ? "plate.down" : "plate.up";
        AddFrame(draws, region, frame, x, (static_cast<float>(rect.y + rect.h) - 1.0f) * tile,
                 width, tile);
        return;
    case Rules::DeviceKind::Door:
        frame = active || !solid ? "door.open" : "door.closed";
        break;
    case Rules::DeviceKind::Laser:
        frame = "laser." + std::string(DirectionName(direction)) + (active ? ".on" : ".off");
        break;
    case Rules::DeviceKind::Spikes:
        frame = "spikes." + std::string(DirectionName(direction));
        break;
    case Rules::DeviceKind::Checkpoint:
        frame = checkpoint ? "checkpoint.active" : "checkpoint.idle";
        AddFrame(draws, region, frame, x, (static_cast<float>(rect.y + rect.h) - 1.0f) * tile,
                 width, tile);
        return;
    case Rules::DeviceKind::LockWall:
        frame = active || !solid ? "lock.open" : "lock.closed";
        break;
    }
    AddFrame(draws, region, frame, x, y, width, height);
}

std::string FacingFrame(std::string_view stem, int facing) {
    std::string frame(stem);
    frame += facing < 0 ? ".left" : ".right";
    return frame;
}

int AnimationIndex(double seconds, uint32_t frameDurationMs, int frameCount) {
    if (frameCount <= 1 || frameDurationMs == 0u || !std::isfinite(seconds)) { return 0; }
    const double elapsedMs = (std::max)(0.0, seconds) * 1000.0;
    const auto step = static_cast<uint64_t>(elapsedMs / static_cast<double>(frameDurationMs));
    return static_cast<int>(step % static_cast<uint64_t>(frameCount));
}

int FeedbackFrameIndex(std::string_view family, double age) {
    // FX の進行はルール時計と切り離した realTime 上の短い固定フレーム列にする。
    uint32_t frameMs = 1u;
    int frameCount = 1;
    if (family == "dash") { frameMs = 35u; frameCount = 4; }
    else if (family == "landing") { frameMs = 45u; frameCount = 4; }
    else if (family == "plate") { frameMs = 40u; frameCount = 3; }
    else if (family == "door") { frameMs = 45u; frameCount = 3; }
    else if (family == "checkpoint") { frameMs = 75u; frameCount = 4; }
    else if (family == "death") { frameMs = 55u; frameCount = 4; }
    return AnimationIndex(age, frameMs, frameCount);
}

bool FeedbackIsActive(const ScreenPresentationFeedback& feedback, double now) {
    return feedback.duration > 0.0 && now >= feedback.startedAt &&
           now - feedback.startedAt < feedback.duration;
}

const ScreenPresentationFeedback* FindFeedback(const std::vector<ScreenPresentationFeedback>& feedback,
                                                std::string_view family, double now) {
    const auto found = std::find_if(feedback.begin(), feedback.end(), [&](const auto& item) {
        return item.family == family && FeedbackIsActive(item, now);
    });
    return found == feedback.end() ? nullptr : &*found;
}

const ScreenPresentationMotionSample* FindSampleAtOrBefore(
    const std::vector<ScreenPresentationMotionSample>& samples, double wantedTime) {
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        if (it->realTime <= wantedTime) { return &*it; }
    }
    return nullptr;
}

void AddFeedbackFrame(std::vector<SpriteDraw>& draws, const RegionDefinition& region,
                      const ScreenPresentationFeedback& feedback, const Rules::WorldSnapshot& snapshot,
                      double now, float tile) {
    // 描画は半透明の PNG 一枚に限り、位置・サイズを各効果の資料上のアンカーに合わせる。
    if (!FeedbackIsActive(feedback, now)) { return; }
    const double age = now - feedback.startedAt;
    int frameIndex = FeedbackFrameIndex(feedback.family, age);
    if (feedback.reversed) {
        const int count = feedback.family == "door" ? 3 : 1;
        frameIndex = count - 1 - frameIndex;
    }
    const std::string frameFamily = feedback.family == "landing" ? "land" : feedback.family;
    const std::string frame = "fx." + frameFamily + "." + std::to_string(frameIndex);
    const float progress = static_cast<float>(age / feedback.duration);
    const float alpha = (feedback.family == "dash" ? 0.60f :
        feedback.family == "landing" ? 0.55f : feedback.family == "plate" ? 0.60f :
        feedback.family == "door" ? 0.50f : feedback.family == "checkpoint" ? 0.60f : 0.85f) *
        (1.0f - progress);

    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    if (feedback.family == "dash") {
        const double facing = snapshot.player.facing < 0 ? -1.0 : 1.0;
        const double centerX = snapshot.presentationPlayer.x + snapshot.presentationPlayer.w * 0.5;
        const double centerY = snapshot.presentationPlayer.y + snapshot.presentationPlayer.h * 0.5;
        x = static_cast<float>((centerX - facing * 0.75 - 0.75) * tile);
        y = static_cast<float>((centerY - 0.325) * tile);
        width = 1.5f * tile;
        height = 0.65f * tile;
    } else if (feedback.family == "landing") {
        const double centerX = feedback.rect.x + feedback.rect.w * 0.5;
        const double feetY = feedback.rect.y + feedback.rect.h;
        width = 0.85f * tile;
        height = 0.50f * tile;
        x = static_cast<float>((centerX - 0.425) * tile);
        y = static_cast<float>((feetY - 0.50) * tile);
    } else if (feedback.family == "plate") {
        width = height = tile;
        x = static_cast<float>((feedback.rect.x + feedback.rect.w * 0.5 - 0.5) * tile);
        y = static_cast<float>((feedback.rect.y + feedback.rect.h - 1.0) * tile);
    } else if (feedback.family == "door") {
        x = static_cast<float>(feedback.rect.x * tile);
        y = static_cast<float>(feedback.rect.y * tile);
        width = static_cast<float>(feedback.rect.w * tile);
        height = static_cast<float>(feedback.rect.h * tile);
    } else if (feedback.family == "checkpoint") {
        width = height = 1.2f * tile;
        const double centerX = feedback.rect.x + feedback.rect.w * 0.5;
        const double centerY = feedback.rect.y + feedback.rect.h - 0.65;
        x = static_cast<float>((centerX - 0.6) * tile);
        y = static_cast<float>((centerY - 0.6) * tile);
    } else {
        width = height = 1.1f * tile;
        const double centerX = feedback.rect.x + feedback.rect.w * 0.5;
        const double centerY = feedback.rect.y + feedback.rect.h * 0.5;
        x = static_cast<float>((centerX - 0.55) * tile);
        y = static_cast<float>((centerY - 0.55) * tile);
    }
    RegionColor tint = SpritePalette::kSpriteTint;
    tint.a = alpha;
    AddFrame(draws, region, frame, x, y, width, height, tint);
}

// 先に全描画数を数え、SDK 上限を超える Canvas を途中まで更新しない。
size_t CountDraws(const Rules::WorldDefinition& definition, const Rules::WorldSnapshot* snapshot,
                  bool showPlayer, const std::vector<ScreenPresentationFeedback>& feedback,
                  const std::vector<ScreenPresentationMotionSample>& samples) {
    const auto& region = definition.region;
    size_t count = 0u;
    for (const char cell : region.cells) { if (cell == '1') { count = SaturatingAdd(count, 1u); } }
    if (snapshot != nullptr) {
        for (const auto& device : snapshot->devices) {
            count = SaturatingAdd(count, DeviceDrawCount(device.kind, device.rect));
        }
        for (const auto& beam : snapshot->beams) { count = SaturatingAdd(count, BeamDrawCount(beam.rect)); }
        if (snapshot->echo) { count = SaturatingAdd(count, 1u); }
        if (snapshot->pendingEcho) { count = SaturatingAdd(count, 1u); }
        if (showPlayer && snapshot->playerPresent && !snapshot->player.dead) { count = SaturatingAdd(count, 1u); }
        if (FindFeedback(feedback, "dash", snapshot->realTime) != nullptr) {
            count = SaturatingAdd(count, 1u);
            if (snapshot->playerPresent && snapshot->dashing &&
                snapshot->realTime - FindFeedback(feedback, "dash", snapshot->realTime)->startedAt < 0.10) {
                for (const double age : {0.035, 0.070}) {
                    if (FindSampleAtOrBefore(samples, snapshot->realTime - age) != nullptr) {
                        count = SaturatingAdd(count, 1u);
                    }
                }
            }
        }
        for (const auto& item : feedback) {
            if (item.family != "dash" && FeedbackIsActive(item, snapshot->realTime)) {
                count = SaturatingAdd(count, 1u);
            }
        }
    } else {
        for (const auto& device : definition.devices) {
            count = SaturatingAdd(count, DeviceDrawCount(device.kind, device.rect));
        }
        count = SaturatingAdd(count, 1u); // 編集状態の出生マーカー
    }
    return count;
}

void DrawBeam(std::vector<SpriteDraw>& draws, const RegionDefinition& region,
              const Rules::Rect& rect, float tile) {
    const bool horizontal = rect.w >= rect.h;
    if (horizontal) {
        for (double offset = 0.0; offset < rect.w; offset += 1.0) {
            const double width = (std::min)(1.0, rect.w - offset);
            AddFrame(draws, region, "laser.beam.horizontal",
                static_cast<float>((rect.x + offset) * tile), static_cast<float>(rect.y * tile),
                static_cast<float>(width * tile), static_cast<float>(rect.h * tile));
        }
    } else {
        for (double offset = 0.0; offset < rect.h; offset += 1.0) {
            const double height = (std::min)(1.0, rect.h - offset);
            AddFrame(draws, region, "laser.beam.vertical",
                static_cast<float>(rect.x * tile), static_cast<float>((rect.y + offset) * tile),
                static_cast<float>(rect.w * tile), static_cast<float>(height * tile));
        }
    }
}

std::vector<SpriteDraw> BuildDraws(const Rules::WorldDefinition& definition,
    const Rules::WorldSnapshot* snapshot, bool showPlayer,
    const std::vector<ScreenPresentationFeedback>& feedback,
    const std::vector<ScreenPresentationMotionSample>& samples,
    const std::optional<double>& pendingEchoStartedAt) {
    // 画面内ローカル座標を論理タイルへ変換し、表示用スプライト列だけを組み立てる。
    std::vector<SpriteDraw> draws;
    const auto& region = definition.region;
    const float tile = static_cast<float>(region.tilePixels);
    const float scale = tile / 64.0f;

    for (int row = 0; row < region.rows; ++row) {
        for (int col = 0; col < region.cols; ++col) {
            if (Occupied(region, col, row)) {
                AddFrame(draws, region, TerrainFrame(region, col, row),
                    static_cast<float>(col) * tile, static_cast<float>(row) * tile, tile, tile,
                    SpritePalette::kSpriteTint);
            }
        }
    }

    if (snapshot != nullptr) {
        // 変化から生じた控えめな演出を危険物と登場人物より後ろに置く。
        for (const auto& item : feedback) {
            AddFeedbackFrame(draws, region, item, *snapshot, snapshot->realTime, tile);
        }
        const auto* dash = FindFeedback(feedback, "dash", snapshot->realTime);
        if (dash != nullptr && snapshot->playerPresent && snapshot->dashing &&
            snapshot->realTime - dash->startedAt < 0.10) {
            for (const auto [age, alpha] : {std::pair{0.035, 0.24f}, std::pair{0.070, 0.12f}}) {
                const auto* sample = FindSampleAtOrBefore(samples, snapshot->realTime - age);
                if (sample == nullptr) { continue; }
                const std::string frame = FacingFrame("player.dash", sample->facing) + ".0";
                RegionColor tint = SpritePalette::kPendingSpriteTint;
                tint.a = alpha;
                AddFrame(draws, region, frame,
                    static_cast<float>((sample->rect.x - 12.0 / 64.0) * tile),
                    static_cast<float>((sample->rect.y - 8.0 / 64.0) * tile),
                    tile, tile, tint);
            }
        }

        for (const auto& device : snapshot->devices) {
            const bool checkpoint = device.kind == Rules::DeviceKind::Checkpoint &&
                (device.id == snapshot->checkpointId || device.active);
            DrawDevice(draws, region, tile, device.kind, device.rect, device.direction,
                       device.active, device.pressed, device.solid, checkpoint);
        }
        for (const auto& beam : snapshot->beams) { DrawBeam(draws, region, beam.rect, tile); }

        if (snapshot->echo) {
            const std::string frame = FacingFrame("echo.idle", snapshot->echo->facing);
            AddFrame(draws, region, frame,
                static_cast<float>((snapshot->echo->rect.x - 12.0 / 64.0) * tile),
                static_cast<float>(snapshot->echo->rect.y * tile), tile, tile,
                SpritePalette::kSpriteTint);
        }
        if (snapshot->pendingEcho) {
            const std::string stem = FacingFrame("echo.form", snapshot->player.facing);
            const double elapsed = pendingEchoStartedAt
                ? (std::max)(0.0, snapshot->realTime - *pendingEchoStartedAt) : 0.0;
            const int index = (std::min)(2, static_cast<int>(elapsed * 1000.0 / 55.0));
            AddFrame(draws, region, stem + "." + std::to_string(index),
                static_cast<float>((snapshot->pendingEcho->x - 12.0 / 64.0) * tile),
                static_cast<float>(snapshot->pendingEcho->y * tile), tile, tile,
                SpritePalette::kPendingSpriteTint);
        }
        if (showPlayer && snapshot->playerPresent && !snapshot->player.dead) {
            const int facing = snapshot->player.facing;
            std::string state;
            int frameCount = 1;
            uint32_t frameDuration = 0u;
            if (snapshot->dashing) { state = "dash"; frameDuration = 140u; }
            else if (const auto* landing = FindFeedback(feedback, "landing", snapshot->realTime);
                     landing != nullptr && snapshot->realTime - landing->startedAt < 0.090) {
                state = "land"; frameDuration = 90u;
            } else if (!snapshot->player.grounded) { state = "air"; }
            else if (std::abs(snapshot->player.walkVx) > 0.01) {
                state = "walk"; frameCount = 4; frameDuration = 110u;
            } else { state = "idle"; frameCount = 2; frameDuration = 500u; }
            const int frameIndex = AnimationIndex(snapshot->realTime, frameDuration, frameCount);
            const std::string frame = FacingFrame("player." + state, facing) + "." +
                std::to_string(frameIndex);
            AddFrame(draws, region, frame,
                static_cast<float>(snapshot->presentationPlayer.x * tile - 12.0 * scale),
                static_cast<float>(snapshot->presentationPlayer.y * tile - 8.0 * scale),
                tile, tile, SpritePalette::kSpriteTint);
        }
    } else {
        // 編集時は作者定義と出生印だけを表示し、規則状態や進行時計を生成しない。
        for (const auto& device : definition.devices) {
            DrawDevice(draws, region, tile, device.kind, device.rect, device.direction,
                       false, false, true, false);
        }
        AddFrame(draws, region, "spawn.marker",
            static_cast<float>(definition.spawn.x * tile - 12.0 * scale),
            static_cast<float>(definition.spawn.y * tile - 8.0 * scale),
            tile, tile, SpritePalette::kSpriteTint);
    }
    return draws;
}

void HashByte(uint64_t& hash, uint8_t value) {
    hash = (hash ^ value) * 1099511628211ull;
}

void HashString(uint64_t& hash, std::string_view value) {
    for (const unsigned char c : value) { HashByte(hash, c); }
    HashByte(hash, 0u);
}

void HashU32(uint64_t& hash, uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        HashByte(hash, static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

void HashU64(uint64_t& hash, uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u) {
        HashByte(hash, static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

void HashFloat(uint64_t& hash, float value) { HashU32(hash, std::bit_cast<uint32_t>(value)); }

uint64_t ContentHash(const std::vector<SpriteDraw>& draws) {
    // 同じ形でも別フレーム PNG なら再送するため、各テクスチャパスも必ずハッシュする。
    uint64_t hash = 14695981039346656037ull;
    for (const auto& draw : draws) {
        const auto& sprite = draw.request;
        HashString(hash, draw.texturePath);
        HashU64(hash, sprite.sourceId);
        HashU32(hash, static_cast<uint32_t>(sprite.order));
        HashU32(hash, sprite.flags);
        HashFloat(hash, sprite.referenceWidth); HashFloat(hash, sprite.referenceHeight);
        HashFloat(hash, sprite.x); HashFloat(hash, sprite.y);
        HashFloat(hash, sprite.width); HashFloat(hash, sprite.height);
        HashFloat(hash, sprite.pivotX); HashFloat(hash, sprite.pivotY);
        HashFloat(hash, sprite.rotationRadians); HashFloat(hash, sprite.fillAmount);
        HashFloat(hash, sprite.effectProgress); HashFloat(hash, sprite.effectIntensity);
        HashFloat(hash, sprite.timeSeconds); HashFloat(hash, sprite.directionX); HashFloat(hash, sprite.directionY);
        HashFloat(hash, sprite.tint.r); HashFloat(hash, sprite.tint.g);
        HashFloat(hash, sprite.tint.b); HashFloat(hash, sprite.tint.a);
        HashFloat(hash, sprite.effectColor.r); HashFloat(hash, sprite.effectColor.g);
        HashFloat(hash, sprite.effectColor.b); HashFloat(hash, sprite.effectColor.a);
        HashFloat(hash, sprite.highlightColor.r); HashFloat(hash, sprite.highlightColor.g);
        HashFloat(hash, sprite.highlightColor.b); HashFloat(hash, sprite.highlightColor.a);
        HashU32(hash, static_cast<uint32_t>(sprite.effect));
    }
    return hash;
}

} // namespace

bool ScreenPresenter::Release(HIKARI::WorldView& world) {
    if (!canvas_.IsValid()) { return true; }
    const auto canvases = world.GetUiCanvases();
    const bool cleared = !surfaceBound_ || canvases.ClearSurface(canvas_, boundObject_);
    if (cleared) {
        // Destroy が失敗しても解除済みの状態を保持し、設定を戻した時に再 Bind できるようにする。
        surfaceBound_ = false;
    }
    const bool destroyed = canvases.Destroy(canvas_);
    if (!destroyed) {
        status_ = cleared ? "画布销毁失败" : "屏幕解除绑定及画布销毁失败";
        return false;
    }
    // Destroy が成功した時だけ所有権を捨てる。失敗時にハンドルを失って枠を漏らさない。
    canvas_ = {};
    boundObject_ = {};
    boundSlot_.clear();
    surfaceBound_ = false;
    outputWidth_ = outputHeight_ = 0;
    contentHash_ = committedRevision_ = 0;
    return true;
}

bool ScreenPresenter::Cancel(HIKARI::UiCanvasView canvases, std::string_view reason) {
    const bool cancelled = canvases.CancelUpdate(canvas_);
    status_ = std::string(reason) + (cancelled ? "；已取消本次提交" : "；取消更新也失败");
    return false;
}

void ScreenPresenter::UpdateFeedback(const Rules::WorldSnapshot& snapshot) {
    // 前回の局所スナップショットとの差分だけを採り、過去の領域や巻き戻った時計を持ち越さない。
    if (previousSnapshot_ && (previousSnapshot_->regionId != snapshot.regionId ||
                              snapshot.realTime < previousSnapshot_->realTime)) {
        feedback_.clear();
        motionSamples_.clear();
        pendingEchoStartedAt_.reset();
        previousSnapshot_.reset();
    }
    const auto add = [&](std::string family, const Rules::Rect& rect, double duration, bool reversed = false) {
        if (feedback_.size() >= 12u) { feedback_.erase(feedback_.begin()); }
        feedback_.push_back({std::move(family), rect, snapshot.realTime, duration, reversed});
    };

    if (previousSnapshot_) {
        const auto& previous = *previousSnapshot_;
        if (snapshot.dashing && !previous.dashing && snapshot.playerPresent) {
            feedback_.erase(std::remove_if(feedback_.begin(), feedback_.end(), [](const auto& item) {
                return item.family == "dash";
            }), feedback_.end());
            add("dash", snapshot.presentationPlayer, 0.140);
        }
        if (!previous.player.grounded && snapshot.player.grounded && previous.player.vy > 0.0 &&
            snapshot.playerPresent) {
            add("landing", snapshot.presentationPlayer, 0.180);
        }
        if ((snapshot.deaths > previous.deaths || (!previous.player.dead && snapshot.player.dead))) {
            add("death", previous.presentationPlayer, 0.220);
        }
        for (const auto& device : snapshot.devices) {
            const auto old = std::find_if(previous.devices.begin(), previous.devices.end(), [&](const auto& item) {
                return item.id == device.id && item.kind == device.kind;
            });
            if (old == previous.devices.end()) { continue; }
            if (device.kind == Rules::DeviceKind::Plate && !old->pressed && device.pressed) {
                add("plate", device.rect, 0.120);
            } else if ((device.kind == Rules::DeviceKind::Door || device.kind == Rules::DeviceKind::LockWall) &&
                       old->active != device.active) {
                add("door", device.rect, 0.135, !device.active);
            }
        }
        if (!snapshot.checkpointId.empty() && snapshot.checkpointId != previous.checkpointId) {
            const auto checkpoint = std::find_if(snapshot.devices.begin(), snapshot.devices.end(), [&](const auto& item) {
                return item.kind == Rules::DeviceKind::Checkpoint && item.id == snapshot.checkpointId;
            });
            const Rules::Rect rect = checkpoint == snapshot.devices.end()
                ? snapshot.presentationPlayer : checkpoint->rect;
            add("checkpoint", rect, 0.300);
        }
        if (previous.pendingEcho && !snapshot.pendingEcho) { pendingEchoStartedAt_.reset(); }
    }
    if (snapshot.pendingEcho && !pendingEchoStartedAt_) { pendingEchoStartedAt_ = snapshot.realTime; }
    if (!snapshot.pendingEcho) { pendingEchoStartedAt_.reset(); }

    if (snapshot.playerPresent && !snapshot.player.dead) {
        const ScreenPresentationMotionSample sample{snapshot.realTime, snapshot.presentationPlayer,
                                                      snapshot.player.facing};
        if (motionSamples_.empty() || snapshot.realTime > motionSamples_.back().realTime) {
            motionSamples_.push_back(sample);
        } else if (snapshot.realTime == motionSamples_.back().realTime) {
            motionSamples_.back() = sample;
        }
        const double oldest = snapshot.realTime - 0.25;
        motionSamples_.erase(std::remove_if(motionSamples_.begin(), motionSamples_.end(),
            [oldest](const auto& item) { return item.realTime < oldest; }), motionSamples_.end());
    }
    feedback_.erase(std::remove_if(feedback_.begin(), feedback_.end(), [&](const auto& item) {
        return !FeedbackIsActive(item, snapshot.realTime);
    }), feedback_.end());
    previousSnapshot_ = snapshot;
}

void ScreenPresenter::Refresh(HIKARI::WorldView& world, const Rules::WorldDefinition& definition,
                              const ScreenConfiguration& configuration, const Rules::WorldSnapshot* snapshot,
                              bool showPlayer) {
    // Canvas の再送はフレーム内容が変わった場合に限り、Rules の状態は読み取りだけにする。
    const auto& region = definition.region;
    const auto validation = ValidateRegion(region);
    if (!validation.valid) {
        if (Release(world)) { status_ = std::string(validation.message); }
        return;
    }
    if (snapshot != nullptr) { UpdateFeedback(*snapshot); }
    const size_t drawCount = CountDraws(definition, snapshot, showPlayer, feedback_, motionSamples_);
    if (drawCount > HIKARI::kGameProjectUiCanvasMaximumDraws) {
        if (Release(world)) {
            status_ = "合計 " + std::to_string(drawCount) +
                " 回の描画が UiCanvas の 512 回上限を超えています";
        }
        return;
    }
    if (configuration.outputWidth <= 0 || configuration.outputHeight <= 0 ||
        configuration.screenSlot.empty() || configuration.screenSlot.size() >= 96) {
        if (Release(world)) { status_ = "画布サイズまたは画面スロットが無効です"; }
        return;
    }
    const auto screen = world.FindSceneObject(configuration.screenObject);
    if (!screen.IsValid()) {
        if (Release(world)) { status_ = "有効な画面オブジェクトがありません。テレビモデルを指定してください"; }
        return;
    }
    const auto canvases = world.GetUiCanvases();
    if (!canvases.IsValid()) { status_ = "UiCanvas が利用できません。UI Feature と World 状態を確認してください"; return; }

    const auto width = static_cast<uint32_t>(configuration.outputWidth);
    const auto height = static_cast<uint32_t>(configuration.outputHeight);
    if (canvas_.IsValid() && (outputWidth_ != width || outputHeight_ != height ||
        boundObject_ != screen.GetHandle() || boundSlot_ != configuration.screenSlot)) {
        if (!Release(world)) { return; }
    }
    if (!canvas_.IsValid()) {
        HIKARI::GameProjectUiCanvasDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.clearColor = {0.010f, 0.019f, 0.025f, 1.0f};
        canvas_ = canvases.Create(desc);
        if (!canvas_.IsValid()) {
            status_ = "UiCanvas の作成に失敗しました。出力寸法または最大 4 面の上限を確認してください";
            return;
        }
        outputWidth_ = width;
        outputHeight_ = height;
        boundObject_ = screen.GetHandle();
        boundSlot_ = configuration.screenSlot;
    }

    const auto draws = BuildDraws(definition, snapshot, showPlayer, feedback_, motionSamples_, pendingEchoStartedAt_);
    const uint64_t hash = ContentHash(draws);
    const bool changed = committedRevision_ == 0 || hash != contentHash_;
    uint64_t revision = changed ? committedRevision_ + 1u : committedRevision_;
    if (revision == 0u) { revision = 1u; }
    if (changed || canvases.NeedsUpdate(canvas_, revision)) {
        if (!canvases.BeginUpdate(canvas_, revision)) {
            status_ = "BeginUpdate に失敗し、新しいスプライトを送信できませんでした";
            return;
        }
        for (const auto& draw : draws) {
            if (!canvases.SubmitSprite(canvas_, draw.texturePath, draw.request)) {
                (void)Cancel(canvases, "SubmitSprite に失敗しました。フレーム PNG のプロジェクト相対パスを確認してください");
                return;
            }
        }
        if (!canvases.CommitUpdate(canvas_)) {
            (void)Cancel(canvases, "CommitUpdate に失敗しました");
            return;
        }
        contentHash_ = hash;
        committedRevision_ = revision;
    }

    if (!surfaceBound_) {
        HIKARI::GameProjectUiCanvasSurfaceBinding binding{};
        binding.canvas = canvas_;
        binding.modelObject = boundObject_;
        std::memcpy(binding.materialSlotName, boundSlot_.c_str(), boundSlot_.size() + 1u);
        if (!canvases.BindSurface(binding)) {
            status_ = "BindSurface に失敗しました。対象に指定したスクリーン材質スロットが必要です";
            return;
        }
        surfaceBound_ = true;
    }

    // 外側の同名スロットを誤変更しないよう、現在表示中のモデル GUID を照合して毎回安全に再適用する。
    std::string brightnessStatus;
    if (configuration.materialIndex >= 0 && !configuration.modelAssetGuid.empty()) {
        HIKARI::GameProjectModelAppearance appearance{};
        const auto model = screen.GetModel();
        const bool appearanceReady = model.TryGetAppearance(appearance);
        const bool modelMatches = appearanceReady &&
            configuration.modelAssetGuid.size() < sizeof(appearance.assetId) &&
            std::strncmp(appearance.assetId, configuration.modelAssetGuid.c_str(),
                         configuration.modelAssetGuid.size() + 1u) == 0;
        if (!std::isfinite(configuration.brightness)) {
            brightnessStatus = "; materialGain=ignored-nonfinite";
        } else if (!modelMatches) {
            brightnessStatus = appearanceReady ? "; materialGain=skipped-model-mismatch"
                                               : "; materialGain=retry-appearance-unavailable";
        } else {
            const float gain = std::clamp(configuration.brightness, 0.5f, 4.0f);
            const HIKARI::GameProjectFloat3 baseColor{gain, gain, gain};
            const HIKARI::GameProjectFloat3 noEmissive{0.0f, 0.0f, 0.0f};
            const auto applied = model.SetMaterialColorOverride(
                static_cast<uint32_t>(configuration.materialIndex), baseColor, 1.0f, noEmissive, 0.0f);
            brightnessStatus = applied.Succeeded()
                ? "; materialGain=" + std::to_string(gain)
                : "; materialGain=retry-result-" +
                    std::to_string(static_cast<uint32_t>(applied.code));
        }
    }

    HIKARI::GameProjectUiCanvasSnapshot canvasSnapshot{};
    if (!canvases.GetSnapshot(canvas_, canvasSnapshot)) {
        status_ = "UiCanvas の表示スナップショットを取得できませんでした";
        return;
    }
    // ready と renderedRevision を分け、CPU 送信を GPU 描画完了と取り違えない。
    status_ = "UiCanvas ready=" + std::to_string(canvasSnapshot.ready) +
        " drawCount=" + std::to_string(drawCount) +
        " contentRevision=" + std::to_string(canvasSnapshot.contentRevision) +
        " renderedRevision=" + std::to_string(canvasSnapshot.renderedRevision) +
        " renderedFrame=" + std::to_string(canvasSnapshot.renderedFrame) + brightnessStatus;
}

void ScreenPresentationSystem::Update(HIKARI::WorldView& world, const HIKARI::FrameContext&) {
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kRegionComponentType)};
    const auto result = world.ForEachComponents<RegionComponent>(types,
        HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&world](HIKARI::ObjectView, RegionComponent& region) { region.RefreshScreen(world); });
    // コンポーネント未発見と Query 失敗を区別し、シーンのシステム欄で診断できるようにする。
    if (!result.Succeeded()) {
        queryStatus_ = "区域查询失败 code=" + std::to_string(static_cast<uint32_t>(result.code)) +
            " term=" + std::to_string(result.diagnosticTermIndex);
        return;
    }
    queryStatus_ = "区域查询成功 rowsVisited=" + std::to_string(result.rowsVisited);
}

} // namespace TD2
