// EventRegistration.cpp - 事件注册实现
//
// 监听事件：
// 1. PlayerJoinEvent - 玩家进服时加载配置
// 2. PlayerDisconnectEvent - 玩家离服时清理状态
#include "SidebarManager.h"
#include "PlayerConfigManager.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"

#include <mc/world/actor/player/Player.h>

// ── LeviLamina 26.40 事件 ID 对齐补丁（玩家事件收不到根因）──
// 根因: 26.40 把事件类挪进 inline namespace (ll::event::inline player / ...)，
// EventId 基于 __FUNCSIG__/__PRETTY_FUNCTION__ 的类型名反射:
//   Clang (官方 LeviLamina.dll) → 省略 inline ns → "ll::event::PlayerJoinEvent"
//   MSVC (本插件)                → 保留 inline ns → "ll::event::player::PlayerJoinEvent"
// 两侧 hash 不一致 → MSVC 插件的监听器收不到事件 →
// 进服预加载配置失效 + 离服状态不清理（内存泄漏）。
// 已用二进制验证: LeviLamina.dll 中字符串为 "ll::event::PlayerJoinEvent" /
// "ll::event::PlayerDisconnectEvent"（无 player:: 前缀）。
// 修复: 显式特化 getEventId，强制对齐 Clang 侧的名字（与 Mtps/Cleaner 同款补丁）。
namespace ll::event {
template <>
constexpr EventIdView getEventId<::ll::event::player::PlayerJoinEvent> =
    EventIdView{"ll::event::PlayerJoinEvent"};

template <>
constexpr EventIdView getEventId<::ll::event::player::PlayerDisconnectEvent> =
    EventIdView{"ll::event::PlayerDisconnectEvent"};
} // namespace ll::event

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

    // 玩家离服 - 清理侧边栏状态（不发包，直接 erase 防止内存泄漏）
    auto disconnectListener = bus.emplaceListener<ll::event::player::PlayerDisconnectEvent>(
        [](ll::event::player::PlayerDisconnectEvent& event) {
            auto& player = event.self();
            SidebarManager::getInstance().onPlayerDisconnect(&player);
        }
    );
    listeners.push_back(disconnectListener);
}

} // namespace meowsidebar
