// DataMigrator.cpp - 数据迁移实现
#include "DataMigrator.h"
#include "Config.h"

#include <filesystem>
#include <fstream>

namespace meowsidebar {

namespace fs = std::filesystem;

static constexpr char const* MIGRATION_FLAG = ".migrated";
// 旧版 BetterSidebar 玩家数据目录
static constexpr char const* OLD_PLAYER_DIR = "plugins/Meow/plugins/BetterSidebar/players";

void DataMigrator::migrateIfNeeded() {
    try {
        if (hasMigrated()) return;
        doMigration();
        markMigrated();
    } catch (...) {}
}

bool DataMigrator::hasMigrated() {
    auto flagPath = ConfigManager::getConfigDir() / MIGRATION_FLAG;
    return fs::exists(flagPath);
}

void DataMigrator::markMigrated() {
    try {
        auto flagPath = ConfigManager::getConfigDir() / MIGRATION_FLAG;
        std::error_code ec;
        fs::create_directories(ConfigManager::getConfigDir(), ec);
        std::ofstream ofs(flagPath);
        if (ofs.is_open()) {
            ofs << "1";
        }
    } catch (...) {}
}

bool DataMigrator::doMigration() {
    try {
        fs::path oldDir(OLD_PLAYER_DIR);
        if (!fs::exists(oldDir)) return true;  // 没有旧数据，无需迁移

        auto const& newDir = ConfigManager::getPlayerDataDir();
        std::error_code ec;
        fs::create_directories(newDir, ec);

        int count = 0;
        for (auto const& entry : fs::directory_iterator(oldDir)) {
            if (entry.path().extension() == ".json") {
                fs::path target = newDir / entry.path().filename();
                if (!fs::exists(target)) {
                    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
                    if (!ec) count++;
                }
            }
        }

        if (count > 0) {
            // 输出日志（通过 MeowSidebar 主类）
            // 这里简单输出到控制台
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace meowsidebar
