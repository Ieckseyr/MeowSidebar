// Config.h - MeowSidebar 配置管理
//
// 配置文件格式（JSON）：
// {
//   "hz": 800,               // 刷新频率（毫秒）
//   "title": [...],          // 标题（支持数组的数组实现动画）
//   "data": [...]            // 数据行（支持数组的数组实现动画）
// }
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <filesystem>

namespace meowsidebar {

namespace fs = std::filesystem;
using json = nlohmann::json;

class ConfigManager {
public:
    static ConfigManager& getInstance();

    // 加载配置文件
    bool load();

    // 重新加载
    bool reload();

    // 保存
    bool save();

    // 获取刷新间隔
    int getHz() const;

    // 获取标题刷新间隔（ms），用于节流标题旋转，避免每 tick 都重建侧边栏
    int getTitleRefreshInterval() const;

    // 原生计分板快照缓存 TTL（ms），默认 500
    // 同一刷新周期内多个 {sb.*} 占位符共用一份快照，避免重复遍历 Scoreboard
    int getScoreboardCacheMs() const;

    // 获取标题配置
    json const& getTitleConfig() const;

    // 获取数据行配置
    json const& getDataConfig() const;

    // 配置目录（相对 DLL 所在目录）
    static fs::path const& getConfigDir();
    static fs::path const& getConfigPath();
    static fs::path const& getPlayerDataDir();

private:
    ConfigManager() = default;
    json mConfig;
};

} // namespace meowsidebar
