// ScoreboardFormat.h - 原生计分板占位符的纯解析/格式化
//
// 本文件刻意不依赖 BDS/LeviLamina：只做「字符串 -> token」与
// 「快照 + 后缀 -> 文本」的纯函数，便于离线单测（tests/scoreboard_format_test.cpp）。
// 真正读取游戏内 Scoreboard 的部分在 ScoreboardAdapter.cpp。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace meowsidebar {

// 原生计分板条目（只读快照）
struct ScoreboardEntry {
    int64_t     id     = 0;   // scoreboardId（me/rank 匹配用）
    std::string name;         // 展示名（假玩家名 / 玩家名 / 实体名）
    int         score  = 0;
    int         type   = 0;   // IdentityDefinition::Type: 1=Player 2=Entity 3=FakePlayer
    bool        hidden = false;
};

// 原生计分板 objective 只读快照
struct ScoreboardSnapshot {
    bool                         valid = false;
    std::string                  name;          // objective 名（命令里的 ID）
    std::string                  displayName;   // 显示名（侧边栏标题）
    std::string                  criteria;      // 判据（dummy 等）
    std::vector<std::string>     displaySlots;  // 当前显示槽：sidebar/belowName/list
    std::vector<ScoreboardEntry> entries;       // 已排序：score 降序，name 升序
};

// 调用者（侧边栏正在渲染的玩家）在计分板里的身份
struct ScoreboardSelf {
    bool    valid = false;
    int64_t id    = 0;
};

enum class ScoreboardTokenKind {
    NotMine, // 不是 {sb.*} / {scoreboard.*}，交给 PAPI
    Parsed,  // 解析成功
};

struct ScoreboardToken {
    bool        sidebar = false; // 引用当前 sidebar 显示槽（{sb.sidebar...}）
    std::string objective;       // 目标 objective 名（不含前缀）
    std::string suffix;          // objective 之后的后缀：""/"count"/"top.1"/条目名...
};

// 解析花括号内文本（不含 '{' '}'）
// knownObjectives 用于最长前缀匹配（objective 名允许包含 '.'）
ScoreboardTokenKind parseScoreboardToken(
    std::string const&              inner,
    std::vector<std::string> const& knownObjectives,
    ScoreboardToken&                out
);

// 按后缀格式化快照；返回 false 表示无法格式化（调用方保留原文）
bool formatScoreboardToken(
    ScoreboardSnapshot const& snap,
    std::string const&        suffix,
    ScoreboardSelf const&     self,
    std::string&              out
);

// 按名查条目下标（先精确，后大小写不敏感）；未找到返回 -1
int findEntryIndex(ScoreboardSnapshot const& snap, std::string const& key);

// 条目分数文本；未找到返回空串
std::string entryScoreText(ScoreboardSnapshot const& snap, std::string const& key);

} // namespace meowsidebar
