// ScoreboardAdapter.h - 原生计分板（游戏内 Scoreboard）只读适配层
//
// 职责：
// 1. 从 BDS 的 Scoreboard 读取 objective / 条目 / 分数（只读，绝不写入）
// 2. 把这些数据以 {sb.*} / {scoreboard.*} 占位符的形式注入侧边栏标题与数据行
// 3. 通过 JSON 查询接口供 /sidebar scoreboard 命令与 RemoteCall API 使用
//
// 线程模型：
// - 读取 BDS 内部结构（Objective::mScores / IdentityDictionary）只能在主线程做
// - 快照带 TTL 缓存（config: scoreboard_cache_ms，默认 500ms），
//   同一刷新周期内多个占位符只读一次 Scoreboard
// - 非主线程调用时只返回缓存副本，绝不触碰 BDS 内部结构
#pragma once

#include "ScoreboardFormat.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class Player;

namespace meowsidebar {

class ScoreboardAdapter {
public:
    // 快照缓存默认 TTL（ms）
    static constexpr int DEFAULT_CACHE_TTL_MS = 500;

    static ScoreboardAdapter& getInstance();

    // 初始化：记录主线程 ID + 读取缓存 TTL（须在主线程调用）
    void init();

    // 停止：清空缓存
    void shutdown();

    // 重载配置：重读缓存 TTL 并清空缓存
    void reloadConfig();

    // 展开字符串中的 {sb.*} / {scoreboard.*} 占位符
    // 未识别的占位符原样保留，交给 MeowPAPI 继续处理
    std::string expand(std::string const& text, Player* player);

    // ===== JSON 查询（RemoteCall / 命令）=====
    // 返回 JSON 数组：[{name, display, criteria, count, slots[]}]
    std::string jsonObjectives();
    // 返回 JSON 对象：{name, display, criteria, count, slots[], entries:[{name,score,type,hidden}]}
    std::string jsonObjective(std::string const& objective);
    // 返回当前 sidebar 显示槽的原生计分板 JSON（无则 "{}"）
    std::string jsonSidebar();
    // 返回某条目在某 objective 里的分数文本（无则空串）
    std::string scoreOf(std::string const& objective, std::string const& entry);

private:
    ScoreboardAdapter() = default;

    struct CacheEntry {
        ScoreboardSnapshot                    snapshot;
        std::chrono::steady_clock::time_point capturedAt{};
    };

    bool isMainThread() const;

    // 直接读取 BDS（仅主线程）
    bool capture(std::string const& objective, ScoreboardSnapshot& out);

    // 带 TTL 缓存的快照
    ScoreboardSnapshot getSnapshot(std::string const& objective);

    // 当前 sidebar 显示槽的快照
    ScoreboardSnapshot getSidebarSnapshot();

    // 已知 objective 名（最长前缀匹配用，带 TTL 缓存）
    std::vector<std::string> getObjectiveNames();

    // 玩家在计分板里的身份（me/rank 用）
    ScoreboardSelf selfFor(Player* player);

    std::atomic<int> mCacheTtlMs{DEFAULT_CACHE_TTL_MS};

    mutable std::mutex                          mCacheMutex;
    std::unordered_map<std::string, CacheEntry> mCache;

    std::vector<std::string>              mObjectiveNames;
    std::chrono::steady_clock::time_point mObjectiveNamesAt{};
    bool                                  mObjectiveNamesCached = false;

    ScoreboardSnapshot mLastSidebarSnapshot;

    std::thread::id mMainThreadId{};
};

} // namespace meowsidebar
