#pragma once
// テレビの中の弾窓の台本（TD2.PopupScript の「弾窓」欄）の書式。ランタイムの読み込みとエディタの読み書きで共有する。
// 一行一つ：id 種類 [key=value ...] | 文。# で始まる行は注釈。文の中の改行は \n、\ は \\ と書く。
//   種類  random（時々）・mood（様子）・event（出来事）・room（部屋に初めて入った時）
//   tier  random の段：early（前）・middle（中）・late（後）・any（いつでも）
//   mood  idle（止まったまま）・died（死んだすぐ後）・stuck（同じ部屋に長くいる）・echo（残影がいる）
//   on    event のきっかけ：checkpoint・gem・mechanism・area・door・worry（同じ部屋で何度も死ぬ）、
//         操作の案内 move・dash・swap（覚えるまで出したまま）、swapped（初めて入れ替えた時に一度）
//   room  その部屋（区域 ID）にいる時だけ。room 種類では入る部屋
//   chance  0 より大きければおまけ：時々・様子の番ごとにこの確率で、ほかの言葉の代わりに出る
//   once  1 なら遊びの保存ごとに一度だけ
//   title 題（空なら「？？？」。空白は書けない）、icon  auto・none・question・warning・mail
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace TD2 {
inline constexpr std::string_view kPopupScriptComponentType = "TD2.PopupScript";

inline constexpr std::array<std::string_view, 4> kPopupTriggerNames{"random", "mood", "event", "room"};
inline constexpr std::array<std::string_view, 4> kPopupTierNames{"early", "middle", "late", "any"};
inline constexpr std::array<std::string_view, 4> kPopupMoodNames{"idle", "died", "stuck", "echo"};
inline constexpr std::array<std::string_view, 10> kPopupEventNames{"checkpoint", "gem", "mechanism", "area", "door", "worry",
                                                                    "move", "dash", "swap", "swapped"};
inline constexpr std::array<std::string_view, 5> kPopupIconNames{"auto", "none", "question", "warning", "mail"};
enum class PopupTrigger : int { Random = 0, Mood, Event, Room };
enum class PopupTier : int { Early = 0, Middle, Late, Any };
enum class PopupMood : int { Idle = 0, Died, Stuck, Echo };
enum class PopupEvent : int { Checkpoint = 0, Gem, Mechanism, Area, Door, Worry, Move, Dash, Swap, Swapped };
enum class PopupIcon : int { Auto = 0, None, Question, Warning, Mail };

struct PopupEntry {
    std::string id;
    PopupTrigger trigger = PopupTrigger::Random;
    PopupTier tier = PopupTier::Early;
    PopupMood mood = PopupMood::Idle;
    PopupEvent event = PopupEvent::Checkpoint;
    std::string room;
    double chance = 0;
    bool once = false;
    std::string title;
    PopupIcon icon = PopupIcon::Auto;
    std::string text;
    bool operator==(const PopupEntry&) const = default;
};

// 頻度の決まり（台本の組件の数の欄。エディタの「設定」で調える）。
struct PopupSettings {
    float firstDelay = 45;        // 最初の時々の弾窓までの遊んだ秒
    float intervalMax = 60;       // 時々の弾窓の間隔（大区域を進むほど intervalMax から intervalMin へ縮む）
    float intervalMin = 22;
    float intervalPerArea = 8;    // 大区域一つで縮める秒
    float moodChance = .35f;      // 様子に当てはまる時、その言葉にする確率
    float moodCooldown = 180;     // 同じ様子の言葉を出してから次に出せるまでの秒
    float idleSeconds = 8, diedSeconds = 10, stuckSeconds = 60;
    float eventGap = 20;          // 出来事の弾窓どうしの最短の間（大区域と何度も死ぬは除く）
    int history = 12;             // 最近これだけの弾窓に出た言葉は出さない
    bool checkpointPerArea = true;   // チェックポイントは大区域ごとに一度
    bool mechanismPerArea = true;    // 機構は大区域ごとに一度
    bool echoPerArea = true;         // 残影の様子の言葉は大区域ごとに一度
    int deathsToWorry = 5;           // 同じ部屋で何度死んだら worry
    bool operator==(const PopupSettings&) const = default;
};

namespace PopupText {
template <size_t N>
int IndexOf(const std::array<std::string_view, N>& names, std::string_view name) {
    for (size_t i = 0; i < N; ++i) { if (names[i] == name) { return static_cast<int>(i); } }
    return -1;
}
// 区域・弾窓の ID に使える文字列（1～95 文字の ASCII 英数字と . _ -）。
inline bool StableId(std::string_view id) {
    if (id.empty() || id.size() > 95) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}
inline std::string Escape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\\') out += "\\\\";
        else if (c != '\r') out += c;
    }
    return out;
}
inline std::string Unescape(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { ++i; out += s[i] == 'n' ? '\n' : s[i]; }
        else out += s[i];
    }
    return out;
}
inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// 読めない行があれば「弾窓の N 行目：…」を error に書いて false を返す（それまでに読めた行は out に残す）。
// skipBad なら読めない行と重なった id の行だけを飛ばして最後まで読み、最初の問題を error に残す（遊ぶ時に一行の誤りで
// 台本全体を捨てない）。
inline bool Parse(std::string_view text, std::vector<PopupEntry>& out, std::string& error, bool skipBad = false) {
    out.clear(); error.clear();
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::istringstream lines{std::string(text)};
    unsigned lineNumber = 0;
    bool ok = true;
    for (std::string raw; std::getline(lines, raw);) {
        ++lineNumber;
        const auto line = Trim(raw);
        if (line.empty() || line.front() == '#') continue;
        const auto fail = [&error, &ok, lineNumber, skipBad](std::string_view message) {
            if (error.empty()) error = "弾窓の " + std::to_string(lineNumber) + " 行目：" + std::string(message);
            ok = false;
            return skipBad;   // true なら次の行へ
        };
        const auto reject = [&](std::string_view message) { return fail(message); };
        const auto bar = line.find('|');
        if (bar == std::string_view::npos) { if (reject("「|」の後に文が要ります")) continue; return false; }
        PopupEntry entry;
        entry.text = Unescape(Trim(line.substr(bar + 1)));
        std::istringstream head{std::string(line.substr(0, bar))};
        std::string trigger;
        if (!(head >> entry.id >> trigger) || !StableId(entry.id)) { if (reject("id と種類が要ります（id は英数字と . _ -）")) continue; return false; }
        const int t = IndexOf(kPopupTriggerNames, trigger);
        if (t < 0) { if (reject("種類は random・mood・event・room のどれか")) continue; return false; }
        entry.trigger = static_cast<PopupTrigger>(t);
        std::string problem;
        for (std::string option; problem.empty() && head >> option;) {
            const auto eq = option.find('=');
            if (eq == std::string::npos) { problem = "「" + option + "」は key=value で書きます"; break; }
            const std::string key = option.substr(0, eq), value = option.substr(eq + 1);
            int index = -1;
            if (key == "tier" && (index = IndexOf(kPopupTierNames, value)) >= 0) entry.tier = static_cast<PopupTier>(index);
            else if (key == "mood" && (index = IndexOf(kPopupMoodNames, value)) >= 0) entry.mood = static_cast<PopupMood>(index);
            else if (key == "on" && (index = IndexOf(kPopupEventNames, value)) >= 0) entry.event = static_cast<PopupEvent>(index);
            else if (key == "icon" && (index = IndexOf(kPopupIconNames, value)) >= 0) entry.icon = static_cast<PopupIcon>(index);
            else if (key == "room" && StableId(value)) entry.room = value;
            else if (key == "title") entry.title = value;
            else if (key == "once" && (value == "0" || value == "1")) entry.once = value == "1";
            else if (key == "chance") {
                double v = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), v);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(v) || v < 0 || v > 1) {
                    problem = "chance は 0〜1";
                } else {
                    entry.chance = v;
                }
            }
            else problem = "「" + option + "」は読めません";
        }
        if (problem.empty() && entry.trigger == PopupTrigger::Room && entry.room.empty()) problem = "room 種類には room=区域ID が要ります";
        if (problem.empty() && entry.text.empty()) problem = "文が空です";
        if (problem.empty() && std::any_of(out.begin(), out.end(), [&](const PopupEntry& e) { return e.id == entry.id; })) {
            problem = "id「" + entry.id + "」が重なっています";
        }
        if (!problem.empty()) { if (reject(problem)) continue; return false; }
        out.push_back(std::move(entry));
    }
    return ok;
}

// 決まった形で書く（初期値の項目は省く）。種類や段が変わる所に組の注釈を入れる（読む時は無視する）。
inline std::string Format(const std::vector<PopupEntry>& entries) {
    constexpr std::array<std::string_view, 4> kTierLabels{"前の段", "中の段", "後の段", "いつでも"};
    constexpr std::array<std::string_view, 4> kMoodLabels{"止まったまま", "死んだすぐ後", "長居", "残影"};
    constexpr std::array<std::string_view, 10> kEventLabels{"チェックポイント", "宝石", "機構", "大区域", "扉", "何度も死ぬ",
                                                            "移動の案内", "ダッシュの案内", "入れ替えの案内", "入れ替えた"};
    std::string out, group;
    for (const auto& e : entries) {
        std::string label;
        if (e.trigger == PopupTrigger::Random) label = "# 時々：" + std::string(kTierLabels[static_cast<size_t>(e.tier)]);
        else if (e.trigger == PopupTrigger::Mood) label = "# 様子：" + std::string(kMoodLabels[static_cast<size_t>(e.mood)]);
        else if (e.trigger == PopupTrigger::Event) label = "# 出来事：" + std::string(kEventLabels[static_cast<size_t>(e.event)]);
        else label = "# 部屋";
        if (label != group) { out += label; out += '\n'; group = label; }
        out += e.id; out += ' '; out += kPopupTriggerNames[static_cast<size_t>(e.trigger)];
        if (e.trigger == PopupTrigger::Random) { out += " tier="; out += kPopupTierNames[static_cast<size_t>(e.tier)]; }
        if (e.trigger == PopupTrigger::Mood) { out += " mood="; out += kPopupMoodNames[static_cast<size_t>(e.mood)]; }
        if (e.trigger == PopupTrigger::Event) { out += " on="; out += kPopupEventNames[static_cast<size_t>(e.event)]; }
        if (!e.room.empty()) { out += " room="; out += e.room; }
        if (e.chance > 0) {
            std::array<char, 32> number{};
            const auto written = std::to_chars(number.data(), number.data() + number.size(), e.chance);
            out += " chance="; out.append(number.data(), written.ptr);
        }
        if (e.once) out += " once=1";
        if (!e.title.empty()) { out += " title="; out += e.title; }
        if (e.icon != PopupIcon::Auto) { out += " icon="; out += kPopupIconNames[static_cast<size_t>(e.icon)]; }
        out += " | "; out += Escape(e.text); out += '\n';
    }
    return out;
}

// 新しく作った台本の中身（2026-10-28 に決めた言葉）。
inline std::string_view DefaultScript() {
    return R"(# 時々：前の段
r.e01 random tier=early | みてるよ
r.e02 random tier=early | たのしい？
r.e03 random tier=early | こっち こっち
r.e04 random tier=early | がんばれー
r.e05 random tier=early | ねえ、きこえる？
r.e06 random tier=early | ゴールで まってるね
r.e07 random tier=early | あそぼ
r.e08 random tier=early | いい てんき だね
r.e09 random tier=early | そっちは\nなにが あるの？
r.e10 random tier=early | じょうず じょうず
r.e11 random tier=early | わたしも\nやりたいな
r.e12 random tier=early | テレビ すき？
r.e13 random tier=early | もっと はやく！
r.e14 random tier=early | ここ、ひろいね
r.e15 random tier=early | いっしょに\nかえろうね
r.e16 random tier=early | あとで\nおしえてね
r.e17 random tier=early | ぴかぴか してる
r.e18 random tier=early | えへへ
# 時々：中の段
r.m01 random tier=middle | まだ かな
r.m02 random tier=middle | どこ いくの？
r.m03 random tier=middle | ずっと いっしょだよ
r.m04 random tier=middle | うしろ みた？
r.m05 random tier=middle | もう すこし
r.m06 random tier=middle | わたしの こと\nすき？
r.m07 random tier=middle | にげないでね
r.m08 random tier=middle | ちゃんと\nみててね
r.m09 random tier=middle | さっきから ひとり？
r.m10 random tier=middle | おなか すいた？
r.m11 random tier=middle | ねむく ない？
r.m12 random tier=middle | てを\nはなさないで
r.m13 random tier=middle | そこ、まえも\nとおったよ
r.m14 random tier=middle | わたしの こえ\nへん？
r.m15 random tier=middle | あめ やまないね
r.m16 random tier=middle | テレビの なかは\nさむいね
r.m17 random tier=middle | もう かえるの？
r.m18 random tier=middle | ないしょ だよ
r.m19 random tier=middle | また あとで ね
# 時々：後の段
r.l01 random tier=late | かえさないよ
r.l02 random tier=late | ずっと ずっと
r.l03 random tier=late | ここに いて
r.l04 random tier=late | みつけた
r.l05 random tier=late | どうして\nとまるの？
r.l06 random tier=late | アハハ
r.l07 random tier=late | いっしょ いっしょ\nいっしょ
r.l08 random tier=late | あなたの へや\nみえるよ
r.l09 random tier=late | うしろの まど
r.l10 random tier=late | ねえ ねえ\nねえ ねえ
r.l11 random tier=late | もう ずっと ここ
r.l12 random tier=late | そとは もう\nない よ
r.l13 random tier=late | わたしが あなた？
r.l14 random tier=late | め を あけて
r.l15 random tier=late | ザザ…ザ…
r.l16 random tier=late | でんき\nけさないで
r.l17 random tier=late | だれも こないよ
r.l18 random tier=late | まだ あそぼ？\nまだ？
# 様子
m.idle1 mood mood=idle | うごかないの？
m.idle2 mood mood=idle | ねちゃった？
m.idle3 mood mood=idle | おーい
m.idle4 mood mood=idle | まってるよ
m.idle5 mood mood=idle | かんがえ ちゅう？
m.died1 mood mood=died | いたい？
m.died2 mood mood=died | もういっかい
m.died3 mood mood=died | へいき へいき
m.died4 mood mood=died | あーあ
m.died5 mood mood=died | だいじょうぶ だよ
m.stuck1 mood mood=stuck | そこ、\nむずかしい？
m.stuck2 mood mood=stuck | ヒント ほしい？
m.stuck3 mood mood=stuck | ゆっくりで いいよ
m.stuck4 mood mood=stuck | いじわる だった？
m.stuck5 mood mood=stuck | がんばって
m.echo1 mood mood=echo | その こ だれ？
m.echo2 mood mood=echo | むらさきの こ、\nわたしに にてる
m.echo3 mood mood=echo | その こ、\nしゃべらないね
m.echo4 mood mood=echo | ふたりも\nいらないよ
# 出来事
e.cp1 event on=checkpoint | ちゃんと おぼえたよ
e.cp2 event on=checkpoint | ここから また\nあそべるね
e.cp3 event on=checkpoint | ちょっと きゅうけい
e.cp4 event on=checkpoint | ひと やすみ
e.cp5 event on=checkpoint | ここ、\nセーブ ポイント？
e.cp6 event on=checkpoint | また ここから ね
e.cp7 event on=checkpoint | わすれないでね\nここ
e.cp8 event on=checkpoint | えらい えらい
e.gem1 event on=gem | きれいでしょ
e.gem2 event on=gem | それ、あげる
e.gem3 event on=gem | たからもの\nみつけた？
e.gem4 event on=gem | ぴかぴか！
e.gem5 event on=gem | あつめてるの？
e.gem6 event on=gem | もっと あるよ
e.mech1 event on=mechanism | うごいた！
e.mech2 event on=mechanism | みて みて
e.mech3 event on=mechanism | テレビが\nうごいたよ
e.mech4 event on=mechanism | わあ…
e.mech5 event on=mechanism | こわれて ない？
e.mech6 event on=mechanism | それ、わたしの
e.area1 event on=area | あたらしい ばしょ！
e.area2 event on=area | ここ、すきなんだ
e.area3 event on=area | ひろいでしょ
e.area4 event on=area | まだ さきが あるよ
e.area5 event on=area | ここ はじめて？
e.area6 event on=area | とおくまで\nきたね
e.area7 event on=area | ここも\nテレビ ばっかり
e.area8 event on=area | ようこそ
e.door1 event on=door | あいたね
e.door2 event on=door | ひらいた！
e.door3 event on=door | すごい すごい
e.door4 event on=door | どうぞ
e.door5 event on=door | とおって いいよ
e.worry1 event on=worry | だいじょうぶ？
# 出来事：操作の案内（覚えるまで出したまま）
g.move event on=move | A と D で\nうごけるよ
g.dash event on=dash | スペースで ダッシュ！\nぶんしんが のこるよ
g.swap event on=swap | スペースを\nながおし すると…
g.swapped event on=swapped | いれかわった！！
# 時々：前の段（道中）
r.e19 random tier=early | この テレビ、\nあったかいね
r.e20 random tier=early | ゆっくりで\nいいよ
r.e21 random tier=early | ぶんしん、\nかわいいね
# 時々：中の段（道中）
r.m20 random tier=middle | ねえ、\nつかれてない？
r.m21 random tier=middle | テレビ、なんだい\nあるか しってる？
r.m22 random tier=middle | ここで はんぶん\nくらい かな
r.m23 random tier=middle | わたしも\nむかし あそんだよ
r.m24 random tier=middle | たまには\nうしろも みてね
# 時々：後の段（道中）
r.l19 random tier=late | もうすぐ\nだよ
r.l20 random tier=late | ゴールの さき、\nしりたい？
r.l21 random tier=late | ずっと\nいっしょだよね？
r.l22 random tier=late | かえさない\nかも
# 様子：残影（道中）
m.echo5 mood mood=echo | その こ、\nおいていくの？
# 時々：いつでも（おまけ：ダジャレ）
o.j01 random tier=any chance=0.015 once=1 | ふとんが\nふっとんだ
o.j02 random tier=any chance=0.015 once=1 | アルミかんの うえに\nある みかん
o.j03 random tier=any chance=0.015 once=1 | トイレに\nいっといれ
o.j04 random tier=any chance=0.015 once=1 | イクラは いくら？
o.j05 random tier=any chance=0.015 once=1 | ねこが\nねころんだ
o.j06 random tier=any chance=0.015 once=1 | カッパが\nかっぱらった
o.j07 random tier=any chance=0.015 once=1 | ダジャレを いったのは\nだれじゃ
o.j08 random tier=any chance=0.015 once=1 | レモンの\nいれもん
o.j09 random tier=any chance=0.015 once=1 | パンダの パンだ
o.j10 random tier=any chance=0.015 once=1 | スキーが すきー
o.j11 random tier=any chance=0.015 once=1 | ダッシュで\nだっしゅつ！
o.j12 random tier=any chance=0.015 once=1 | テレビが\nてれてる
o.j13 random tier=any chance=0.015 once=1 | チャンネルを\nちゃんと ねる
o.j14 random tier=any chance=0.015 once=1 | かがみの まえで\nかがみこむ
o.j15 random tier=any chance=0.015 once=1 | でんちが\nでんちゃった
o.j16 random tier=any chance=0.015 once=1 | ラクダは らくだ
o.j17 random tier=any chance=0.015 once=1 | ハムが はむかう
o.j18 random tier=any chance=0.015 once=1 | …いまの、\nわらうとこ
o.j19 random tier=any chance=0.015 once=1 | さむい？\nここ でんき ないから
# 時々：いつでも（おまけ）
o.x01 random tier=any chance=0.015 once=1 | この メッセージは\n5びょうごに きえる
o.x02 random tier=any chance=0.015 once=1 | うしろ… みた？
o.x03 random tier=any chance=0.015 once=1 | Ctrl+Z は\nないよ
o.x04 random tier=any chance=0.015 once=1 | チャンネル、\nかえないでね
o.x05 random tier=any chance=0.015 once=1 | 1 + 1 =\nぶんしん
o.x06 random tier=any chance=0.015 once=1 | ぶんしんに\nなまえ つけた？
o.x07 random tier=any chance=0.015 once=1 | わたしの ことは\nきにしないで\n…きにして
o.x08 random tier=any chance=0.015 once=1 | すなあらしの むこうに\nだれか いる
o.x09 random tier=any chance=0.015 once=1 | でんきだいが\nこわい
o.x10 random tier=any chance=0.015 once=1 | いま、\nめが あった
o.x11 random tier=any chance=0.015 once=1 | この へやの テレビ、\nぜんぶ わたし
o.x12 random tier=any chance=0.015 once=1 | ねえ、\nなまえ なんだっけ
)";
}
}
}
