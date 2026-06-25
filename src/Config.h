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

    // 获取配置
    json const& getConfig() const { return mConfig; }

    // 获取刷新间隔
    int getHz() const;

    // 获取标题配置
    json const& getTitleConfig() const;

    // 获取数据行配置
    json const& getDataConfig() const;

    // 配置目录
    static fs::path const& getConfigDir();
    static fs::path const& getConfigPath();
    static fs::path const& getPlayerDataDir();

private:
    ConfigManager() = default;
    json mConfig;
    bool mLoaded = false;

    static fs::path sConfigDir;
    static fs::path sConfigPath;
    static fs::path sPlayerDataDir;
};

} // namespace meowsidebar
