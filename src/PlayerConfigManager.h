// PlayerConfigManager.h - 玩家配置管理器（PCM）
//
// 管理每个玩家的自定义侧边栏行，支持增删查清空。
// 数据存储在 plugins/MeowSidebar/players/<xuid>.json
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace meowsidebar {

class PlayerConfigManager {
public:
    static PlayerConfigManager& getInstance();

    // 加载所有玩家配置（从文件）
    void loadAll();

    // 获取玩家的自定义行
    std::vector<std::string> getLines(std::string const& xuid);

    // 添加一行
    bool addLine(std::string const& xuid, std::string const& line);

    // 删除指定索引的行
    bool delLine(std::string const& xuid, int index);

    // 清空所有自定义行
    bool clearLines(std::string const& xuid);

    // 获取行数
    int getLineCount(std::string const& xuid);

    // 列出所有行（用于显示）
    std::vector<std::string> listLines(std::string const& xuid);

private:
    PlayerConfigManager() = default;

    struct PlayerConfig {
        std::vector<std::string> customLines;
    };

    std::unordered_map<std::string, PlayerConfig> mCache;
    mutable std::mutex mMutex;

    std::string getFilePath(std::string const& xuid) const;
    bool loadPlayer(std::string const& xuid);
    bool savePlayer(std::string const& xuid);
};

} // namespace meowsidebar
