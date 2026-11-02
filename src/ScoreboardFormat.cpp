// ScoreboardFormat.cpp - 原生计分板占位符纯解析/格式化实现
#include "ScoreboardFormat.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace meowsidebar {

namespace {

constexpr size_t MAX_TOP_INDEX     = 100; // top.<n> 上限，避免奇葩输入
constexpr size_t MAX_LINES_ENTRIES = 30;  // lines 聚合上限（侧边栏本身也显示不了这么多）

bool startsWith(std::string const& s, char const* prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// 从 s[pos] 开始读一个十进制正整数；成功时 pos 前移
bool parseIndex(std::string const& s, size_t& pos, size_t& out) {
    size_t value = 0;
    size_t start = pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        value = value * 10 + static_cast<size_t>(s[pos] - '0');
        if (value > MAX_TOP_INDEX) return false;
        ++pos;
    }
    if (pos == start) return false;
    out = value;
    return true;
}

void joinInto(std::string& out, std::vector<std::string> const& parts, char const* sep) {
    out.clear();
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
}

} // namespace

int findEntryIndex(ScoreboardSnapshot const& snap, std::string const& key) {
    if (key.empty()) return -1;
    for (size_t i = 0; i < snap.entries.size(); ++i) {
        if (snap.entries[i].name == key) return static_cast<int>(i);
    }
    std::string lowerKey = toLowerAscii(key);
    for (size_t i = 0; i < snap.entries.size(); ++i) {
        if (toLowerAscii(snap.entries[i].name) == lowerKey) return static_cast<int>(i);
    }
    return -1;
}

std::string entryScoreText(ScoreboardSnapshot const& snap, std::string const& key) {
    int idx = findEntryIndex(snap, key);
    if (idx < 0) return {};
    return std::to_string(snap.entries[static_cast<size_t>(idx)].score);
}

ScoreboardTokenKind parseScoreboardToken(
    std::string const&              inner,
    std::vector<std::string> const& knownObjectives,
    ScoreboardToken&                out
) {
    out = {};

    std::string body;
    if (startsWith(inner, "sb.")) {
        body = inner.substr(3);
    } else if (startsWith(inner, "scoreboard.")) {
        body = inner.substr(11);
    } else {
        return ScoreboardTokenKind::NotMine;
    }
    if (body.empty()) return ScoreboardTokenKind::NotMine;

    // {sb.sidebar...} —— 当前 sidebar 显示槽里的原生 objective
    if (body == "sidebar" || startsWith(body, "sidebar.")) {
        out.sidebar = true;
        out.suffix  = body.size() > 7 ? body.substr(8) : std::string{};
        return ScoreboardTokenKind::Parsed;
    }

    // {sb.objective.<name>...} —— 强制按 objective 名解析（避免与 sidebar 关键字冲突）
    bool        explicitObjective = startsWith(body, "objective.");
    std::string spec              = explicitObjective ? body.substr(10) : body;
    if (spec.empty()) return ScoreboardTokenKind::NotMine;

    // 最长前缀匹配：objective 名可以含 '.'，所以优先匹配已知名字
    size_t bestLen = 0;
    for (auto const& name : knownObjectives) {
        if (name.empty() || spec.size() < name.size()) continue;
        if (spec.compare(0, name.size(), name) != 0) continue;
        if (spec.size() != name.size() && spec[name.size()] != '.') continue;
        if (name.size() > bestLen) bestLen = name.size();
    }
    if (bestLen > 0) {
        out.objective = spec.substr(0, bestLen);
        out.suffix    = spec.size() > bestLen ? spec.substr(bestLen + 1) : std::string{};
        return ScoreboardTokenKind::Parsed;
    }
    if (explicitObjective) {
        // 显式写了 objective. 但服务端没有这个 objective
        out.objective = spec;
        out.suffix.clear();
        return ScoreboardTokenKind::Parsed;
    }

    // 兜底：第一段当 objective 名（后续交给快照有效性判定）
    size_t dot    = spec.find('.');
    out.objective = (dot == std::string::npos) ? spec : spec.substr(0, dot);
    out.suffix    = (dot == std::string::npos) ? std::string{} : spec.substr(dot + 1);
    return ScoreboardTokenKind::Parsed;
}

bool formatScoreboardToken(
    ScoreboardSnapshot const& snap,
    std::string const&        suffix,
    ScoreboardSelf const&     self,
    std::string&              out
) {
    if (!snap.valid) return false;

    auto const& entries = snap.entries;

    // 标题 / 元信息
    if (suffix.empty() || suffix == "title" || suffix == "display" || suffix == "name") {
        out = snap.displayName;
        return true;
    }
    if (suffix == "objective" || suffix == "id" || suffix == "key") {
        out = snap.name;
        return true;
    }
    if (suffix == "count" || suffix == "size" || suffix == "entries") {
        out = std::to_string(entries.size());
        return true;
    }
    if (suffix == "criteria") {
        out = snap.criteria;
        return true;
    }
    if (suffix == "slot" || suffix == "slots") {
        joinInto(out, snap.displaySlots, ",");
        return true;
    }

    // 聚合
    if (suffix == "lines") {
        out.clear();
        size_t limit = std::min(entries.size(), MAX_LINES_ENTRIES);
        for (size_t i = 0; i < limit; ++i) {
            if (i) out += '\n';
            out += entries[i].name;
            out += ' ';
            out += std::to_string(entries[i].score);
        }
        return true;
    }
    if (suffix == "names") {
        std::vector<std::string> names;
        names.reserve(entries.size());
        for (auto const& entry : entries) names.push_back(entry.name);
        joinInto(out, names, ", ");
        return true;
    }
    if (suffix == "scores") {
        std::vector<std::string> scores;
        scores.reserve(entries.size());
        for (auto const& entry : entries) scores.push_back(std::to_string(entry.score));
        joinInto(out, scores, ", ");
        return true;
    }

    // 当前玩家
    if (suffix == "me" || suffix == "myscore" || suffix == "my") {
        out.clear();
        if (self.valid) {
            for (auto const& entry : entries) {
                if (entry.id == self.id) {
                    out = std::to_string(entry.score);
                    break;
                }
            }
        }
        return true;
    }
    if (suffix == "rank" || suffix == "myrank") {
        out.clear();
        if (self.valid) {
            for (size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].id == self.id) {
                    out = std::to_string(i + 1);
                    break;
                }
            }
        }
        return true;
    }

    // 排行榜：{sb.<obj>.top.<n>[.name|.score]}
    if (startsWith(suffix, "top.")) {
        out.clear();
        size_t pos = 4;
        size_t n   = 0;
        if (!parseIndex(suffix, pos, n) || n == 0) return true;
        std::string tail = pos < suffix.size() ? suffix.substr(pos) : std::string{};
        size_t      idx  = n - 1;
        bool        ok   = idx < entries.size();
        if (tail == ".name") {
            if (ok) out = entries[idx].name;
            return true;
        }
        if (tail == ".score") {
            if (ok) out = std::to_string(entries[idx].score);
            return true;
        }
        if (!tail.empty()) return true; // 未知 tail -> 空
        if (ok) {
            out = entries[idx].name;
            out += ' ';
            out += std::to_string(entries[idx].score);
        }
        return true;
    }

    // 显式取分数：{sb.<obj>.of.<entry>}（条目名与关键字同名时用）
    if (startsWith(suffix, "of.")) {
        out = entryScoreText(snap, suffix.substr(3));
        return true;
    }

    // 默认：后缀整体当作条目名查分数
    out = entryScoreText(snap, suffix);
    return true;
}

} // namespace meowsidebar
