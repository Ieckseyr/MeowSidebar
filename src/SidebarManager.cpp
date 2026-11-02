// SidebarManager.cpp - 侧边栏核心管理器实现
#include "SidebarManager.h"
#include "Config.h"
#include "PlayerConfigManager.h"
#include "MeowSidebar.h"
#include "ScoreboardAdapter.h"

#include "meowpapi/PlaceholderApi.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

// 26.60: 侧边栏协议层（objective 派生 / 发包 / 回读校验）由 HologramLib 提供
#include <hologramlib/HologramLib.h>

#include <algorithm>
#include <chrono>

// ── LeviLamina 26.40 事件 ID 对齐补丁（侧边栏不刷新根因）──
// 根因: 26.40 把事件类挪进 inline namespace (ll::event::inline world / ...)，
// EventId 基于 __FUNCSIG__/__PRETTY_FUNCTION__ 的类型名反射:
//   Clang (官方 LeviLamina.dll) → 省略 inline ns → "ll::event::ServerLevelTickEvent"
//   MSVC (本插件)                → 保留 inline ns → "ll::event::world::ServerLevelTickEvent"
// 两侧 hash 不一致 → MSVC 插件的监听器收不到事件 → onTick 从未执行 →
// 侧边栏刷新循环整体死亡（玩家进服后无任何刷新包，仅 RemoteCall 外部触发偶发一次）。
// 已用二进制验证: LeviLamina.dll 中字符串为 "ll::event::ServerLevelTickEvent"（无 world::）。
// 修复: 显式特化 getEventId，强制对齐 Clang 侧的名字（与 Mtps/Cleaner 同款补丁）。
namespace ll::event {
template <>
constexpr EventIdView getEventId<::ll::event::world::ServerLevelTickEvent> =
    EventIdView{"ll::event::ServerLevelTickEvent"};
} // namespace ll::event

// ===== 协议层：交给 HologramLib =====
// 26.60 起侧边栏不再自己构造/发送协议包：
//   旧实现手写 SetDisplayObjective(107) / SetScore(108) / RemoveObjective(106) 的 wire 字节流，
//   再用 NetworkPeer::sendPacket 直发；现在全部走 HologramLib 的 ISidebar
//   （objective 名按玩家派生、旧行清理、发包与 BDS 回读校验都在库里）。
// 本文件只负责"这一拍显示什么"：配置 + 占位符 + 动画索引 → title/lines。
namespace {

hologramlib::ISidebar& sidebarApi() {
    return hologramlib::IHologramLib::getInstance().sidebar();
}

} // anonymous namespace
namespace meowsidebar {

SidebarManager& SidebarManager::getInstance() {
    static SidebarManager instance;
    return instance;
}

void SidebarManager::ArrayIndexManager::rebuild(json const& arr) {
    sourceArray = arr;
    indexes.clear();
    if (!arr.is_array()) return;
    indexes.resize(arr.size());
    for (size_t i = 0; i < arr.size(); i++) {
        if (arr[i].is_array()) {
            if (!arr[i].empty() && arr[i][0].is_array()) {
                indexes[i].max = static_cast<int>(arr[i][0].size()) - 1;
                if (arr[i].size() >= 2 && arr[i][1].is_number()) {
                    indexes[i].interval = arr[i][1].get<int>();
                    if (indexes[i].interval < 0) indexes[i].interval = 0;
                }
            } else {
                indexes[i].max = static_cast<int>(arr[i].size()) - 1;
            }
            indexes[i].current = 0;
            indexes[i].elapsed = 0;
        } else {
            indexes[i].max = 0;
            indexes[i].current = 0;
            indexes[i].interval = 0;
            indexes[i].elapsed = 0;
        }
    }
}

std::vector<std::string> SidebarManager::ArrayIndexManager::getCurrentValues() const {
    std::vector<std::string> result;
    if (!sourceArray.is_array()) return result;
    result.reserve(sourceArray.size());
    for (size_t i = 0; i < sourceArray.size(); i++) {
        if (sourceArray[i].is_array()) {
            int idx = (i < indexes.size()) ? indexes[i].current : 0;
            json const* values = &sourceArray[i];
            if (!sourceArray[i].empty() && sourceArray[i][0].is_array()) {
                values = &sourceArray[i][0];
            }
            if (idx >= 0 && idx < static_cast<int>(values->size())) {
                auto const& elem = (*values)[idx];
                if (elem.is_string()) {
                    result.push_back(elem.get<std::string>());
                } else {
                    result.push_back(elem.dump());
                }
            } else {
                result.push_back("");
            }
        } else if (sourceArray[i].is_string()) {
            result.push_back(sourceArray[i].get<std::string>());
        } else {
            result.push_back(sourceArray[i].dump());
        }
    }
    return result;
}

void SidebarManager::ArrayIndexManager::incrementAll(int deltaMs) {
    for (auto& idx : indexes) {
        if (idx.max > 0) {
            if (idx.interval > 0) {
                idx.elapsed += deltaMs;
                if (idx.elapsed >= idx.interval) {
                    idx.elapsed = 0;
                    idx.current++;
                    if (idx.current > idx.max) idx.current = 0;
                }
            } else {
                idx.current++;
                if (idx.current > idx.max) idx.current = 0;
            }
        }
    }
}

void SidebarManager::init() {
    // 原生计分板适配层：记录主线程 ID 并加载缓存 TTL
    ScoreboardAdapter::getInstance().init();

    auto const& titleCfg = ConfigManager::getInstance().getTitleConfig();
    auto const& dataCfg = ConfigManager::getInstance().getDataConfig();
    {
        std::lock_guard<std::mutex> lock(mAIMMutex);
        mTitleAIM.rebuild(titleCfg.is_array() ? titleCfg : json::array({titleCfg}));
        mDataAIM.rebuild(dataCfg.is_array() ? dataCfg : json::array({dataCfg}));
        mTitleElapsedMs = 0;
    }

    auto& bus = ll::event::EventBus::getInstance();
    mTickListener = bus.emplaceListener<ll::event::world::ServerLevelTickEvent>(
        [this](ll::event::world::ServerLevelTickEvent&) {
            this->onTick();
        }
    );
    mLastRefreshTimeValid = false;
}

void SidebarManager::shutdown() {
    if (mTickListener) {
        auto& bus = ll::event::EventBus::getInstance();
        bus.removeListener(mTickListener);
        mTickListener = nullptr;
    }

    auto level = ll::service::getLevel();
    if (level) {
        level->forEachPlayer([this](Player& player) -> bool {
            sendRemoveSidebarPackets(&player);
            return true;
        });
    }

    std::lock_guard<std::mutex> lock(mStateMutex);
    mActivePlayers.clear();

    // 清空原生计分板快照缓存
    ScoreboardAdapter::getInstance().shutdown();
}

void SidebarManager::reloadConfig() {
    ConfigManager::getInstance().reload();
    ScoreboardAdapter::getInstance().reloadConfig();
    auto const& titleCfg = ConfigManager::getInstance().getTitleConfig();
    auto const& dataCfg = ConfigManager::getInstance().getDataConfig();
    {
        std::lock_guard<std::mutex> lock(mAIMMutex);
        mTitleAIM.rebuild(titleCfg.is_array() ? titleCfg : json::array({titleCfg}));
        mDataAIM.rebuild(dataCfg.is_array() ? dataCfg : json::array({dataCfg}));
        mTitleElapsedMs = 0;
    }
}

void SidebarManager::onTick() {
    static bool sFirstTick = true; // 首次触发日志: 验证事件 ID 对齐补丁生效
    if (sFirstTick) {
        sFirstTick = false;
        try {
            meowsidebar::lib().getSelf().getLogger().info("tick 事件已触发（事件 ID 对齐补丁生效），侧边栏刷新循环启动");
        } catch (...) {}
    }

    int hz = ConfigManager::getInstance().getHz();
    if (hz < 50) hz = 50;

    auto now = std::chrono::steady_clock::now();
    if (!mLastRefreshTimeValid) {
        mLastRefreshTime = now;
        mLastRefreshTimeValid = true;
        return;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastRefreshTime).count();
    if (elapsed < hz) {
        return;
    }

    int deltaMs = static_cast<int>(elapsed);
    mLastRefreshTime = now;

    try {
        refreshAll(deltaMs);
    } catch (...) {
    }
}

void SidebarManager::refreshAll(int deltaMs) {
    auto level = ll::service::getLevel();
    if (!level) return;

    level->forEachPlayer([this](Player& player) -> bool {
        try {
            processPlayer(&player);
        } catch (...) {
        }
        return true;
    });

    std::lock_guard<std::mutex> lock(mAIMMutex);
    mDataAIM.incrementAll(deltaMs);

    int titleInterval = ConfigManager::getInstance().getTitleRefreshInterval();
    if (titleInterval > 0) {
        mTitleElapsedMs += deltaMs;
        if (mTitleElapsedMs >= titleInterval) {
            mTitleAIM.incrementAll(mTitleElapsedMs);
            mTitleElapsedMs = 0;
        }
    } else {
        mTitleAIM.incrementAll(deltaMs);
    }
}

void SidebarManager::refreshPlayer(Player* player) {
    if (!player) return;
    processPlayer(player);
}

bool SidebarManager::shouldShowSidebar(Player* player) const {
    if (!player) return false;
    if (!player->isPlayer()) return false;

    std::string xuid;
    try { xuid = player->getXuid(); } catch (...) { return false; }
    if (xuid.empty()) return false;

    {
        std::lock_guard<std::mutex> lock(mEnabledMutex);
        auto it = mPlayerEnabled.find(xuid);
        if (it != mPlayerEnabled.end() && !it->second) return false;
    }

    return true;
}

// 展开一行文本：先解析原生计分板占位符（{sb.*}/{scoreboard.*}），
// 再交给 MeowPAPI 翻译。
// 展开结果里的 '\n' 会拆成多行侧边栏行——{sb.sidebar.lines} 这类聚合占位符
// 依靠它把游戏内计分板整块搬进侧边栏。
static void appendExpandedLines(
    std::vector<std::string>& out,
    std::string const&        raw,
    Player*                   player
) {
    if (raw.empty()) return;

    std::string expanded = ScoreboardAdapter::getInstance().expand(raw, player);

    size_t start = 0;
    while (true) {
        size_t const nl   = expanded.find('\n', start);
        std::string  part = (nl == std::string::npos) ? expanded.substr(start) : expanded.substr(start, nl - start);
        if (!part.empty()) {
            part = meowpapi::PlaceholderApi::getInstance().translateStringWithPlayer(part, player);
            out.push_back(std::move(part));
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

void SidebarManager::processPlayer(Player* player) {
    if (!shouldShowSidebar(player)) {
        std::string xuid;
        try { xuid = player->getXuid(); } catch (...) { return; }
        std::lock_guard<std::mutex> lock(mStateMutex);
        if (mActivePlayers.erase(xuid) > 0) {
            sendRemoveSidebarPackets(player);
        }
        return;
    }

    std::vector<std::string> titleParts, dataParts;
    {
        std::lock_guard<std::mutex> lock(mAIMMutex);
        titleParts = mTitleAIM.getCurrentValues();
        dataParts = mDataAIM.getCurrentValues();
    }

    std::string title;
    for (auto const& p : titleParts) title += p;

    // 原生计分板占位符展开（标题不能含换行，聚合占位符的换行折叠为空格）
    title = ScoreboardAdapter::getInstance().expand(title, player);
    std::replace(title.begin(), title.end(), '\n', ' ');

    title = meowpapi::PlaceholderApi::getInstance().translateString(title);
    title = meowpapi::PlaceholderApi::getInstance().translateStringWithPlayer(title, player);

    std::string xuid;
    try { xuid = player->getXuid(); } catch (...) { return; }
    auto customLines = PlayerConfigManager::getInstance().getLines(xuid);

    std::vector<std::string> lines;
    lines.reserve(dataParts.size() + customLines.size());
    for (auto const& line : dataParts) appendExpandedLines(lines, line, player);
    for (auto const& line : customLines) appendExpandedLines(lines, line, player);

    std::unordered_set<std::string> seen;
    std::vector<std::string> uniqueLines;
    uniqueLines.reserve(lines.size());
    for (auto& line : lines) {
        if (line.empty()) continue;
        std::string uniqueLine = line;
        while (seen.count(uniqueLine)) {
            uniqueLine += " ";
        }
        seen.insert(uniqueLine);
        uniqueLines.push_back(std::move(uniqueLine));
    }

    if (uniqueLines.empty()) {
        std::lock_guard<std::mutex> lock(mStateMutex);
        if (mActivePlayers.erase(xuid) > 0) {
            sendRemoveSidebarPackets(player);
        }
        return;
    }

    sendSidebarPackets(player, title, uniqueLines);

    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        mActivePlayers.insert(xuid);
    }
}
void SidebarManager::sendSidebarPackets(
    Player* player,
    std::string const& title,
    std::vector<std::string> const& lines
) {
    if (!player || lines.empty()) return;
    try {
        // HologramLib 按玩家名索引（objective 名也由名字派生）
        sidebarApi().set(player->getRealName(), title, lines);
    } catch (...) {
        // 发包失败不影响服务端 tick
    }
}

void SidebarManager::sendRemoveSidebarPackets(Player* player) {
    if (!player) return;
    try {
        sidebarApi().remove(player->getRealName());
    } catch (...) {
    }
}

void SidebarManager::removeSidebar(Player* player) {
    if (!player) return;
    sendRemoveSidebarPackets(player);
    std::string xuid;
    try { xuid = player->getXuid(); } catch (...) { return; }
    std::lock_guard<std::mutex> lock(mStateMutex);
    mActivePlayers.erase(xuid);
}

void SidebarManager::onPlayerDisconnect(Player* player) {
    if (!player) return;
    std::string xuid;
    try { xuid = player->getXuid(); } catch (...) { return; }
    if (xuid.empty()) return;
    std::lock_guard<std::mutex> lock(mStateMutex);
    mActivePlayers.erase(xuid);
}

void SidebarManager::setPlayerEnabled(std::string const& xuid, bool enabled) {
    std::lock_guard<std::mutex> lock(mEnabledMutex);
    mPlayerEnabled[xuid] = enabled;
}

bool SidebarManager::isPlayerEnabled(std::string const& xuid) const {
    std::lock_guard<std::mutex> lock(mEnabledMutex);
    auto it = mPlayerEnabled.find(xuid);
    if (it != mPlayerEnabled.end()) return it->second;
    return true;
}

} // namespace meowsidebar
