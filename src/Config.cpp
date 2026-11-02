// Config.cpp - MeowSidebar 配置管理实现
#include "Config.h"
#include "MeowSidebar.h"

#include <fstream>
#include <sstream>

namespace meowsidebar {

fs::path ConfigManager::sConfigDir     = "plugins/MeowSidebar";
fs::path ConfigManager::sConfigPath    = "plugins/MeowSidebar/config.json";
fs::path ConfigManager::sPlayerDataDir = "plugins/MeowSidebar/players";

// 默认配置
static json const DEFAULT_CONFIG = {
    {"hz", 800},
    {"scoreboard_cache_ms", 500},
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
            std::ifstream ifs(sConfigPath, std::ios::binary);
            if (!ifs.is_open()) {
                mConfig = DEFAULT_CONFIG;
                return false;
            }
            std::stringstream ss;
            ss << ifs.rdbuf();
            std::string content = ss.str();

            // 剥离 UTF-8 BOM（记事本等编辑器保存时会写入 BOM，导致 JSON 解析失败）
            if (content.size() >= 3
                && (unsigned char)content[0] == 0xEF
                && (unsigned char)content[1] == 0xBB
                && (unsigned char)content[2] == 0xBF) {
                content.erase(0, 3);
            }

            try {
                mConfig = json::parse(content, nullptr, true);
            } catch (std::exception const& e) {
                // 解析失败：先备份用户配置再回退默认，绝不静默丢弃用户数据
                auto& logger = lib().getSelf().getLogger();
                logger.warn(
                    "config.json 解析失败({}),原文件已备份为 config.json.bak,已生成默认配置",
                    e.what()
                );
                std::error_code bec;
                fs::copy_file(
                    sConfigPath,
                    sConfigPath.string() + ".bak",
                    fs::copy_options::overwrite_existing,
                    bec
                );
                mConfig = DEFAULT_CONFIG;
                save();
            }
        }

        // 确保必要字段存在
        if (!mConfig.contains("hz")) mConfig["hz"] = 800;
        if (!mConfig.contains("title")) mConfig["title"] = DEFAULT_CONFIG["title"];
        if (!mConfig.contains("data")) mConfig["data"] = DEFAULT_CONFIG["data"];

        return true;
    } catch (std::exception const&) {
        mConfig = DEFAULT_CONFIG;
        return false;
    }
}

bool ConfigManager::reload() {
    return load();
}

bool ConfigManager::save() {
    try {
        // 原子写：先写临时文件再替换，避免写入中途异常/崩溃损坏 config.json
        fs::path tmpPath = sConfigPath.string() + ".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::binary);
            if (!ofs.is_open()) return false;
            ofs << mConfig.dump(2);
            if (!ofs.good()) return false;
        }
        std::error_code ec;
        fs::rename(tmpPath, sConfigPath, ec);
        if (ec) {
            // rename 失败（目标被占用等），退回直接写
            std::ofstream ofs(sConfigPath, std::ios::binary);
            if (!ofs.is_open()) return false;
            ofs << mConfig.dump(2);
            fs::remove(tmpPath, ec);
        }
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

int ConfigManager::getTitleRefreshInterval() const {
    try {
        // 默认 0 = 不节流（每次 tick 都旋转标题）
        return mConfig.value("title_refresh_interval", 0);
    } catch (...) {
        return 0;
    }
}

int ConfigManager::getScoreboardCacheMs() const {
    try {
        return mConfig.value("scoreboard_cache_ms", 500);
    } catch (...) {
        return 500;
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
