// CommandRegistration.cpp - /sidebar 命令注册
//
// 命令格式：
//   /sidebar              - 显示帮助（仅玩家）
//   /sidebar on           - 开启侧边栏（仅玩家）
//   /sidebar off          - 关闭侧边栏（仅玩家）
//   /sidebar add <text>   - 添加自定义行（仅玩家）
//   /sidebar del <index>  - 删除自定义行（仅玩家）
//   /sidebar clear        - 清空自定义行（仅玩家）
//   /sidebar list         - 列出自定义行（仅玩家）
//   /sidebar scoreboard   - 列出游戏内原生计分板 objective（后台可用）
//   /sidebar scoreboard <objective> - 查看某个 objective 的排行（后台可用）
//   /sidebar reload       - 重载配置（独立子命令：后台控制台与游戏内均可执行，游戏内需OP）
//
// reload 为独立 literal 子命令，不做玩家实体检查，后台可执行；
// 其余子命令全部玩家限定。
#include "Config.h"
#include "SidebarManager.h"
#include "PlayerConfigManager.h"
#include "ScoreboardAdapter.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOriginType.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"

#include <string>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace meowsidebar {

struct ActionParam {
    std::string action;
};

struct TextParam {
    std::string text;
};

struct IndexParam {
    int index;
};

struct ObjectiveParam {
    std::string objective;
};

namespace {

// reload 子命令统一处理：后台控制台与游戏内均可执行（游戏内需 OP）
void handleReload(CommandOrigin const& origin, CommandOutput& output) {
    // 游戏内玩家需 OP 权限；后台控制台(DedicatedServer)等非玩家来源直接放行
    if (origin.getOriginType() == CommandOriginType::Player
        && (int)origin.getPermissionsLevel() < (int)CommandPermissionLevel::GameDirectors) {
        output.error("§c[MeowSidebar] 权限不足(需OP)");
        return;
    }
    SidebarManager::getInstance().reloadConfig();
    output.success("§a[MeowSidebar] 配置已重载");
}

// 把 JSON 里的 slots 数组拼成 "sidebar,list" 形式
std::string joinSlots(nlohmann::json const& item) {
    std::string slots;
    if (item.contains("slots") && item["slots"].is_array()) {
        for (auto const& slot : item["slots"]) {
            if (!slot.is_string()) continue;
            if (!slots.empty()) slots += ',';
            slots += slot.get<std::string>();
        }
    }
    return slots.empty() ? std::string{"-"} : slots;
}

// /sidebar scoreboard —— 列出游戏内所有原生计分板 objective + 当前 sidebar 显示槽
void handleScoreboardList(CommandOutput& output) {
    nlohmann::json sidebar = nlohmann::json::object();
    nlohmann::json objectives = nlohmann::json::array();
    try {
        sidebar = nlohmann::json::parse(ScoreboardAdapter::getInstance().jsonSidebar());
    } catch (...) {}
    try {
        objectives = nlohmann::json::parse(ScoreboardAdapter::getInstance().jsonObjectives());
    } catch (...) {}

    output.success("§a===== §b游戏内计分板(Scoreboard) §a=====");

    if (sidebar.is_object() && sidebar.contains("name")) {
        output.success(fmt::format(
            "§7sidebar 槽: §f{} §7(§f{}§7, §f{}§7 条)",
            sidebar.value("name", std::string{}),
            sidebar.value("display", std::string{}),
            sidebar.value("count", 0)
        ));
    } else {
        output.success("§7sidebar 槽: §8(无原生计分板显示)");
    }

    if (!objectives.is_array() || objectives.empty()) {
        output.success("§7服务端没有任何原生计分板 objective");
        return;
    }

    output.success(fmt::format("§7共 §f{} §7个 objective:", objectives.size()));
    size_t shown = 0;
    for (auto const& item : objectives) {
        if (shown >= 20) {
            output.success("§8... (仅显示前 20 个)");
            break;
        }
        ++shown;
        output.success(fmt::format(
            "§e{} §7(§f{}§7) §7条目 §f{} §7槽 §f{}",
            item.value("name", std::string{}),
            item.value("display", std::string{}),
            item.value("count", 0),
            joinSlots(item)
        ));
    }
    output.success("§7查看详情: §e/sidebar scoreboard <objective>");
}

// /sidebar scoreboard <objective> —— 查看某个 objective 的排行（前 10）
void handleScoreboardDetail(CommandOutput& output, std::string const& objective) {
    nlohmann::json obj = nlohmann::json::object();
    try {
        obj = nlohmann::json::parse(ScoreboardAdapter::getInstance().jsonObjective(objective));
    } catch (...) {}

    if (!obj.is_object() || !obj.contains("name")) {
        output.error(fmt::format("§c[MeowSidebar] objective 不存在: §f{}", objective));
        return;
    }

    output.success(fmt::format("§a===== §b{} §a=====", obj.value("name", std::string{})));
    output.success(fmt::format("§7显示名: §f{}", obj.value("display", std::string{})));
    output.success(fmt::format(
        "§7判据: §f{} §7条目: §f{} §7槽: §f{}",
        obj.value("criteria", std::string{}),
        obj.value("count", 0),
        joinSlots(obj)
    ));

    nlohmann::json entries = nlohmann::json::array();
    if (obj.contains("entries") && obj["entries"].is_array()) entries = obj["entries"];
    if (entries.empty()) {
        output.success("§7(该 objective 没有任何条目)");
        return;
    }

    size_t rank = 0;
    for (auto const& entry : entries) {
        if (rank >= 10) {
            output.success("§8... (仅显示前 10 名)");
            break;
        }
        ++rank;
        output.success(fmt::format(
            "§e{}§7. §f{} §7- §a{}",
            rank,
            entry.value("name", std::string{}),
            entry.value("score", 0)
        ));
    }
}

// /sidebar expand <文本> —— 试算 {sb.*} 占位符（玩家执行时带 me/rank 上下文）
void handleExpand(CommandOrigin const& origin, CommandOutput& output, std::string const& text) {
    Player* player = nullptr;
    if (auto* entity = origin.getEntity(); entity && entity->isPlayer()) {
        player = static_cast<Player*>(entity);
    }
    std::string expanded = ScoreboardAdapter::getInstance().expand(text, player);
    output.success(fmt::format("§7原文: §f{}", text));
    output.success(fmt::format("§7展开: §f{}", expanded));
    if (expanded == text) {
        output.success("§8(没命中任何 {sb.*} 占位符，或 objective 不存在)");
    }
}

} // namespace

void registerCommands() {
    auto& cmd = ll::command::CommandRegistrar::getInstance(false)
                    .getOrCreateCommand("sidebar", "§a设置§b侧边栏", CommandPermissionLevel::Any);

    // /sidebar - 无参数，显示帮助
    cmd.overload().execute(
        [](CommandOrigin const& origin, CommandOutput& output) {
            auto* entity = origin.getEntity();
            if (!entity || !entity->isPlayer()) {
                output.error("§c此指令仅允许游戏内玩家执行");
                return;
            }
            auto& player = static_cast<Player&>(*entity);
            player.sendMessage("§a===== §bMeowSidebar §a=====");
            player.sendMessage("§e/sidebar on §7- 开启侧边栏");
            player.sendMessage("§e/sidebar off §7- 关闭侧边栏");
            player.sendMessage("§e/sidebar add <文本> §7- 添加自定义行");
            player.sendMessage("§e/sidebar del <序号> §7- 删除自定义行");
            player.sendMessage("§e/sidebar clear §7- 清空自定义行");
            player.sendMessage("§e/sidebar list §7- 列出自定义行");
            player.sendMessage("§e/sidebar scoreboard §7- 查看游戏内计分板");
            player.sendMessage("§e/sidebar scoreboard <objective> §7- 查看计分板排行");
            player.sendMessage("§e/sidebar expand <文本> §7- 试算 {sb.*} 占位符");
            player.sendMessage("§e/sidebar reload §7- 重载配置(OP,后台可用)");
        }
    );

    // /sidebar reload - 重载配置（独立 literal 子命令：后台控制台与游戏内均可执行）
    cmd.overload().text("reload").execute(
        [](CommandOrigin const& origin, CommandOutput& output) { handleReload(origin, output); }
    );

    // /sidebar <action>
    cmd.overload<ActionParam>().required("action").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ActionParam const& param) {
            auto const& action = param.action;

            // reload 分流：即使该输入命中本字符串重载，也在玩家检查前处理，
            // 保证后台执行不被"仅允许游戏内玩家执行"拦截（与 literal 子命令等效）
            if (action == "reload") {
                handleReload(origin, output);
                return;
            }

            // scoreboard 分流：只读查询，后台/玩家均可执行
            if (action == "scoreboard" || action == "计分板" || action == "sb") {
                handleScoreboardList(output);
                return;
            }

            // expand 缺文本时给出用法（避免落到"未知操作"）
            if (action == "expand" || action == "试算") {
                output.error("§c用法: /sidebar expand <文本> 例: /sidebar expand {sb.money.top.1.name}");
                return;
            }

            auto* entity = origin.getEntity();
            if (!entity || !entity->isPlayer()) {
                output.error("§c此指令仅允许游戏内玩家执行");
                return;
            }
            auto& player = static_cast<Player&>(*entity);
            std::string xuid;
            try { xuid = player.getXuid(); } catch (...) {
                output.error("§c无法获取玩家信息");
                return;
            }
            if (xuid.empty()) {
                output.error("§c无法获取 XUID");
                return;
            }

            if (action == "on" || action == "开" || action == "true") {
                SidebarManager::getInstance().setPlayerEnabled(xuid, true);
                SidebarManager::getInstance().refreshPlayer(&player);
                player.sendMessage("§a[MeowSidebar] 侧边栏已开启");
            } else if (action == "off" || action == "关" || action == "false") {
                SidebarManager::getInstance().setPlayerEnabled(xuid, false);
                SidebarManager::getInstance().removeSidebar(&player);
                player.sendMessage("§c[MeowSidebar] 侧边栏已关闭");
            } else if (action == "clear") {
                PlayerConfigManager::getInstance().clearLines(xuid);
                SidebarManager::getInstance().refreshPlayer(&player);
                player.sendMessage("§a[MeowSidebar] 已清空自定义行");
            } else if (action == "list") {
                auto lines = PlayerConfigManager::getInstance().getLines(xuid);
                if (lines.empty()) {
                    player.sendMessage("§7[MeowSidebar] 暂无自定义行");
                } else {
                    player.sendMessage("§a===== §b自定义行列表 §a=====");
                    for (size_t i = 0; i < lines.size(); i++) {
                        player.sendMessage(fmt::format("§e{}: §f{}", i, lines[i]));
                    }
                }
            } else {
                player.sendMessage("§c[MeowSidebar] 未知操作: " + action);
            }
        }
    );

    // /sidebar add <text>
    cmd.overload<TextParam>().required("text").execute(
        [](CommandOrigin const& origin, CommandOutput& output, TextParam const& param) {
            auto* entity = origin.getEntity();
            if (!entity || !entity->isPlayer()) {
                output.error("§c此指令仅允许游戏内玩家执行");
                return;
            }
            auto& player = static_cast<Player&>(*entity);
            std::string xuid;
            try { xuid = player.getXuid(); } catch (...) {
                output.error("§c无法获取玩家信息");
                return;
            }
            if (xuid.empty()) {
                output.error("§c无法获取 XUID");
                return;
            }
            if (PlayerConfigManager::getInstance().addLine(xuid, param.text)) {
                SidebarManager::getInstance().refreshPlayer(&player);
                player.sendMessage("§a[MeowSidebar] 已添加: §f" + param.text);
            } else {
                player.sendMessage("§c[MeowSidebar] 添加失败");
            }
        }
    );

    // /sidebar del <index>
    cmd.overload<IndexParam>().required("index").execute(
        [](CommandOrigin const& origin, CommandOutput& output, IndexParam const& param) {
            auto* entity = origin.getEntity();
            if (!entity || !entity->isPlayer()) {
                output.error("§c此指令仅允许游戏内玩家执行");
                return;
            }
            auto& player = static_cast<Player&>(*entity);
            std::string xuid;
            try { xuid = player.getXuid(); } catch (...) {
                output.error("§c无法获取玩家信息");
                return;
            }
            if (xuid.empty()) {
                output.error("§c无法获取 XUID");
                return;
            }
            if (PlayerConfigManager::getInstance().delLine(xuid, param.index)) {
                SidebarManager::getInstance().refreshPlayer(&player);
                player.sendMessage(fmt::format("§a[MeowSidebar] 已删除行: {}", param.index));
            } else {
                player.sendMessage("§c[MeowSidebar] 删除失败（索引无效）");
            }
        }
    );

    // /sidebar scoreboard —— 列出游戏内原生计分板（控制台也可执行）
    cmd.overload().text("scoreboard").execute(
        [](CommandOrigin const&, CommandOutput& output) { handleScoreboardList(output); }
    );

    // /sidebar scoreboard <objective> —— 查看某个 objective 的排行
    cmd.overload<ObjectiveParam>().text("scoreboard").required("objective").execute(
        [](CommandOrigin const&, CommandOutput& output, ObjectiveParam const& param) {
            handleScoreboardDetail(output, param.objective);
        }
    );

    // /sidebar expand <text> —— 试算计分板占位符（后台也可用）
    cmd.overload<TextParam>().text("expand").required("text").execute(
        [](CommandOrigin const& origin, CommandOutput& output, TextParam const& param) {
            handleExpand(origin, output, param.text);
        }
    );
}

} // namespace meowsidebar
