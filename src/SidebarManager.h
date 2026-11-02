// SidebarManager.h - 侧边栏核心管理器
//
// 负责：
// 1. 定时刷新（基于配置的 hz 频率，通过 ServerLevelTickEvent 在主线程驱动）
// 2. 配置驱动的标题/数据行渲染（支持数组的数组实现动画效果）
// 3. 通过 MeowPAPI 翻译变量
// 4. 把渲染结果交给 HologramLib 的 ISidebar 显示（26.60 起本插件不再自己构造/发送协议包）
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <mutex>

#include "ll/api/event/ListenerBase.h"

class Player;

namespace meowsidebar {

using json = nlohmann::json;

class SidebarManager {
public:
    static SidebarManager& getInstance();

    // 初始化（注册主线程 tick 事件监听器）
    void init();

    // 停止（移除 tick 事件监听器，清除所有侧边栏）
    void shutdown();

    // 重新加载配置
    void reloadConfig();

    // 立即刷新指定玩家的侧边栏
    void refreshPlayer(Player* player);

    // 移除指定玩家的侧边栏（在线切换关闭，发包清除客户端显示）
    void removeSidebar(Player* player);

    // 玩家断连清理：不发包（连接已关闭），直接 erase 状态条目防止内存泄漏
    void onPlayerDisconnect(Player* player);

    // 设置玩家的侧边栏开关状态
    void setPlayerEnabled(std::string const& xuid, bool enabled);
    bool isPlayerEnabled(std::string const& xuid) const;

private:
    SidebarManager() = default;

    // 主线程 tick 事件回调
    void onTick();

    // 刷新所有在线玩家的侧边栏（tick 驱动，内部使用）
    void refreshAll(int deltaMs);

    // 处理单个玩家的侧边栏
    void processPlayer(Player* player);

    // 判断玩家是否应该显示侧边栏
    bool shouldShowSidebar(Player* player) const;

    // 把"这一拍该显示什么"交给 HologramLib（26.60 起侧边栏不再自己构造发包）
    //
    // 协议层（SetDisplayObjective 107 / SetScore 108 / RemoveObjective 106 的编码、
    // 回读校验、objective 名按玩家派生、行数变化时的旧行清理）全部在 HologramLib 的
    // ISidebar 里。本插件只负责渲染配置 + 占位符，产出 title/lines。
    // 详见 HologramLib/docs/lse/sidebar.md 与 src/scoreboard/SidebarManager.cpp。
    void sendSidebarPackets(
        Player* player,
        std::string const& title,
        std::vector<std::string> const& lines
    );

    // 清除玩家的侧边栏（HologramLib 侧发 RemoveObjective）
    void sendRemoveSidebarPackets(Player* player);

    // ===== Array Index Manager (AIM) =====
    // 用于处理配置中数组的数组（子数组循环切换实现动画）
    struct AIMIndex {
        int current  = 0;
        int max      = 0;
        int interval = 0;  // 自定义旋转间隔（ms），0 = 每次刷新都旋转
        int elapsed  = 0;  // 已累计的时间（ms）
    };

    struct ArrayIndexManager {
        std::vector<AIMIndex> indexes;  // 按 path 索引
        json sourceArray;

        void rebuild(json const& arr);
        // 获取当前值：每个顶层元素如果是数组，取循环当前值；否则直接返回
        std::vector<std::string> getCurrentValues() const;
        // 所有循环索引前进一步（deltaMs 为本次距上次的毫秒数）
        void incrementAll(int deltaMs);
    };

    ArrayIndexManager mTitleAIM;
    ArrayIndexManager mDataAIM;
    mutable std::mutex mAIMMutex;       // 保护 mTitleAIM/mDataAIM 的并发访问
    int mTitleElapsedMs = 0;            // 标题旋转节流计时器（累计 ms）

    // 当前已显示侧边栏的玩家 XUID 集合：用于判断是否已显示，避免重复发移除包。
    // 协议层的增量 diff / 旧行清理都在 HologramLib 的 ISidebar 内；
    // 断连时 erase 条目防止内存泄漏。
    std::unordered_set<std::string> mActivePlayers;
    mutable std::mutex mStateMutex;

    // 玩家开关状态
    std::unordered_map<std::string, bool> mPlayerEnabled;
    mutable std::mutex mEnabledMutex;

    // 主线程 tick 事件驱动
    ll::event::ListenerPtr mTickListener = 0;
    std::chrono::steady_clock::time_point mLastRefreshTime;
    bool mLastRefreshTimeValid = false;
};

} // namespace meowsidebar
