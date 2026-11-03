// MeowSidebar.cpp - 侧边栏插件主模块实现
#include "MeowSidebar.h"
#include "Config.h"
#include <hologramlib/HologramLib.h>

#include "SidebarManager.h"
#include "PlayerConfigManager.h"
#include "DataMigrator.h"
#include "ScoreboardAdapter.h"

#include "meowpapi/EnsureLoaded.h"
#include "meowpapi/PlaceholderApi.h"
#include "meowpapi/Builtins.h"
#include "meowpapi/RemoteCallBridge.h"
#include "meowpapi/RemoteCallAPI.h"
#include "meowpapi/BepApiBridge.h"
#include "meowpapi/DllExports.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include <mc/world/level/Level.h>
#include <mc/world/actor/player/Player.h>

#include <nlohmann/json.hpp>

namespace meowsidebar {

// ===== MeowSidebar 命名空间 RemoteCall 导出 =====
// ZXDash 等外部插件通过 RemoteCall::hasFunc("MeowSidebar", "reloadConfig") 检测本插件，
// 并通过 importAs 调用以下函数管理侧边栏。
// 导出命名空间为 "MeowSidebar"（与 MeowPAPI 命名空间区分）。
static constexpr const char* SIDEBAR_NS = "MeowSidebar";

static void exportSidebarRemoteCallApi() {
    // reloadConfig — 重载配置（ZXDash 用于检测 + 管理）
    RemoteCall::exportAs(SIDEBAR_NS, "reloadConfig", []() -> bool {
        SidebarManager::getInstance().reloadConfig();
        return true;
    });

    // getPlayerLines — 获取玩家自定义行（返回 JSON 数组字符串）
    RemoteCall::exportAs(SIDEBAR_NS, "getPlayerLines", [](std::string const& xuid) -> std::string {
        auto lines = PlayerConfigManager::getInstance().getLines(xuid);
        nlohmann::json j = nlohmann::json::array();
        for (auto const& l : lines) j.push_back(l);
        return j.dump();
    });

    // addPlayerLine — 添加自定义行
    RemoteCall::exportAs(SIDEBAR_NS, "addPlayerLine", [](std::string const& xuid, std::string const& line) -> bool {
        return PlayerConfigManager::getInstance().addLine(xuid, line);
    });

    // getPlayerLineCount — 获取自定义行数
    RemoteCall::exportAs(SIDEBAR_NS, "getPlayerLineCount", [](std::string const& xuid) -> int {
        return PlayerConfigManager::getInstance().getLineCount(xuid);
    });

    // refreshPlayerByXuid — 通过 XUID 刷新指定玩家的侧边栏
    RemoteCall::exportAs(SIDEBAR_NS, "refreshPlayerByXuid", [](std::string const& xuid) -> bool {
        auto lvl = ll::service::getLevel();
        if (!lvl) return false;
        auto* player = lvl->getPlayerByXuid(xuid);
        if (!player) return false;
        SidebarManager::getInstance().refreshPlayer(player);
        return true;
    });

    // updatePlayerLine — 更新指定索引的自定义行
    RemoteCall::exportAs(SIDEBAR_NS, "updatePlayerLine", [](std::string const& xuid, int index, std::string const& line) -> bool {
        auto lines = PlayerConfigManager::getInstance().getLines(xuid);
        if (index < 0 || index >= (int)lines.size()) return false;
        lines[index] = line;
        PlayerConfigManager::getInstance().clearLines(xuid);
        for (auto const& l : lines) PlayerConfigManager::getInstance().addLine(xuid, l);
        return true;
    });

    // delPlayerLine — 删除指定索引的自定义行
    RemoteCall::exportAs(SIDEBAR_NS, "delPlayerLine", [](std::string const& xuid, int index) -> bool {
        return PlayerConfigManager::getInstance().delLine(xuid, index);
    });

    // clearPlayerLines — 清空自定义行
    RemoteCall::exportAs(SIDEBAR_NS, "clearPlayerLines", [](std::string const& xuid) -> bool {
        return PlayerConfigManager::getInstance().clearLines(xuid);
    });

    // getPlayerSidebarEnabled — 获取玩家侧边栏开关状态
    RemoteCall::exportAs(SIDEBAR_NS, "getPlayerSidebarEnabled", [](std::string const& xuid) -> bool {
        return SidebarManager::getInstance().isPlayerEnabled(xuid);
    });

    // getDefaultShowSidebar — 获取默认显示状态（默认为 true）
    RemoteCall::exportAs(SIDEBAR_NS, "getDefaultShowSidebar", []() -> bool {
        return true;
    });

    // setPlayerSidebarEnabled — 设置玩家侧边栏开关
    RemoteCall::exportAs(SIDEBAR_NS, "setPlayerSidebarEnabled", [](std::string const& xuid, bool enabled) -> bool {
        SidebarManager::getInstance().setPlayerEnabled(xuid, enabled);
        return true;
    });

    // setDefaultShowSidebar — 设置默认显示状态
    RemoteCall::exportAs(SIDEBAR_NS, "setDefaultShowSidebar", [](bool /*enabled*/) -> bool {
        // 当前实现中默认始终为 true，此接口仅为兼容 ZXDash 检测
        return true;
    });

    // ===== 原生计分板（游戏内 Scoreboard）只读查询 =====
    // getScoreboardObjectives — 所有 objective 概要（JSON 数组字符串）
    //   [{name, display, criteria, count, slots:[]}]
    RemoteCall::exportAs(SIDEBAR_NS, "getScoreboardObjectives", []() -> std::string {
        return ScoreboardAdapter::getInstance().jsonObjectives();
    });

    // getScoreboardObjective — 按 objective 名取完整快照（JSON 对象字符串，含条目与分数）
    RemoteCall::exportAs(SIDEBAR_NS, "getScoreboardObjective", [](std::string const& objective) -> std::string {
        return ScoreboardAdapter::getInstance().jsonObjective(objective);
    });

    // getScoreboardSidebar — 当前 sidebar 显示槽里的原生计分板（无则 "{}"）
    RemoteCall::exportAs(SIDEBAR_NS, "getScoreboardSidebar", []() -> std::string {
        return ScoreboardAdapter::getInstance().jsonSidebar();
    });

    // getScoreboardScore — 某 objective 中某条目的分数文本（无则空串）
    RemoteCall::exportAs(
        SIDEBAR_NS,
        "getScoreboardScore",
        [](std::string const& objective, std::string const& entry) -> std::string {
            return ScoreboardAdapter::getInstance().scoreOf(objective, entry);
        }
    );

    // expandScoreboard — 展开 {sb.*}/{scoreboard.*} 占位符（供其他插件复用同一套语法）
    // playerName 可为空；给定时 {sb.<obj>.me}/{sb.<obj>.rank} 才有值
    RemoteCall::exportAs(
        SIDEBAR_NS,
        "expandScoreboard",
        [](std::string const& text, std::string const& playerName) -> std::string {
            if (text.empty()) return text;
            Player* player = nullptr;
            if (!playerName.empty()) {
                if (auto level = ll::service::getLevel()) player = level->getPlayer(playerName);
            }
            return ScoreboardAdapter::getInstance().expand(text, player);
        }
    );
}

static void removeSidebarRemoteCallApi() {
    RemoteCall::removeNameSpace(SIDEBAR_NS);
}

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

    // 标准化模式:在 load() 阶段触发 MeowPAPI 部署 + 自动初始化
    // ensureLoaded() 会自动执行:initAsServer + registerBuiltinPlaceholders +
    // exportRemoteCallApi + installBepApiFallback
    if (!meowpapi::ensureLoaded()) {
        logger.error("MeowPAPI 加载失败,侧边栏系统无法工作");
        return false;
    }
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

        // 3. MeowPAPI 已在 load() 阶段通过 ensureLoaded() 完成部署和自动初始化
        //    (initAsServer + registerBuiltinPlaceholders + exportRemoteCallApi + installBepApiFallback)
        //    此处无需重复调用上述函数
        //    运行时 ABI 诊断（新版本 ABI 调用）：检测已加载 MeowPAPI.dll 的
        //    ABI 版本/功能位/构建时间戳，与编译期 ABI 对照，秒判部署版本
        {
            uint32_t rtAbi     = meowpapi::loadedAbiVersion();
            uint64_t rtBuildTs = meowpapi::loadedBuildTimestamp();
            logger.info(
                "MeowPAPI 运行时: ABI 0x{:06x}{} (本插件编译期 0x{:06x}), build {}",
                rtAbi,
                meowpapi::paramPapiSupported() ? " 带参PAPI" : "",
                MEOWPAPI_ABI_VERSION,
                rtBuildTs > 0 ? std::to_string(rtBuildTs) : "unknown"
            );
            if (rtAbi > 0 && rtAbi < MEOWPAPI_ABI_VERSION) {
                logger.warn(
                    "运行中的 MeowPAPI ABI 0x{:06x} 低于本插件编译期 ABI 0x{:06x}，部分功能可能降级",
                    rtAbi, MEOWPAPI_ABI_VERSION
                );
            }
        }

        // 4. 导出 MeowSidebar 命名空间的 RemoteCall API（供 ZXDash 等检测和管理）
        exportSidebarRemoteCallApi();

        // 5. 初始化侧边栏管理器
        SidebarManager::getInstance().init();

        // 6. 原生计分板适配自检：真正读一次 Scoreboard，
        //     验证 26.51 ABI 访问正常（读不到只会得到 0 个 objective，不会崩溃）
        {
            auto&       adapter        = ScoreboardAdapter::getInstance();
            size_t      objectiveCount = 0;
            std::string objectivePreview;
            std::string sidebarDesc = "未占用";
            try {
                auto objectives = nlohmann::json::parse(adapter.jsonObjectives());
                if (objectives.is_array()) {
                    objectiveCount = objectives.size();
                    size_t shown   = 0;
                    for (auto const& item : objectives) {
                        if (shown >= 10) {
                            objectivePreview += ",...";
                            break;
                        }
                        if (shown) objectivePreview += ", ";
                        objectivePreview += item.value("name", std::string{});
                        ++shown;
                    }
                }
                auto sidebar = nlohmann::json::parse(adapter.jsonSidebar());
                if (sidebar.is_object() && sidebar.contains("name")) {
                    sidebarDesc = sidebar.value("name", std::string{}) + " ("
                                  + std::to_string(sidebar.value("count", 0)) + " 条)";
                }
            } catch (...) {
            }
            logger.info(
                "原生计分板适配就绪: {} 个 objective{}, sidebar 槽显示: {}",
                objectiveCount,
                objectivePreview.empty() ? std::string{} : " [" + objectivePreview + "]",
                sidebarDesc
            );
        }

        // 7. 加载玩家配置
        PlayerConfigManager::getInstance().loadAll();
        // 8. 注册命令
        registerCommands();

        // 9. 注册事件监听
        registerEvents(mListenerPtrs);

        {
            auto const runtimeAbi = hologramlib::IHologramLib::getInstance().version();
            if (!hologramlib::abiCompatible(runtimeAbi)) {
                logger.error(
                    "MeowSidebar 按 0x{:06X} 编译，当前 HologramLib 只有 0x{:06X} → 拒绝启用（重装配套版本）",
                    static_cast<std::uint32_t>(HOLOGLIB_API_VERSION),
                    static_cast<std::uint32_t>(runtimeAbi)
                );
                return false;
            }
        }
        logger.info(
            "MeowSidebar (C++) enabled. PAPI 中心已启动，支持 RemoteCall 注册。HologramLib ABI 0x{:06X}",
            hologramlib::IHologramLib::getInstance().version()
        );
        logger.info("Usage: /sidebar [on|off|reload|add|del|clear|list|scoreboard|expand]");
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

    // 标准化模式:仅清理本插件的 RemoteCall 命名空间
    removeSidebarRemoteCallApi();

    // 仅当本插件是 MeowPAPI 的部署者时,才执行全局清理
    // (BEPAPI 兼容层 + MeowPAPI 命名空间 RemoteCall API)
    // 非部署者(其他消费者已先部署 MeowPAPI)不应清理全局资源
    if (meowpapi::isDeployer()) {
        meowpapi::removeBepApiFallback();
        meowpapi::removeRemoteCallApi();
        logger.info("本插件为 MeowPAPI 部署者,已清理全局资源");
    }

    logger.info("MeowSidebar (C++) disabled.");
    return true;
}

} // namespace meowsidebar

LL_REGISTER_MOD(meowsidebar::MeowSidebar, meowsidebar::MeowSidebar::getInstance());
