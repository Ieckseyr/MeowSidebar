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
#include "ll/api/thread/ServerThreadExecutor.h"

#include <mc/world/level/Level.h>
#include <mc/world/actor/player/Player.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>

namespace meowsidebar {

// MeowPAPI 是否已链上（内嵌部署成功 / 连上已加载的 MeowPAPI.dll）
static std::atomic<bool> gPapiReady{false};

bool papiReady() {
    return gPapiReady.load(std::memory_order_relaxed);
}

// 非主线程调用时把活儿丢回服务端主线程执行（LSE/RemoteCall 线程不能碰 BDS 结构）
static void runOnServerThread(std::function<void()> fn) {
    auto& manager = SidebarManager::getInstance();
    if (manager.isServerThread()) {
        fn();
        return;
    }
    try {
        ll::thread::ServerThreadExecutor::getDefault().execute(std::move(fn));
    } catch (...) {
        // 服务端正在收尾/执行器已销毁：丢弃这次刷新，不影响调用方
    }
}

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
        if (xuid.empty()) return false;
        // LSE / 其他插件线程直接进来时不能碰 Player 与发包，丢回服务端主线程
        if (!SidebarManager::getInstance().isServerThread()) {
            std::string copy = xuid;
            runOnServerThread([copy] {
                auto level = ll::service::getLevel();
                if (!level) return;
                if (auto* player = level->getPlayerByXuid(copy)) {
                    SidebarManager::getInstance().refreshPlayer(player);
                }
            });
            return true; // 已排队，下一拍执行
        }
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

    // setPlayerSidebarEnabled — 设置玩家侧边栏开关（落盘 + 主线程立即生效）
    RemoteCall::exportAs(SIDEBAR_NS, "setPlayerSidebarEnabled", [](std::string const& xuid, bool enabled) -> bool {
        if (xuid.empty()) return false;
        SidebarManager::getInstance().setPlayerEnabled(xuid, enabled);
        std::string copy = xuid;
        runOnServerThread([copy, enabled] {
            auto level = ll::service::getLevel();
            if (!level) return;
            auto* player = level->getPlayerByXuid(copy);
            if (!player) return;
            if (enabled) {
                SidebarManager::getInstance().refreshPlayer(player);
            } else {
                SidebarManager::getInstance().removeSidebar(player);
            }
        });
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

    // MeowPAPI 链入：内嵌的 MeowPAPI.dll 在 load 阶段释放并加载；
    // 若服上已经把 MeowPAPI 当独立插件加载，这里直接连上它（多消费者防冲突在静态库里）。
    if (meowpapi::ensureLoaded()) {
        gPapiReady.store(true, std::memory_order_relaxed);
        logger.info(
            "MeowPAPI 已就绪: {} (ABI 0x{:06x})",
            meowpapi::isDeployer() ? "本插件部署并托管" : "连接已由 LeviLamina 加载的 MeowPAPI",
            meowpapi::loadedAbiVersion()
        );
    } else {
        // 侧边栏渲染本身不依赖占位符引擎：先降级为原文显示，enable() 里再补一次
        logger.error("MeowPAPI 未链上，占位符将保持原文；侧边栏其余功能照常");
    }
    return true;
}

bool MeowSidebar::enable() {
    auto& logger = getSelf().getLogger();
    logger.info("MeowSidebar (C++) enabling...");

    // 0. HologramLib 前置检查必须最先做：
    //    本插件所有侧边栏显示都走它的 ISidebar，库比消费方旧时虚表槽位会对不上，
    //    所以要在注册命令/事件/RemoteCall 之前就拒绝启用（避免半初始化状态）。
    std::uint32_t hologramAbi = 0;
    try {
        hologramAbi = static_cast<std::uint32_t>(hologramlib::IHologramLib::getInstance().version());
    } catch (...) {
        logger.error("HologramLib 不可用：无法取得 IHologramLib 实例，拒绝启用");
        return false;
    }
    if (!hologramlib::abiCompatible(hologramAbi)) {
        logger.error(
            "MeowSidebar 按 0x{:06X} 编译，当前 HologramLib 只有 0x{:06X} → 拒绝启用（重装配套版本）",
            static_cast<std::uint32_t>(HOLOGLIB_API_VERSION),
            hologramAbi
        );
        return false;
    }
    logger.info(
        "HologramLib 已连接 (ABI 0x{:06X} / 编译期 0x{:06X})",
        hologramAbi,
        static_cast<std::uint32_t>(HOLOGLIB_API_VERSION)
    );

    try {
        // 1. 数据迁移（BetterSidebar → MeowSidebar）
        DataMigrator::migrateIfNeeded();

        // 2. 加载配置
        ConfigManager::getInstance().load();

        // 3. MeowPAPI：load() 阶段若因部署/加载时序没链上，这里再补一次；
        //    仍失败则降级运行（占位符保持原文），不阻断侧边栏。
        if (!papiReady()) {
            if (meowpapi::ensureLoaded()) {
                gPapiReady.store(true, std::memory_order_relaxed);
                logger.info(
                    "MeowPAPI 已就绪（enable 补链）: {}",
                    meowpapi::isDeployer() ? "本插件部署并托管" : "连接独立 MeowPAPI 插件"
                );
            }
        }
        if (papiReady()) {
            // 运行时 ABI 诊断：检测已加载 MeowPAPI.dll 的 ABI 版本/功能位/构建时间戳，
            // 与编译期 ABI 对照，秒判部署版本
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
        } else {
            logger.error("MeowPAPI 未链上：占位符保持原文，侧边栏其余功能照常");
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

        logger.info(
            "MeowSidebar (C++) enabled. 侧边栏=tick 驱动, PAPI={}, HologramLib ABI 0x{:06X}",
            papiReady() ? "已链入" : "未链上(占位符降级)",
            hologramAbi
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
