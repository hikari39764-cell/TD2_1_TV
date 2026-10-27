#include "TD2SaveData.h"
#include "HIKARI/Project/GameFlowViews.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace TD2 {
namespace {
constexpr std::string_view kProgressHeader = "TD2PROGRESS 2";   // 2：復活点を足元で残す
constexpr std::string_view kSettingsHeader = "TD2SETTINGS 1";
// 保存の枠の大きさの上限（進みは数 KB、これを越えるものは壊れているとみなす）。
constexpr uint64_t kSlotLimit = 1u << 20;

// 行の数を最後の「end 行数」で確かめてから一行ずつ渡す。
template <class Line>
bool ReadLines(std::string_view text, std::string_view header, Line&& line) {
    std::istringstream in{std::string(text)};
    std::string first;
    if (!std::getline(in, first) || first != header) return false;
    size_t count = 0;
    bool ended = false;
    for (std::string row; std::getline(in, row);) {
        if (row.empty()) continue;
        std::istringstream words(row);
        std::string key;
        words >> key;
        if (key == "end") { size_t expected = 0; words >> expected; ended = expected == count; break; }
        if (!line(key, words)) return false;
        ++count;
    }
    return ended;
}
}

std::string_view ProgressSlot() {
    static const std::string slot = [] { const char* v = std::getenv("TD2_SAVE_SLOT"); return std::string(v && *v ? v : "progress"); }();
    return slot;
}

std::string_view SettingsSlot() {
    static const std::string slot = [] { const char* v = std::getenv("TD2_SAVE_SLOT"); return v && *v ? std::string(v) + "_settings" : std::string("settings"); }();
    return slot;
}

std::string WriteProgressText(const Rules::Progress& p) {
    std::ostringstream out;
    out.precision(9);
    size_t count = 0;
    out << kProgressHeader << '\n';
    out << "checkpoint " << p.region << ' ' << p.local.x << ' ' << p.local.y << ' ' << (p.checkpoint.empty() ? "-" : p.checkpoint) << '\n'; ++count;
    for (const auto& [id, state] : p.mechanisms) { out << "mech " << id << ' ' << state << '\n'; ++count; }
    for (const auto& [id, step] : p.carousels) { out << "carousel " << id << ' ' << step << '\n'; ++count; }
    for (const auto& [id, slots] : p.slides) { out << "slide " << id << ' ' << slots.size(); for (const int s : slots) out << ' ' << s; out << '\n'; ++count; }
    for (const auto& [id, places] : p.stacks) { out << "stack " << id << ' ' << places.size(); for (const int s : places) out << ' ' << s; out << '\n'; ++count; }
    for (const auto& id : p.latched) { out << "latch " << id << '\n'; ++count; }
    for (const auto& id : p.taken) { out << "gem " << id << '\n'; ++count; }
    for (const auto& [a, b] : p.sealed) { out << "seal " << a << ' ' << b << '\n'; ++count; }
    if (!p.finale.empty()) { out << "finale " << p.finale << '\n'; ++count; }
    out << "end " << count << '\n';
    return out.str();
}

bool ReadProgressText(std::string_view text, Rules::Progress& out) {
    Rules::Progress p;
    bool checkpoint = false;
    const auto list = [](std::istringstream& words, std::vector<int>& values) {
        size_t n = 0;
        if (!(words >> n) || n > 64) return false;
        values.resize(n);
        for (auto& v : values) if (!(words >> v)) return false;
        return true;
    };
    const bool ok = ReadLines(text, kProgressHeader, [&](const std::string& key, std::istringstream& words) {
        std::string id;
        if (key == "checkpoint") {
            std::string device;
            if (!(words >> p.region >> p.local.x >> p.local.y >> device)) return false;
            p.checkpoint = device == "-" ? std::string{} : device;
            checkpoint = true;
            return true;
        }
        if (!(words >> id)) return false;
        if (key == "mech" || key == "carousel") {
            int value = 0;
            if (!(words >> value)) return false;
            (key == "mech" ? p.mechanisms : p.carousels).push_back({id, value});
            return true;
        }
        if (key == "slide" || key == "stack") {
            std::vector<int> values;
            if (!list(words, values)) return false;
            (key == "slide" ? p.slides : p.stacks).push_back({id, std::move(values)});
            return true;
        }
        if (key == "latch") { p.latched.push_back(id); return true; }
        if (key == "gem") { p.taken.push_back(id); return true; }
        if (key == "seal") { std::string other; if (!(words >> other)) return false; p.sealed.push_back({id, other}); return true; }
        if (key == "finale") { p.finale = id; return true; }
        return true;   // 新しい版の知らない行は読み飛ばす
    });
    if (!ok || !checkpoint || p.region.empty()) return false;
    out = std::move(p);
    return true;
}

std::string WriteSettingsText(const UserSettings& s) {
    std::ostringstream out;
    out << kSettingsHeader << '\n' << "bgm " << s.bgm << '\n' << "se " << s.se << '\n' << "ambient " << s.ambient << '\n'
        << "window " << s.windowMode << '\n' << "resolution " << s.resolution << '\n' << "vsync " << (s.vsync ? 1 : 0) << '\n' << "end 6\n";
    return out.str();
}

bool ReadSettingsText(std::string_view text, UserSettings& out) {
    UserSettings s;
    const auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
    const bool ok = ReadLines(text, kSettingsHeader, [&](const std::string& key, std::istringstream& words) {
        int value = 0;
        if (!(words >> value)) return false;
        if (key == "bgm") s.bgm = clamp(value, 0, 10);
        else if (key == "se") s.se = clamp(value, 0, 10);
        else if (key == "ambient") s.ambient = clamp(value, 0, 10);
        else if (key == "window") s.windowMode = clamp(value, 0, 2);
        else if (key == "resolution") s.resolution = clamp(value, 0, 3);
        else if (key == "vsync") s.vsync = value != 0;
        return true;
    });
    if (!ok) return false;
    out = s;
    return true;
}

bool LoadSlot(HIKARI::WorldView& world, std::string_view slot, std::string& out) {
    const auto save = world.GetSaveGame();
    if (!save.IsValid() || !save.Exists(slot)) return false;
    const uint64_t size = save.GetSize(slot);
    if (size == 0 || size > kSlotLimit) return false;
    std::string text(static_cast<size_t>(size), '\0');
    uint64_t read = 0;
    if (!save.Read(slot, text.data(), size, read) || read != size) return false;
    out = std::move(text);
    return true;
}

bool StoreSlot(HIKARI::WorldView& world, std::string_view slot, const std::string& text) {
    const auto save = world.GetSaveGame();
    const bool ok = save.IsValid() && save.WriteAtomic(slot, text.data(), text.size());
    std::fprintf(stderr, "[TD2 save] write slot=%.*s bytes=%zu ok=%d\n", static_cast<int>(slot.size()), slot.data(), text.size(), ok);
    return ok;
}

void DeleteSlot(HIKARI::WorldView& world, std::string_view slot) {
    const auto save = world.GetSaveGame();
    if (save.IsValid() && save.Exists(slot)) {
        const bool ok = save.Delete(slot);
        std::fprintf(stderr, "[TD2 save] delete slot=%.*s ok=%d\n", static_cast<int>(slot.size()), slot.data(), ok);
    }
}
}
