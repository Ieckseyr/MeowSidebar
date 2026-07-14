// SidebarManager.h - 侧边栏核心管理器
//
// 负责：
// 1. 定时刷新（基于配置的 hz 频率）
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
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>

class Player;

namespace meowsidebar {

using json = nlohmann::json;

class SidebarManager {
public:
    static SidebarManager& getInstance();

    // 初始化（启动定时线程）
    void init();

    // 停止（终止定时线程）
    void shutdown();

    // 重新加载配置
    void reloadConfig();

    // 立即刷新所有在线玩家的侧边栏
    void refreshAll();

    // 立即刷新指定玩家的侧边栏
    void refreshPlayer(Player* player);

    // 移除指定玩家的侧边栏
    void removeSidebar(Player* player);

    // 设置玩家的侧边栏开关状态
    void setPlayerEnabled(std::string const& xuid, bool enabled);
    bool isPlayerEnabled(std::string const& xuid) const;

private:
    SidebarManager() = default;

    // 定时刷新线程
    void tickLoop();

    // 处理单个玩家的侧边栏
    void processPlayer(Player* player);

    // 判断玩家是否应该显示侧边栏
    bool shouldShowSidebar(Player* player) const;

    // 发送侧边栏数据包给玩家
    // oldTitle/oldLines/wasActive 用于增量更新（移除旧条目，避免内容残留）
    void sendSidebarPackets(
        Player* player,
        std::string const& title,
        std::vector<std::string> const& lines,
        std::string const& oldTitle,
        std::vector<std::string> const& oldLines,
        bool wasActive
    );

    // 清除玩家的侧边栏（发送空包）
    void sendRemoveSidebarPackets(Player* player);

    // ===== Array Index Manager (AIM) =====
    // 用于处理配置中数组的数组（子数组循环切换实现动画）
    struct AIMIndex {
        int current = 0;
        int max     = 0;
    };

    struct ArrayIndexManager {
        std::vector<AIMIndex> indexes;  // 按 path 索引
        json sourceArray;

        void rebuild(json const& arr);
        // 获取当前值：每个顶层元素如果是数组，取循环当前值；否则直接返回
        std::vector<std::string> getCurrentValues() const;
        // 所有循环索引前进一步
        void incrementAll();
    };

    ArrayIndexManager mTitleAIM;
    ArrayIndexManager mDataAIM;

    // 记录每个玩家上次发送的行（用于增量更新）
    struct PlayerSidebarState {
        std::string title;
        std::vector<std::string> lines;
        bool active = false;
    };
    std::unordered_map<std::string, PlayerSidebarState> mPlayerStates;
    mutable std::mutex mStateMutex;

    // 玩家开关状态
    std::unordered_map<std::string, bool> mPlayerEnabled;
    mutable std::mutex mEnabledMutex;

    // 定时线程
    std::thread mTickThread;
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mShuttingDown{false};
};

} // namespace meowsidebar
