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
#include "mc/network/NetworkSystem.h"
#include "mc/network/NetworkPeer.h"
#include "mc/network/Compressibility.h"
#include "mc/world/scores/ObjectiveSortOrder.h"

#include <algorithm>
#include <cstring>

// ===== 原始字节流数据包构造 =====
// 自定义 ::Packet 子类并 override write() 在 BDS 中不可靠：write() 被调用时
// BinaryStream 可能已包含同一批次中前一个数据包（如 UpdateBlock）的字节，
// 导致我们的包体被追加到旧数据之后，且包头 ID 取自前一个包（21=UpdateBlock
// 而非 107=SetDisplayObjective）。
// 解决方案：手动构造完整数据包字节流（包头 VarUInt + 包体），通过
// NetworkPeer::sendPacket 直接发送原始字节，绕过 ::Packet 基类。
// 参考实现：MeowLib 的 ProtocolPacketWriter。

namespace {

// 写入无符号 VarUInt（小端基128变长编码）
void writeUnsignedVarInt(std::string& buf, uint32_t value) {
    while (value >= 0x80) {
        buf.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    buf.push_back(static_cast<char>(value));
}

// 写入有符号 VarInt（ZigZag 编码后再用 VarUInt 编码）
void writeVarInt(std::string& buf, int32_t value) {
    uint32_t zz = (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31);
    writeUnsignedVarInt(buf, zz);
}

// 写入字符串（VarUInt 长度 + 原始字节）
void writeString(std::string& buf, std::string const& str) {
    writeUnsignedVarInt(buf, static_cast<uint32_t>(str.size()));
    buf.append(str);
}

// 写入单个字节
void writeByte(std::string& buf, uint8_t value) {
    buf.push_back(static_cast<char>(value));
}

// 写入有符号 VarInt64（ZigZag 编码后再用 VarUInt 编码）
void writeVarInt64(std::string& buf, int64_t value) {
    uint64_t zz = (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
    while (zz >= 0x80) {
        buf.push_back(static_cast<char>((zz & 0x7F) | 0x80));
        zz >>= 7;
    }
    buf.push_back(static_cast<char>(zz));
}

// 写入有符号 32 位整数（4 字节小端，Protocol-v944 writeSignedInt）
void writeSignedInt32(std::string& buf, int32_t value) {
    buf.push_back(static_cast<char>(value & 0xFF));
    buf.push_back(static_cast<char>((value >> 8) & 0xFF));
    buf.push_back(static_cast<char>((value >> 16) & 0xFF));
    buf.push_back(static_cast<char>((value >> 24) & 0xFF));
}

// MCBE 数据包头 ID（与协议 wire ID 一致）
constexpr uint32_t PKT_ID_SET_DISPLAY_OBJECTIVE = 107; // SetDisplayObjective
constexpr uint32_t PKT_ID_REMOVE_OBJECTIVE      = 106; // RemoveObjective
constexpr uint32_t PKT_ID_SET_SCORE             = 108; // SetScore

// 通过 NetworkPeer 直接发送原始字节流
bool sendRawPacket(Player* player, std::string const& data) {
    if (!player || data.empty()) return false;
    auto networkSystem = ll::service::getNetworkSystem();
    if (!networkSystem.has_value()) return false;
    auto* peer = networkSystem->getPeerForUser(player->getNetworkIdentifier());
    if (!peer) return false;
    peer->sendPacket(
        data,
        NetworkPeer::Reliability::ReliableOrdered,
        Compressibility::Compressible
    );
    return true;
}

} // anonymous namespace

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

    // 获取旧状态（用于增量更新，移除已不存在的旧条目）
    std::string oldTitle;
    std::vector<std::string> oldLines;
    bool wasActive = false;
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        auto it = mPlayerStates.find(xuid);
        if (it != mPlayerStates.end()) {
            oldTitle  = it->second.title;
            oldLines  = it->second.lines;
            wasActive = it->second.active;
        }
    }

    // 发送侧边栏数据包（带旧状态进行增量更新）
    sendSidebarPackets(player, title, uniqueLines, oldTitle, oldLines, wasActive);

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
    std::vector<std::string> const& lines,
    std::string const& oldTitle,
    std::vector<std::string> const& oldLines,
    bool wasActive
) {
    if (!player || lines.empty()) return;

    try {
        bool titleChanged = (title != oldTitle) || !wasActive;

        if (titleChanged) {
            // ===== 标题变更或首次显示：完全重建 =====
            // 1. 移除旧 objective（清除所有旧分数条目，防止残留）
            if (wasActive) {
                std::string removePkt;
                writeUnsignedVarInt(removePkt, PKT_ID_REMOVE_OBJECTIVE);
                writeString(removePkt, SIDEBAR_OBJECTIVE);
                sendRawPacket(player, removePkt);
            }

            // 2. 创建新 objective
            std::string displayPkt;
            writeUnsignedVarInt(displayPkt, PKT_ID_SET_DISPLAY_OBJECTIVE);
            writeString(displayPkt, SIDEBAR_SLOT);
            writeString(displayPkt, SIDEBAR_OBJECTIVE);
            writeString(displayPkt, title);
            writeString(displayPkt, DUMMY_CRITERIA);
            writeVarInt(displayPkt, static_cast<int32_t>(ObjectiveSortOrder::Ascending));
            sendRawPacket(player, displayPkt);

            // 3. 添加所有新条目
            std::string scorePkt;
            writeUnsignedVarInt(scorePkt, PKT_ID_SET_SCORE);
            writeByte(scorePkt, 0);  // type: 0 = Change
            writeUnsignedVarInt(scorePkt, static_cast<uint32_t>(lines.size()));
            for (int i = 0; i < static_cast<int>(lines.size()); i++) {
                writeVarInt64(scorePkt, makeScoreboardId(lines[i]));
                writeString(scorePkt, SIDEBAR_OBJECTIVE);
                writeSignedInt32(scorePkt, i + 1);
                writeByte(scorePkt, 3);  // identityType: 3 = FakePlayer
                writeString(scorePkt, lines[i]);
            }
            sendRawPacket(player, scorePkt);
        } else {
            // ===== 标题未变：增量更新（避免闪烁，仅更新变化的条目）=====

            // 1. 移除不再存在的旧条目
            //    SetScore(Remove) 格式: VarUInt(108) + byte(1=Remove) + VarUInt count
            //    + 每条: VarInt64 scoreboardId + string objName + writeSignedInt scoreValue
            //    注意: Remove 类型也必须写入 scoreValue（Protocol-v944 规范），
            //          但不写 identityType 和 fakePlayerName
            std::unordered_set<std::string> newLineSet(lines.begin(), lines.end());
            std::vector<std::string> toRemove;
            toRemove.reserve(oldLines.size());
            for (auto const& oldLine : oldLines) {
                if (newLineSet.find(oldLine) == newLineSet.end()) {
                    toRemove.push_back(oldLine);
                }
            }

            if (!toRemove.empty()) {
                std::string removeScorePkt;
                writeUnsignedVarInt(removeScorePkt, PKT_ID_SET_SCORE);
                writeByte(removeScorePkt, 1);  // type: 1 = Remove
                writeUnsignedVarInt(removeScorePkt, static_cast<uint32_t>(toRemove.size()));
                for (auto const& line : toRemove) {
                    writeVarInt64(removeScorePkt, makeScoreboardId(line));
                    writeString(removeScorePkt, SIDEBAR_OBJECTIVE);
                    writeSignedInt32(removeScorePkt, 0);  // scoreValue: Remove 时被忽略，但必须写入
                }
                sendRawPacket(player, removeScorePkt);
            }

            // 2. 添加/更新所有新条目
            std::string scorePkt;
            writeUnsignedVarInt(scorePkt, PKT_ID_SET_SCORE);
            writeByte(scorePkt, 0);  // type: 0 = Change
            writeUnsignedVarInt(scorePkt, static_cast<uint32_t>(lines.size()));
            for (int i = 0; i < static_cast<int>(lines.size()); i++) {
                writeVarInt64(scorePkt, makeScoreboardId(lines[i]));
                writeString(scorePkt, SIDEBAR_OBJECTIVE);
                writeSignedInt32(scorePkt, i + 1);
                writeByte(scorePkt, 3);  // identityType: 3 = FakePlayer
                writeString(scorePkt, lines[i]);
            }
            sendRawPacket(player, scorePkt);
        }
    } catch (...) {}
}

void SidebarManager::sendRemoveSidebarPackets(Player* player) {
    if (!player) return;
    try {
        // 构造 RemoveObjective 原始字节流（ID 106）
        // 格式: VarUInt(106) + string objectiveName
        std::string removePkt;
        writeUnsignedVarInt(removePkt, PKT_ID_REMOVE_OBJECTIVE);
        writeString(removePkt, SIDEBAR_OBJECTIVE);
        sendRawPacket(player, removePkt);
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
