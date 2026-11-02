// scoreboard_format_test.cpp - 原生计分板占位符解析/格式化的离线单测
//
// 只链接 src/ScoreboardFormat.cpp（纯逻辑，无 BDS/LeviLamina 依赖）。
// 运行: xmake run scoreboard_format_test
#include "ScoreboardFormat.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace meowsidebar;

namespace {

int gFailures = 0;
int gChecks   = 0;

void check(bool ok, char const* what, std::string const& got, std::string const& want) {
    ++gChecks;
    if (ok) return;
    ++gFailures;
    std::printf("[FAIL] %s\n        got  = \"%s\"\n        want = \"%s\"\n", what, got.c_str(), want.c_str());
}

void checkStr(std::string const& got, std::string const& want, char const* what) {
    check(got == want, what, got, want);
}

void checkInt(int got, int want, char const* what) {
    check(got == want, what, std::to_string(got), std::to_string(want));
}

ScoreboardSnapshot makeSnapshot() {
    ScoreboardSnapshot snap;
    snap.valid        = true;
    snap.name         = "rank";
    snap.displayName  = "§6在线排行";
    snap.criteria     = "dummy";
    snap.displaySlots = {"sidebar", "list"};

    snap.entries = {
        {1001, "Alice",    30, 1, false},
        {1002, "Bob",      20, 1, false},
        {1003, "§7fake",   10, 3, false},
        {1004, "count",     5, 3, false},
    };
    return snap;
}

void testParse() {
    std::vector<std::string> const known = {"rank", "money", "my.obj"};

    ScoreboardToken token;

    // 非本适配器 token -> 交给 PAPI
    checkInt(
        static_cast<int>(parseScoreboardToken("pl.money", known, token)),
        static_cast<int>(ScoreboardTokenKind::NotMine),
        "parse: {pl.money} 不属于计分板"
    );
    checkInt(
        static_cast<int>(parseScoreboardToken("sb.", known, token)),
        static_cast<int>(ScoreboardTokenKind::NotMine),
        "parse: {sb.} 非法"
    );

    // 基础
    checkInt(
        static_cast<int>(parseScoreboardToken("sb.rank", known, token)),
        static_cast<int>(ScoreboardTokenKind::Parsed),
        "parse: {sb.rank}"
    );
    checkStr(token.objective, "rank", "parse: objective=rank");
    checkStr(token.suffix, "", "parse: suffix 为空");

    // 后缀
    parseScoreboardToken("sb.rank.top.3.name", known, token);
    checkStr(token.objective, "rank", "parse: rank.top.3.name objective");
    checkStr(token.suffix, "top.3.name", "parse: rank.top.3.name suffix");

    // sidebar 显示槽
    parseScoreboardToken("sb.sidebar", known, token);
    check(token.sidebar, "parse: sidebar 标记", token.sidebar ? "1" : "0", "1");
    checkStr(token.suffix, "", "parse: sidebar 无后缀");
    parseScoreboardToken("sb.sidebar.top.2", known, token);
    checkStr(token.suffix, "top.2", "parse: sidebar.top.2 suffix");

    // 长前缀别名
    parseScoreboardToken("scoreboard.money.count", known, token);
    checkStr(token.objective, "money", "parse: scoreboard 别名 objective");
    checkStr(token.suffix, "count", "parse: scoreboard 别名 suffix");

    // objective 名含 '.'：最长前缀优先
    parseScoreboardToken("sb.my.obj.top.1", known, token);
    checkStr(token.objective, "my.obj", "parse: 带点 objective 最长前缀");
    checkStr(token.suffix, "top.1", "parse: 带点 objective suffix");

    // 显式 objective. 前缀（避免与 sidebar 关键字冲突）
    parseScoreboardToken("sb.objective.sidebar", known, token);
    check(!token.sidebar, "parse: objective.sidebar 不走显示槽", token.sidebar ? "1" : "0", "0");
    checkStr(token.objective, "sidebar", "parse: objective.sidebar 名字");
}

void testFormat() {
    ScoreboardSnapshot const snap = makeSnapshot();
    ScoreboardSelf         self;
    self.valid = true;
    self.id    = 1002; // Bob

    std::string out;

    // 元信息
    check(formatScoreboardToken(snap, "", self, out), "format: 空后缀", "", "");
    checkStr(out, "§6在线排行", "format: 显示名");
    formatScoreboardToken(snap, "title", self, out);
    checkStr(out, "§6在线排行", "format: title 别名");
    formatScoreboardToken(snap, "objective", self, out);
    checkStr(out, "rank", "format: objective 名");
    formatScoreboardToken(snap, "count", self, out);
    checkStr(out, "4", "format: count");
    formatScoreboardToken(snap, "criteria", self, out);
    checkStr(out, "dummy", "format: criteria");
    formatScoreboardToken(snap, "slots", self, out);
    checkStr(out, "sidebar,list", "format: slots");

    // 排行
    formatScoreboardToken(snap, "top.1", self, out);
    checkStr(out, "Alice 30", "format: top.1");
    formatScoreboardToken(snap, "top.1.name", self, out);
    checkStr(out, "Alice", "format: top.1.name");
    formatScoreboardToken(snap, "top.2.score", self, out);
    checkStr(out, "20", "format: top.2.score");
    formatScoreboardToken(snap, "top.9", self, out);
    checkStr(out, "", "format: 越界 top.9");
    formatScoreboardToken(snap, "top.0", self, out);
    checkStr(out, "", "format: top.0 非法");

    // 聚合
    formatScoreboardToken(snap, "lines", self, out);
    checkStr(out, "Alice 30\nBob 20\n§7fake 10\ncount 5", "format: lines 多行");
    formatScoreboardToken(snap, "names", self, out);
    checkStr(out, "Alice, Bob, §7fake, count", "format: names");
    formatScoreboardToken(snap, "scores", self, out);
    checkStr(out, "30, 20, 10, 5", "format: scores");

    // 按条目名查分
    formatScoreboardToken(snap, "Alice", self, out);
    checkStr(out, "30", "format: 按名查分");
    formatScoreboardToken(snap, "alice", self, out);
    checkStr(out, "30", "format: 大小写不敏感");
    formatScoreboardToken(snap, "nobody", self, out);
    checkStr(out, "", "format: 条目不存在");

    // 关键字同名条目：默认被元信息占用，用 of. 显式取分
    formatScoreboardToken(snap, "count", self, out);
    checkStr(out, "4", "format: count 关键字优先");
    formatScoreboardToken(snap, "of.count", self, out);
    checkStr(out, "5", "format: of.count 取条目分数");

    // 当前玩家
    formatScoreboardToken(snap, "me", self, out);
    checkStr(out, "20", "format: me");
    formatScoreboardToken(snap, "rank", self, out);
    checkStr(out, "2", "format: rank");
    formatScoreboardToken(snap, "me", ScoreboardSelf{}, out);
    checkStr(out, "", "format: 无身份时 me 为空");

    // 无效快照
    ScoreboardSnapshot invalid;
    check(!formatScoreboardToken(invalid, "count", self, out), "format: 无效快照返回 false", "", "");

    // 条目查找
    checkInt(findEntryIndex(snap, "Bob"), 1, "findEntryIndex: Bob");
    checkInt(findEntryIndex(snap, "BOB"), 1, "findEntryIndex: 大小写");
    checkInt(findEntryIndex(snap, "ghost"), -1, "findEntryIndex: 不存在");
    checkInt(findEntryIndex(snap, ""), -1, "findEntryIndex: 空键");
    checkStr(entryScoreText(snap, "§7fake"), "10", "entryScoreText: 带色码假玩家");
}

} // namespace

int main() {
    testParse();
    testFormat();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
