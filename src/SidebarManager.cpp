// SidebarManager.cpp - 侧边栏核心管理器实现
#include "SidebarManager.h"
#include "Config.h"
#include "PlayerConfigManager.h"
#include "MeowSidebar.h"

#include "meowpapi/PlaceholderApi.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/server/ServerStartedEvent.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/network/Packet.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/platform/Result.h"
#include "mc/network/packet/SetScorePacket.h"
#include "mc/network/packet/ScorePacketInfo.h"
#include "mc/network/packet/ScorePacketType.h"
#include "mc/world/scores/IdentityDefinition.h"
#include "mc/world/scores/ObjectiveSortOrder.h"
#include "mc/world/scores/ScoreboardId.h"
#include "mc/world/scores/PlayerScoreboardId.h"
#include "mc/legacy/ActorUniqueID.h"

#include <algorithm>
#include <cstring>

// ===== 自定义数据包 =====
// SetDisplayObjectivePacketPayload 的构造函数未从 bedrock_server.dll 导出，
// 无法直接构造 SetDisplayObjectivePacket。
// 解决方案：继承 ::Packet，手动通过 BinaryStream 写入协议字段。
// MCBE 协议 SetDisplayObjective (ID 107) 格式：
//   string: displaySlotName, string: objectiveName, string: displayName,
//   string: criteriaName, varint: sortOrder
class MeowSetDisplayObjectivePacket : public ::Packet {
public:
    std::string mSlot;
    std::string mObjName;
    std::string mDisplayName;
    std::string mCriteria;
    int         mOrder;

    MeowSetDisplayObjectivePacket(
        std::string slot, std::string obj, std::string display, std::string crit, int order
    ) : mSlot(std::move(slot)),
        mObjName(std::move(obj)),
        mDisplayName(std::move(display)),
        mCriteria(std::move(crit)),
        mOrder(order) {}

    ::MinecraftPacketIds getId() const override { return ::MinecraftPacketIds::SetDisplayObjective; }

    ::std::string_view getName() const override { return "MeowSetDisplayObjective"; }

    void write(::BinaryStream& stream) const override {
        stream.writeString(mSlot, nullptr, nullptr);
        stream.writeString(mObjName, nullptr, nullptr);
        stream.writeString(mDisplayName, nullptr, nullptr);
        stream.writeString(mCriteria, nullptr, nullptr);
        stream.writeVarInt(mOrder, nullptr, nullptr);
    }

    ::Bedrock::Result<void> _read(::ReadOnlyBinaryStream&) override { return {}; }
};

// MCBE 协议 RemoveObjective (ID 106) 格式：
//   string: objectiveName
class MeowRemoveObjectivePacket : public ::Packet {
public:
    std::string mObjName;

    explicit MeowRemoveObjectivePacket(std::string obj) : mObjName(std::move(obj)) {}

    ::MinecraftPacketIds getId() const override { return ::MinecraftPacketIds::RemoveObjective; }

    ::std::string_view getName() const override { return "MeowRemoveObjective"; }

    void write(::BinaryStream& stream) const override {
        stream.writeString(mObjName, nullptr, nullptr);
    }

    ::Bedrock::Result<void> _read(::ReadOnlyBinaryStream&) override { return {}; }
};

namespace meowsidebar {

// 侧边栏使用的 objective 名称（固定，所有玩家共用，因为发包是 per-player 的）
static constexpr char const* SIDEBAR_OBJECTIVE = "_msb";
// 侧边栏显示槽位
static constexpr char const* SIDEBAR_SLOT = "sidebar";
// "dummy" criteria（不显示分数值，只用于排序）
static constexpr char const* DUMMY_CRITERIA = "dummy";

// 生成 fake player 的 ScoreboardId（基于字符串哈希）
static int64_t makeScoreboardId(std::string const& name) {
    std::hash<std::string> hasher;
    return static_cast<int64_t>(hasher(name));
}

SidebarManager& SidebarManager::getInstance() {
    static SidebarManager instance;
    return instance;
}

// ===== ArrayIndexManager 实现 =====

void SidebarManager::ArrayIndexManager::rebuild(json const& arr) {
    sourceArray = arr;
    indexes.clear();
    if (!arr.is_array()) return;
    indexes.resize(arr.size());
    for (size_t i = 0; i < arr.size(); i++) {
        if (arr[i].is_array()) {
            indexes[i].max = static_cast<int>(arr[i].size()) - 1;
            indexes[i].current = 0;
        } else {
            indexes[i].max = 0;
            indexes[i].current = 0;
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
            if (idx >= 0 && idx < static_cast<int>(sourceArray[i].size())) {
                result.push_back(sourceArray[i][idx].get<std::string>());
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

void SidebarManager::ArrayIndexManager::incrementAll() {
    for (auto& idx : indexes) {
        if (idx.max > 0) {
            idx.current++;
            if (idx.current > idx.max) idx.current = 0;
        }
    }
}

// ===== SidebarManager 主逻辑 =====

void SidebarManager::init() {
    // 重建 AIM
    auto const& titleCfg = ConfigManager::getInstance().getTitleConfig();
    auto const& dataCfg = ConfigManager::getInstance().getDataConfig();
    mTitleAIM.rebuild(titleCfg.is_array() ? titleCfg : json::array({titleCfg}));
    mDataAIM.rebuild(dataCfg.is_array() ? dataCfg : json::array({dataCfg}));

    // 启动定时线程
    mRunning = true;
    mShuttingDown = false;
    mTickThread = std::thread([this]() { this->tickLoop(); });
}

void SidebarManager::shutdown() {
    mShuttingDown = true;
    mRunning = false;
    if (mTickThread.joinable()) {
        mTickThread.join();
    }

    // 清除所有在线玩家的侧边栏
    auto level = ll::service::getLevel();
    if (level) {
        level->forEachPlayer([this](Player& player) -> bool {
            sendRemoveSidebarPackets(&player);
            return true;
        });
    }

    std::lock_guard<std::mutex> lock(mStateMutex);
    mPlayerStates.clear();
}

void SidebarManager::reloadConfig() {
    ConfigManager::getInstance().reload();
    auto const& titleCfg = ConfigManager::getInstance().getTitleConfig();
    auto const& dataCfg = ConfigManager::getInstance().getDataConfig();
    mTitleAIM.rebuild(titleCfg.is_array() ? titleCfg : json::array({titleCfg}));
    mDataAIM.rebuild(dataCfg.is_array() ? dataCfg : json::array({dataCfg}));
}

void SidebarManager::tickLoop() {
    while (mRunning) {
        // 等待刷新间隔
        int hz = ConfigManager::getInstance().getHz();
        if (hz < 100) hz = 100;
        std::this_thread::sleep_for(std::chrono::milliseconds(hz));

        if (mShuttingDown) break;

        try {
            refreshAll();
        } catch (...) {
            // 防止异常导致线程退出
        }
    }
}

void SidebarManager::refreshAll() {
    auto level = ll::service::getLevel();
    if (!level) return;

    // 获取所有在线玩家
    level->forEachPlayer([this](Player& player) -> bool {
        try {
            processPlayer(&player);
        } catch (...) {}
        return true;
    });

    // AIM 前进一步（动画效果）
    mTitleAIM.incrementAll();
    mDataAIM.incrementAll();
}

void SidebarManager::refreshPlayer(Player* player) {
    if (!player) return;
    processPlayer(player);
}

bool SidebarManager::shouldShowSidebar(Player* player) const {
    if (!player) return false;

    // 检查是否是模拟玩家（NPC）
    // 在 C++ 中通过 isPlayer 检查
    if (!player->isPlayer()) return false;

    // 检查玩家开关状态
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

void SidebarManager::processPlayer(Player* player) {
    if (!shouldShowSidebar(player)) {
        // 如果玩家不应显示侧边栏但之前显示了，则移除
        std::string xuid;
        try { xuid = player->getXuid(); } catch (...) { return; }
        std::lock_guard<std::mutex> lock(mStateMutex);
        auto it = mPlayerStates.find(xuid);
        if (it != mPlayerStates.end() && it->second.active) {
            sendRemoveSidebarPackets(player);
            it->second.active = false;
        }
        return;
    }

    // 获取 AIM 当前值
    auto titleParts = mTitleAIM.getCurrentValues();
    auto dataParts = mDataAIM.getCurrentValues();

    // 合并标题
    std::string title;
    for (auto const& p : titleParts) title += p;

    // 翻译标题变量（服务器级，无需玩家）
    title = meowpapi::PlaceholderApi::getInstance().translateString(title);
    // 也翻译玩家级变量
    title = meowpapi::PlaceholderApi::getInstance().translateStringWithPlayer(title, player);

    // 合并玩家自定义行
    std::string xuid;
    try { xuid = player->getXuid(); } catch (...) { return; }
    auto customLines = PlayerConfigManager::getInstance().getLines(xuid);

    // 翻译数据行
    std::vector<std::string> lines;
    lines.reserve(dataParts.size() + customLines.size());
    for (auto const& line : dataParts) {
        if (line.empty()) continue;
        std::string translated = meowpapi::PlaceholderApi::getInstance().translateStringWithPlayer(line, player);
        lines.push_back(std::move(translated));
    }
    for (auto const& line : customLines) {
        if (line.empty()) continue;
        std::string translated = meowpapi::PlaceholderApi::getInstance().translateStringWithPlayer(line, player);
        lines.push_back(std::move(translated));
    }

    // 去重（侧边栏不能有重复行名）
    std::unordered_set<std::string> seen;
    std::vector<std::string> uniqueLines;
    uniqueLines.reserve(lines.size());
    for (auto& line : lines) {
        if (line.empty()) continue;
        // 如果重复，添加空格使唯一
        std::string uniqueLine = line;
        while (seen.count(uniqueLine)) {
            uniqueLine += " ";
        }
        seen.insert(uniqueLine);
        uniqueLines.push_back(std::move(uniqueLine));
    }

    if (uniqueLines.empty()) {
        // 无数据，移除侧边栏
        std::lock_guard<std::mutex> lock(mStateMutex);
        auto it = mPlayerStates.find(xuid);
        if (it == mPlayerStates.end() || it->second.active) {
            sendRemoveSidebarPackets(player);
            if (it != mPlayerStates.end()) it->second.active = false;
        }
        return;
    }

    // 发送侧边栏数据包
    sendSidebarPackets(player, title, uniqueLines);

    // 更新状态
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        auto& state = mPlayerStates[xuid];
        state.title = title;
        state.lines = uniqueLines;
        state.active = true;
    }
}

void SidebarManager::sendSidebarPackets(
    Player* player,
    std::string const& title,
    std::vector<std::string> const& lines
) {
    if (!player || lines.empty()) return;

    try {
        // 1. 发送自定义 SetDisplayObjective 数据包 - 设置标题和显示槽位
        MeowSetDisplayObjectivePacket displayPkt(
            SIDEBAR_SLOT,
            SIDEBAR_OBJECTIVE,
            title,
            DUMMY_CRITERIA,
            static_cast<int>(ObjectiveSortOrder::Ascending)
        );
        player->sendNetworkPacket(displayPkt);

        // 2. 构造 SetScorePacket (Change) - 设置每行的分数
        std::vector<ScorePacketInfo> scoreInfos;
        scoreInfos.reserve(lines.size());

        for (int i = 0; i < static_cast<int>(lines.size()); i++) {
            ScorePacketInfo info;
            // ScoreboardId 基于 fake player 名的哈希
            // mScoreboardId 是 TypedStorageImpl<8,16,ScoreboardId>，需用 -> 访问内部成员
            info.mScoreboardId->mRawID = makeScoreboardId(lines[i]);
            info.mScoreboardId->mIdentityDef = nullptr;
            info.mObjectiveName = std::string(SIDEBAR_OBJECTIVE);
            info.mScoreValue = i + 1;  // 1, 2, 3, ... (升序，行1在顶部)
            info.mIdentityType = IdentityDefinition::Type::FakePlayer;
            info.mFakePlayerName = lines[i];
            scoreInfos.push_back(std::move(info));
        }

        SetScorePacket scorePkt = SetScorePacket::change(std::move(scoreInfos));
        player->sendNetworkPacket(scorePkt);

    } catch (...) {}
}

void SidebarManager::sendRemoveSidebarPackets(Player* player) {
    if (!player) return;
    try {
        // 发送 RemoveObjective 数据包清除侧边栏
        MeowRemoveObjectivePacket removePkt(SIDEBAR_OBJECTIVE);
        player->sendNetworkPacket(removePkt);
    } catch (...) {}
}

void SidebarManager::removeSidebar(Player* player) {
    if (!player) return;
    sendRemoveSidebarPackets(player);
    std::string xuid;
    try { xuid = player->getXuid(); } catch (...) { return; }
    std::lock_guard<std::mutex> lock(mStateMutex);
    auto it = mPlayerStates.find(xuid);
    if (it != mPlayerStates.end()) {
        it->second.active = false;
    }
}

void SidebarManager::setPlayerEnabled(std::string const& xuid, bool enabled) {
    std::lock_guard<std::mutex> lock(mEnabledMutex);
    mPlayerEnabled[xuid] = enabled;
}

bool SidebarManager::isPlayerEnabled(std::string const& xuid) const {
    std::lock_guard<std::mutex> lock(mEnabledMutex);
    auto it = mPlayerEnabled.find(xuid);
    if (it != mPlayerEnabled.end()) return it->second;
    return true;  // 默认启用
}

} // namespace meowsidebar
