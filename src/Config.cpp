// Config.cpp - MeowSidebar 配置管理实现
#include "Config.h"

#include <fstream>
#include <sstream>

namespace meowsidebar {

fs::path ConfigManager::sConfigDir     = "plugins/MeowSidebar";
fs::path ConfigManager::sConfigPath    = "plugins/MeowSidebar/config.json";
fs::path ConfigManager::sPlayerDataDir = "plugins/MeowSidebar/players";

// 默认配置
static json const DEFAULT_CONFIG = {
    {"hz", 800},
    {"title", json::array({
        "§1§g",
        json::array({"|", "/", "-", "\\"}),
        "§5欢迎您加入! §g",
        json::array({"|", "/", "-", "\\"})
    })},
    {"data", json::array({
        "§e#§d#§b#§g#个§e人§d信§b息§e#§d#§b#§g#",
        "§e你好,§g{pl.realName}",
        "§g血量: §c{pl.health}/§f{pl.maxHealth}",
        "§g饥饿: §e{pl.hunger}",
        "§g坐标: §b{pl.pos}",
        "§g维度: §a{pl.dimName}",
        "§g等级: §d{pl.expLevel}",
        "§e#§d#§b#§g#服§e务§d器§b信§e息§d#§b#§g#",
        "§g在线: §a{server_online}§g/§e{server_max_players}",
        "§gTPS: §a{server_tps} §gMSPT: §e{server_mspt}",
        "§g版本: §b{server_version}",
        "§g运行: §d{server_uptime}",
        "§e#§d#§b#§g#时§e间§d信§b息§e#§d#§b#§g#",
        "§g日期: §c{date.Y}§g-§e{date.M}§g-§b{date.D}§g(§d{date.W}§g)",
        "§g时间: §c{date.h}§g:§e{date.m}§g:§b{date.s}",
        json::array({
            "§gTps : §a{server_tps}§g  ♪  ",
            "§gTps : §a{server_tps}§g ♪   ",
            "§gTps : §a{server_tps}§g♪    ",
            "§gTps : §a{server_tps}§g ♪   ",
            "§gTps : §a{server_tps}§g  ♪  "
        })
    })}
};

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::load() {
    try {
        // 确保目录存在
        std::error_code ec;
        fs::create_directories(sConfigDir, ec);
        fs::create_directories(sPlayerDataDir, ec);

        if (!fs::exists(sConfigPath)) {
            // 写入默认配置
            mConfig = DEFAULT_CONFIG;
            save();
        } else {
            std::ifstream ifs(sConfigPath);
            if (!ifs.is_open()) {
                mConfig = DEFAULT_CONFIG;
                return false;
            }
            std::stringstream ss;
            ss << ifs.rdbuf();
            mConfig = json::parse(ss.str(), nullptr, true);
        }

        // 确保必要字段存在
        if (!mConfig.contains("hz")) mConfig["hz"] = 800;
        if (!mConfig.contains("title")) mConfig["title"] = DEFAULT_CONFIG["title"];
        if (!mConfig.contains("data")) mConfig["data"] = DEFAULT_CONFIG["data"];

        mLoaded = true;
        return true;
    } catch (std::exception const& e) {
        mConfig = DEFAULT_CONFIG;
        mLoaded = true;
        return false;
    }
}

bool ConfigManager::reload() {
    return load();
}

bool ConfigManager::save() {
    try {
        std::ofstream ofs(sConfigPath);
        if (!ofs.is_open()) return false;
        ofs << mConfig.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

int ConfigManager::getHz() const {
    try {
        return mConfig.value("hz", 800);
    } catch (...) {
        return 800;
    }
}

json const& ConfigManager::getTitleConfig() const {
    static json empty;
    try {
        return mConfig.at("title");
    } catch (...) {
        return empty;
    }
}

json const& ConfigManager::getDataConfig() const {
    static json empty;
    try {
        return mConfig.at("data");
    } catch (...) {
        return empty;
    }
}

fs::path const& ConfigManager::getConfigDir() { return sConfigDir; }
fs::path const& ConfigManager::getConfigPath() { return sConfigPath; }
fs::path const& ConfigManager::getPlayerDataDir() { return sPlayerDataDir; }

} // namespace meowsidebar
