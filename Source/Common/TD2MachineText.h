#pragma once
// 画面の外の仕掛け（機構・スロット棚・テレビの塔・回転柱）の文字列欄の書式。
// ランタイムの読み込みとエディタの読み書きは、すべてここを通す。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TD2 {
// 圧力板の目標のうち、仕掛けの受け口（mech/ID、slide/区域ID、stack/ID/…、carousel/ID/…）。
inline bool IsMachineTarget(std::string_view target) {
    return target.starts_with("mech/") || target.starts_with("slide/") || target.starts_with("stack/") || target.starts_with("carousel/");
}

// 区域のずれ：区域ID dX dY [flip] [@錨の区域ID]。区域ID park はどことも重ならない預け場所へ離す。
struct MachineRegionOffset {
    std::string regionId, anchor;   // anchor が空なら自分の配置からのずれ
    int dx = 0, dy = 0;
    bool flip = false, park = false;
    bool operator==(const MachineRegionOffset&) const = default;
};
// 物体のずれ：物体ID dx dy dz rx ry rz [cx cy cz] [@錨の物体ID] [~始め-終わり] [^高さ]。
// 基準（状態 A、錨があればその物体）の向きで測った位置（m）と、基準に後から掛ける回転（度）。cx cy cz は回す中心。
// ~始め-終わり は全体の進み（0〜1）のうちこの物体が動く区間。先にどく物と後から入る物がぶつからないよう順番を付ける。
// ^高さ は跳ぶ乗り物の高さ（m）。先にこの高さまで浮き上がり、高いところで運んで、最後に降りる（棚や塔を上から越える）。
// 負の高さは落ちる乗り物：先にその分だけ真下へ落ち、横へ運ぶにつれて終わりの高さへなじむ。
struct MachineObjectDelta {
    uint64_t object = 0, anchor = 0;
    std::array<double, 3> position{}, rotation{}, pivot{};
    bool hasPivot = false;
    double from = 0, to = 1, lift = 0;
    bool operator==(const MachineObjectDelta&) const = default;
};

namespace MachineText {
// 一行の欄でも書けるよう、「;」も行の区切りとして読む。
inline std::string Lines(std::string text) {
    std::replace(text.begin(), text.end(), ';', '\n');
    return text;
}
// 小数 4 桁までで、末尾の 0 を省いて書く。
inline std::string Number(double v) {
    if (std::abs(v) < 5e-5) { return "0"; }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(4) << v;
    auto text = out.str();
    while (!text.empty() && text.back() == '0') { text.pop_back(); }
    if (!text.empty() && text.back() == '.') { text.pop_back(); }
    return text;
}

// 区域 ID の並び（カンマか改行区切り）。
inline std::vector<std::string> ParseNames(std::string text) {
    std::replace(text.begin(), text.end(), ',', '\n');
    std::vector<std::string> names;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::istringstream value(line);
        if (std::string id; value >> id) { names.push_back(id); }
    }
    return names;
}
inline std::string JoinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& name : names) { out += (out.empty() ? "" : ",") + name; }
    return out;
}

// 以下の Parse は読めた行を out に入れ、最初に読めなかった行の説明を error に書く。
// ランタイムは error があればその仕掛けを止め、エディタは読めた分だけを使う。
inline bool ParseOffsets(const std::string& text, std::vector<MachineRegionOffset>& out, std::string& error) {
    out.clear(); error.clear();
    std::istringstream lines(Lines(text));
    for (std::string line; std::getline(lines, line);) {
        std::istringstream values(line);
        values.imbue(std::locale::classic());
        MachineRegionOffset offset;
        if (!(values >> offset.regionId)) { continue; }
        const auto fail = [&error](std::string_view message) { if (error.empty()) { error = message; } };
        std::string first;
        if (!(values >> first)) { fail("区域の行：区域ID dX dY [flip] [@基準の区域] か 区域ID park"); continue; }
        if (first == "park") { offset.park = true; out.push_back(std::move(offset)); continue; }
        try { offset.dx = std::stoi(first); } catch (...) { fail("区域の行の dX は整数"); continue; }
        if (!(values >> offset.dy)) { fail("区域の行：区域ID dX dY [flip] [@基準の区域]"); continue; }
        bool valid = true;
        for (std::string option; values >> option;) {
            if (option == "flip") { offset.flip = true; }
            else if (option.size() > 1 && option[0] == '@') { offset.anchor = option.substr(1); }
            else { fail("区域の行の追加は flip か @基準の区域だけ"); valid = false; break; }
        }
        if (valid) { out.push_back(std::move(offset)); }
    }
    return error.empty();
}
inline std::string FormatOffsets(const std::vector<MachineRegionOffset>& offsets) {
    std::string out;
    for (const auto& o : offsets) {
        if (!out.empty()) { out += "; "; }
        if (o.park) { out += o.regionId + " park"; continue; }
        out += o.regionId + " " + std::to_string(o.dx) + " " + std::to_string(o.dy);
        if (o.flip) { out += " flip"; }
        if (!o.anchor.empty()) { out += " @" + o.anchor; }
    }
    return out;
}

inline bool ParseDeltas(const std::string& text, std::vector<MachineObjectDelta>& out, std::string& error) {
    out.clear(); error.clear();
    std::istringstream lines(Lines(text));
    for (std::string line; std::getline(lines, line);) {
        std::istringstream values(line);
        values.imbue(std::locale::classic());
        MachineObjectDelta delta;
        if (!(values >> delta.object)) { continue; }
        const auto fail = [&error](std::string_view message) { if (error.empty()) { error = message; } };
        if (!(values >> delta.position[0] >> delta.position[1] >> delta.position[2] >> delta.rotation[0] >> delta.rotation[1] >> delta.rotation[2])) {
            fail("物体の行：物体ID dx dy dz rx ry rz [cx cy cz] [@基準の物体]"); continue;
        }
        std::vector<std::string> rest;
        bool timed = true;
        for (std::string token; values >> token;) {
            // 動く区間は位置を問わず読む（~0.4-1）。
            if (token.size() > 1 && token[0] == '~') {
                const auto dash = token.find('-', 2);
                try {
                    if (dash == std::string::npos) { throw 0; }
                    delta.from = std::stod(token.substr(1, dash - 1)); delta.to = std::stod(token.substr(dash + 1));
                } catch (...) { timed = false; }
                if (!(delta.from >= 0 && delta.from < delta.to && delta.to <= 1)) { timed = false; }
                continue;
            }
            if (token.size() > 1 && token[0] == '^') {
                try { delta.lift = std::stod(token.substr(1)); } catch (...) { timed = false; }
                continue;
            }
            rest.push_back(token);
        }
        if (!timed) { fail("動く区間は ~始め-終わり（0 以上 1 以下、始め < 終わり）、弧は ^高さ"); continue; }
        size_t i = 0;
        if (rest.size() >= 3 && rest[0][0] != '@') {
            try { for (size_t k = 0; k < 3; ++k) { delta.pivot[k] = std::stod(rest[k]); } }
            catch (...) { fail("物体の行の回転の中心は三つの数"); continue; }
            delta.hasPivot = true; i = 3;
        }
        if (i < rest.size()) {
            if (rest[i].size() < 2 || rest[i][0] != '@') { fail("物体の行の最後は @基準の物体だけ"); continue; }
            try { delta.anchor = std::stoull(rest[i].substr(1)); } catch (...) { fail("基準の物体には物体 ID が要ります"); continue; }
            ++i;
        }
        if (i != rest.size()) { fail("物体の行に余計な項目があります"); continue; }
        out.push_back(delta);
    }
    return error.empty();
}
inline std::string FormatDeltas(const std::vector<MachineObjectDelta>& deltas) {
    std::string out;
    for (const auto& d : deltas) {
        if (!out.empty()) { out += "; "; }
        out += std::to_string(d.object);
        for (const double v : d.position) { out += " " + Number(v); }
        for (const double v : d.rotation) { out += " " + Number(v); }
        if (d.hasPivot) { for (const double v : d.pivot) { out += " " + Number(v); } }
        if (d.anchor) { out += " @" + std::to_string(d.anchor); }
        if (d.from > 0 || d.to < 1) { out += " ~" + Number(d.from) + "-" + Number(d.to); }
        if (d.lift != 0) { out += " ^" + Number(d.lift); }
    }
    return out;
}

// スロット棚の空きスロット：一番目のメンバーの世界地図の位置からのずれ（dX dY）。
inline bool ParseSlots(const std::string& text, std::vector<std::pair<int, int>>& out, std::string& error) {
    out.clear(); error.clear();
    std::istringstream lines(Lines(text));
    for (std::string line; std::getline(lines, line);) {
        std::istringstream values(line);
        std::pair<int, int> slot;
        if (!(values >> slot.first)) { continue; }
        if (!(values >> slot.second)) { if (error.empty()) { error = "空きスロットの行：dX dY"; } continue; }
        out.push_back(slot);
    }
    return error.empty();
}
inline std::string FormatSlots(const std::vector<std::pair<int, int>>& slots) {
    std::string out;
    for (const auto& [x, y] : slots) { out += (out.empty() ? "" : "; ") + std::to_string(x) + " " + std::to_string(y); }
    return out;
}

// 空白か「;」区切りの数の並び（テレビの塔の抜く距離など）。
inline std::vector<double> ParseNumbers(const std::string& text) {
    std::istringstream values(Lines(text));
    values.imbue(std::locale::classic());
    std::vector<double> numbers;
    for (double v = 0; values >> v;) { numbers.push_back(v); }
    return numbers;
}
inline std::string FormatNumbers(const std::vector<double>& numbers) {
    std::string out;
    for (const double v : numbers) { out += (out.empty() ? "" : " ") + Number(v); }
    return out;
}
}
}
