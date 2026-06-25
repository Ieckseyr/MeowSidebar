// MeowSidebar.cpp - 侧边栏插件主模块实现
#include "MeowSidebar.h"
#include "Config.h"
#include "SidebarManager.h"
#include "PlayerConfigManager.h"
#include "DataMigrator.h"

#include "meowpapi/PlaceholderApi.h"
#include "meowpapi/Builtins.h"
#include "meowpapi/RemoteCallBridge.h"
#include "meowpapi/BepApiBridge.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/mod/RegisterHelper.h"

namespace meowsidebar {

// 前向声明（定义在 CommandRegistration.cpp / EventRegistration.cpp 中）
void registerCommands();
void registerEvents(std::vector<ll::event::ListenerPtr>& listeners);

MeowSidebar& MeowSidebar::getInstance() {
    static MeowSidebar instance;
    return instance;
}

MeowSidebar& lib() {
    return MeowSidebar::getInstance();
}

bool MeowSidebar::load() {
    auto& logger = getSelf().getLogger();
    logger.info("MeowSidebar (C++) loading...");
    return true;
}

bool MeowSidebar::enable() {
    auto& logger = getSelf().getLogger();
    logger.info("MeowSidebar (C++) enabling...");

    try {
        // 1. 数据迁移（BetterSidebar → MeowSidebar）
        DataMigrator::migrateIfNeeded();

        // 2. 加载配置
        ConfigManager::getInstance().load();

        // 3. 初始化 PAPI 为服务端模式
        meowpapi::PlaceholderApi::getInstance().initAsServer();

        // 4. 注册内置占位符（MSPT/TPS/服务器/玩家/时间等）
        meowpapi::registerBuiltinPlaceholders();

        // 5. 导出 RemoteCall API（供其他插件调用）
        meowpapi::exportRemoteCallApi();

        // 6. 安装 BEPAPI 兼容层（如果 BEPAPI 已加载）
        meowpapi::installBepApiFallback();

        // 7. 初始化侧边栏管理器
        SidebarManager::getInstance().init();

        // 8. 加载玩家配置
        PlayerConfigManager::getInstance().loadAll();

        // 9. 注册命令
        registerCommands();

        // 10. 注册事件监听
        registerEvents(mListenerPtrs);

        logger.info("MeowSidebar (C++) enabled. PAPI 中心已启动，支持 RemoteCall 注册。");
        logger.info("Usage: /sidebar [on|off|reload|add|del|clear|list]");
    } catch (std::exception const& e) {
        logger.error("MeowSidebar enable failed: {}", e.what());
    } catch (...) {
        logger.error("MeowSidebar enable failed: unknown exception");
    }
    return true;
}

bool MeowSidebar::disable() {
    auto& logger = getSelf().getLogger();
    logger.info("MeowSidebar (C++) disabling...");

    // 停止侧边栏定时刷新
    SidebarManager::getInstance().shutdown();

    // 移除事件监听
    auto& eventBus = ll::event::EventBus::getInstance();
    for (auto& ptr : mListenerPtrs) {
        eventBus.removeListener(ptr);
    }
    mListenerPtrs.clear();

    // 移除 BEPAPI 兼容层
    meowpapi::removeBepApiFallback();

    // 移除 RemoteCall API
    meowpapi::removeRemoteCallApi();

    logger.info("MeowSidebar (C++) disabled.");
    return true;
}

} // namespace meowsidebar

LL_REGISTER_MOD(meowsidebar::MeowSidebar, meowsidebar::MeowSidebar::getInstance());
