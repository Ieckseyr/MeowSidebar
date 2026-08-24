// SidebarManager.h - 侧边栏核心管理器
//
// 负责：
// 1. 定时刷新（基于配置的 hz 频率，通过 ServerLevelTickEvent 在主线程驱动）
// 2. 配置驱动的标题/数据行渲染（支持数组的数组实现动画效果）
// 3. 通过 MeowPAPI 翻译变量
// 4. 手动构造并发送 SetDisplayObjectivePacket + SetScorePacket
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <mutex>
#include <functional>

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

    // 立即刷新所有在线玩家的侧边栏
    void refreshAll(int deltaMs);

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

    // 处理单个玩家的侧边栏
    void processPlayer(Player* player);

    // 判断玩家是否应该显示侧边栏
    bool shouldShowSidebar(Player* player) const;

    // 发送侧边栏数据包给玩家
    // oldTitle/oldLineCount/wasActive 用于增量更新
    // 全部使用手动字节流 + NetworkPeer::sendPacket 统一发送
    //
    // scoreboardId 策略（稳定 ID）：
    // scoreboardId = SCOREBOARD_ID_BASE + lineIndex，每次刷新复用同一批 ID。
    // 根因已确认：sort=2 错误源于 ProtocolLib rc.1 的 Cereal 数值 variant 序列化，
    // 升级到 rc.2（字符串 tag 序列化）后已修复。稳定 ID 不会产生身份累积
    // （同一批 ID 反复使用，Remove 后 Change 同 ID 由 rc.2 正确翻译给 26.40 客户端）。
    // SCOREBOARD_ID_BASE 偏移避免与 BDS 内部分配的小 ID 冲突。
    void sendSidebarPackets(
        Player* player,
        std::string const& title,
        std::vector<std::string> const& lines,
        std::string const& oldTitle,
        int oldLineCount,
        bool wasActive
    );

    // 清除玩家的侧边栏（发送 RemoveObjective 包，用于在线切换关闭）
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

    // 记录每个玩家上次发送的行（用于增量更新）
    // scoreboardId 使用稳定 ID：SCOREBOARD_ID_BASE + lineIndex，每次刷新复用。
    // 根因已确认 sort=2 源于 ProtocolLib rc.1 序列化问题，rc.2 已修复，
    // 无需 generation 计数器。断连时 erase 条目防止内存泄漏。
    struct PlayerSidebarState {
        std::string title;
        int lineCount = 0;
        bool active = false;
    };
    std::unordered_map<std::string, PlayerSidebarState> mPlayerStates;
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
