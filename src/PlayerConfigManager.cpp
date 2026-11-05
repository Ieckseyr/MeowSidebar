// PlayerConfigManager.cpp - 玩家配置管理器实现
#include "PlayerConfigManager.h"
#include "Config.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace meowsidebar {

namespace fs = std::filesystem;
using json = nlohmann::json;

PlayerConfigManager& PlayerConfigManager::getInstance() {
    static PlayerConfigManager instance;
    return instance;
}

std::string PlayerConfigManager::getFilePath(std::string const& xuid) const {
    return (ConfigManager::getPlayerDataDir() / (xuid + ".json")).string();
}

bool PlayerConfigManager::loadPlayer(std::string const& xuid) {
    try {
        std::string path = getFilePath(xuid);
        if (!fs::exists(path)) {
            std::lock_guard<std::mutex> lock(mMutex);
            mCache[xuid] = PlayerConfig{};
            return true;
        }
        std::ifstream ifs(path);
        if (!ifs.is_open()) return false;
        std::stringstream ss;
        ss << ifs.rdbuf();
        auto j = json::parse(ss.str(), nullptr, false);
        PlayerConfig cfg;
        if (j.is_object()) {
            if (j.contains("customLines") && j["customLines"].is_array()) {
                for (auto const& line : j["customLines"]) {
                    if (line.is_string()) cfg.customLines.push_back(line.get<std::string>());
                }
            }
            // 老数据（BetterSidebar 迁移过来的）写的是 1/0，新数据写 true/false，两种都认
            if (j.contains("sidebarVisible")) {
                auto const& visible = j["sidebarVisible"];
                if (visible.is_boolean()) {
                    cfg.visible = visible.get<bool>();
                } else if (visible.is_number()) {
                    cfg.visible = visible.get<double>() != 0.0;
                } else if (visible.is_string()) {
                    auto const text = visible.get<std::string>();
                    cfg.visible     = !(text == "0" || text == "false" || text == "off");
                }
            }
        }
        std::lock_guard<std::mutex> lock(mMutex);
        mCache[xuid] = std::move(cfg);
        return true;
    } catch (...) {
        return false;
    }
}

bool PlayerConfigManager::savePlayer(std::string const& xuid) {
    try {
        json j;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            auto it = mCache.find(xuid);
            if (it == mCache.end()) return false;
            j["customLines"]    = it->second.customLines;
            j["sidebarVisible"] = it->second.visible ? 1 : 0;
        }
        std::string path = getFilePath(xuid);
        // 确保目录存在
        fs::path dir = fs::path(path).parent_path();
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream ofs(path);
        if (!ofs.is_open()) return false;
        ofs << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

void PlayerConfigManager::loadAll() {
    try {
        auto const& playerDir = ConfigManager::getPlayerDataDir();
        if (!fs::exists(playerDir)) return;
        for (auto const& entry : fs::directory_iterator(playerDir)) {
            if (entry.path().extension() == ".json") {
                std::string stem = entry.path().stem().string();
                loadPlayer(stem);
            }
        }
    } catch (...) {}
}

std::vector<std::string> PlayerConfigManager::getLines(std::string const& xuid) {
    // 先检查缓存
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mCache.find(xuid);
        if (it != mCache.end()) {
            return it->second.customLines;
        }
    }
    // 加载后重试
    loadPlayer(xuid);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mCache.find(xuid);
        if (it != mCache.end()) {
            return it->second.customLines;
        }
    }
    return {};
}

bool PlayerConfigManager::addLine(std::string const& xuid, std::string const& line) {
    if (line.empty()) return false;
    // 确保已加载
    loadPlayer(xuid);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto& cfg = mCache[xuid];
        cfg.customLines.push_back(line);
    }
    return savePlayer(xuid);
}

bool PlayerConfigManager::delLine(std::string const& xuid, int index) {
    loadPlayer(xuid);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mCache.find(xuid);
        if (it == mCache.end()) return false;
        if (index < 0 || index >= static_cast<int>(it->second.customLines.size())) return false;
        it->second.customLines.erase(it->second.customLines.begin() + index);
    }
    return savePlayer(xuid);
}

bool PlayerConfigManager::clearLines(std::string const& xuid) {
    loadPlayer(xuid);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mCache.find(xuid);
        if (it == mCache.end()) return false;
        it->second.customLines.clear();
    }
    return savePlayer(xuid);
}

int PlayerConfigManager::getLineCount(std::string const& xuid) {
    auto lines = getLines(xuid);
    return static_cast<int>(lines.size());
}

bool PlayerConfigManager::isVisible(std::string const& xuid) {
    if (xuid.empty()) return true;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto                        it = mCache.find(xuid);
        if (it != mCache.end()) return it->second.visible;
    }
    // 首次遇到这个玩家：从磁盘读一次并进缓存，之后每拍只查内存
    loadPlayer(xuid);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto                        it = mCache.find(xuid);
        if (it != mCache.end()) return it->second.visible;
    }
    return true;
}

bool PlayerConfigManager::setVisible(std::string const& xuid, bool visible) {
    if (xuid.empty()) return false;
    // 先确保磁盘上的自定义行已进缓存，避免只写回开关时把行覆盖掉
    loadPlayer(xuid);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto                        it = mCache.find(xuid);
        if (it != mCache.end() && it->second.visible == visible) return true;
        mCache[xuid].visible = visible;
    }
    return savePlayer(xuid);
}

} // namespace meowsidebar
