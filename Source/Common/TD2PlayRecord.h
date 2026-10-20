#pragma once
// 試遊の記録（解法）。ゲームが毎フレームの秒とボタン、カメラからルールへの呼び出しを書き、
// 編集器が読み返して同じルールの写しで再生する。行の文字列で書き、JSON の部品に頼らない。
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace TD2 {
struct PlayRecord {
    // 入力の印：1 左、2 右、4 空格、8 世界が止まっていた（手動の切り替え中）。
    struct Frame { double seconds = 0; uint8_t flags = 0; };
    // フレームの slice 番目の刻みの後に呼んだもの（slice が刻みの数と同じならフレームの最後）。kind は clear か back。
    struct Call { uint32_t frame = 0, slice = 0; std::string kind, region; int direction = 0; };
    struct Mark { uint32_t frame = 0; std::string region; };
    std::string entryRegion;                 // 空なら作者の開始テレビ
    std::optional<std::pair<double, double>> spawn;   // 試遊の入口（区域内の左上基準）
    std::vector<std::pair<std::string, int>> states; // 試遊の初めに置いた仕掛けの状態
    std::vector<Frame> frames;
    std::vector<Call> calls;
    std::vector<Mark> regions, deaths;      // 今いる区域が変わった時と、死んだ時
    std::string endRegion;
    double endX = 0, endY = 0;
    uint64_t endDeaths = 0;
};

// 一時ディレクトリの置き場（ゲームと編集器は同じプロセスで動く）。
inline std::filesystem::path PlayFolder() {
    std::error_code error;
    auto folder = std::filesystem::temp_directory_path(error) / "TD2_TV";
    std::filesystem::create_directories(folder, error);
    return folder;
}
inline std::filesystem::path LastPlayPath() { return PlayFolder() / "last_play.td2play"; }
inline std::filesystem::path PreviewStatesPath() { return PlayFolder() / "preview_states.txt"; }

inline bool WritePlayRecord(const std::filesystem::path& path, const PlayRecord& r) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { return false; }
    out.imbue(std::locale::classic());
    out.precision(9);
    out << "td2play 1\n";
    out << "entry " << (r.entryRegion.empty() ? "-" : r.entryRegion);
    if (r.spawn) { out << ' ' << r.spawn->first << ' ' << r.spawn->second; }
    out << '\n';
    for (const auto& [id, state] : r.states) { out << "state " << id << ' ' << state << '\n'; }
    for (const auto& f : r.frames) { out << "f " << f.seconds << ' ' << static_cast<int>(f.flags) << '\n'; }
    for (const auto& c : r.calls) { out << "call " << c.frame << ' ' << c.slice << ' ' << c.kind << ' ' << (c.region.empty() ? "-" : c.region) << ' ' << c.direction << '\n'; }
    for (const auto& m : r.regions) { out << "region " << m.frame << ' ' << m.region << '\n'; }
    for (const auto& m : r.deaths) { out << "death " << m.frame << ' ' << m.region << '\n'; }
    out << "end " << (r.endRegion.empty() ? "-" : r.endRegion) << ' ' << r.endX << ' ' << r.endY << ' ' << r.endDeaths << '\n';
    return static_cast<bool>(out);
}

inline std::optional<PlayRecord> ReadPlayRecord(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { return std::nullopt; }
    PlayRecord r;
    std::string line;
    if (!std::getline(in, line) || line.rfind("td2play 1", 0) != 0) { return std::nullopt; }
    while (std::getline(in, line)) {
        std::istringstream s(line);
        s.imbue(std::locale::classic());
        std::string key; s >> key;
        if (key == "f") { PlayRecord::Frame f; int flags = 0; s >> f.seconds >> flags; f.flags = static_cast<uint8_t>(flags); r.frames.push_back(f); }
        else if (key == "entry") {
            std::string region; s >> region; r.entryRegion = region == "-" ? std::string{} : region;
            double x = 0, y = 0; if (s >> x >> y) { r.spawn = std::pair{x, y}; }
        } else if (key == "state") { std::string id; int state = 0; if (s >> id >> state) { r.states.push_back({id, state}); } }
        else if (key == "call") { PlayRecord::Call c; s >> c.frame >> c.slice >> c.kind >> c.region >> c.direction; if (c.region == "-") { c.region.clear(); } r.calls.push_back(c); }
        else if (key == "region") { PlayRecord::Mark m; s >> m.frame >> m.region; r.regions.push_back(m); }
        else if (key == "death") { PlayRecord::Mark m; s >> m.frame >> m.region; r.deaths.push_back(m); }
        else if (key == "end") { s >> r.endRegion >> r.endX >> r.endY >> r.endDeaths; if (r.endRegion == "-") { r.endRegion.clear(); } }
    }
    return r;
}

// 試遊の初めの仕掛けの状態（編集器が書き、ゲームが一回だけ読んで消す）。
inline std::vector<std::pair<std::string, int>> TakePreviewStates() {
    std::vector<std::pair<std::string, int>> states;
    const auto path = PreviewStatesPath();
    {
        std::ifstream in(path);
        std::string id; int state = 0;
        while (in >> id >> state) { states.push_back({id, state}); }
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    return states;
}
}
