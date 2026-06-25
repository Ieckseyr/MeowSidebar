// EventRegistration.cpp - 事件注册实现
//
// 监听事件：
// 1. PlayerJoinEvent - 玩家进服时加载配置
// 2. PlayerDisconnectEvent - 玩家离服时清理状态
#include "MeowSidebar.h"
#include "SidebarManager.h"
#include "PlayerConfigManager.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"

#include <mc/world/actor/player/Player.h>

namespace meowsidebar {

void registerEvents(std::vector<ll::event::ListenerPtr>& listeners) {
    auto& bus = ll::event::EventBus::getInstance();

    // 玩家进服 - 加载玩家配置
    auto joinListener = bus.emplaceListener<ll::event::player::PlayerJoinEvent>(
        [](ll::event::player::PlayerJoinEvent& event) {
            auto& player = event.self();
            std::string xuid;
            try { xuid = player.getXuid(); } catch (...) { return; }
            if (xuid.empty()) return;
            // 预加载玩家配置
            PlayerConfigManager::getInstance().getLines(xuid);
        }
    );
    listeners.push_back(joinListener);

    // 玩家离服 - 清理侧边栏状态
    auto disconnectListener = bus.emplaceListener<ll::event::player::PlayerDisconnectEvent>(
        [](ll::event::player::PlayerDisconnectEvent& event) {
            auto& player = event.self();
            std::string xuid;
            try { xuid = player.getXuid(); } catch (...) { return; }
            if (xuid.empty()) return;
            // 移除侧边栏状态
            SidebarManager::getInstance().removeSidebar(&player);
        }
    );
    listeners.push_back(disconnectListener);
}

} // namespace meowsidebar
