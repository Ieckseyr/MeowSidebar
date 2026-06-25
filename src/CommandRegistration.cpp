// CommandRegistration.cpp - /sidebar 命令注册
//
// 命令格式：
//   /sidebar              - 显示帮助
//   /sidebar on           - 开启侧边栏
//   /sidebar off          - 关闭侧边栏
//   /sidebar reload       - 重载配置（仅OP）
//   /sidebar add <text>   - 添加自定义行
//   /sidebar del <index>  - 删除自定义行
//   /sidebar clear        - 清空自定义行
//   /sidebar list         - 列出自定义行
#include "MeowSidebar.h"
#include "Config.h"
#include "SidebarManager.h"
#include "PlayerConfigManager.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"

#include <string>
#include <fmt/format.h>

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
            player.sendMessage("§e/sidebar reload §7- 重载配置(OP)");
        }
    );

    // /sidebar <action>
    cmd.overload<ActionParam>().required("action").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ActionParam const& param) {
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

            auto const& action = param.action;

            if (action == "on" || action == "开" || action == "true") {
                SidebarManager::getInstance().setPlayerEnabled(xuid, true);
                SidebarManager::getInstance().refreshPlayer(&player);
                player.sendMessage("§a[MeowSidebar] 侧边栏已开启");
            } else if (action == "off" || action == "关" || action == "false") {
                SidebarManager::getInstance().setPlayerEnabled(xuid, false);
                SidebarManager::getInstance().removeSidebar(&player);
                player.sendMessage("§c[MeowSidebar] 侧边栏已关闭");
            } else if (action == "reload") {
                // 检查 OP 权限
                if ((int)player.getCommandPermissionLevel() < 1) {
                    player.sendMessage("§c[MeowSidebar] 权限不足");
                    return;
                }
                SidebarManager::getInstance().reloadConfig();
                player.sendMessage("§a[MeowSidebar] 配置已重载");
            } else if (action == "clear") {
                PlayerConfigManager::getInstance().clearLines(xuid);
                SidebarManager::getInstance().refreshPlayer(&player);
                player.sendMessage("§a[MeowSidebar] 已清空自定义行");
            } else if (action == "list") {
                auto lines = PlayerConfigManager::getInstance().listLines(xuid);
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
}

} // namespace meowsidebar
