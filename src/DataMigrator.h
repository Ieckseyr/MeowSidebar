// DataMigrator.h - 数据迁移工具
//
// 将旧版 BetterSidebar 的玩家配置迁移到 MeowSidebar
// 源路径: plugins/Meow/plugins/BetterSidebar/players/
// 目标路径: plugins/MeowSidebar/players/
#pragma once

namespace meowsidebar {

class DataMigrator {
public:
    // 检查并执行迁移（仅首次启动时执行）
    static void migrateIfNeeded();

private:
    // 执行实际迁移
    static bool doMigration();

    // 检查是否已经迁移过
    static bool hasMigrated();

    // 标记已迁移
    static void markMigrated();
};

} // namespace meowsidebar
