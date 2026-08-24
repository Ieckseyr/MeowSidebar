// SidebarManager.cpp - 侧边栏核心管理器实现
#include "SidebarManager.h"
#include "Config.h"
#include "PlayerConfigManager.h"
#include "MeowSidebar.h"

#include "meowpapi/PlaceholderApi.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/network/NetworkSystem.h"
#include "mc/network/NetworkPeer.h"
#include "mc/network/Compressibility.h"
#include "mc/world/scores/ObjectiveSortOrder.h"

#include <algorithm>
#include <cstring>
#include <string_view>

// ===== 数据包构造策略 =====
// 经对比 BedrockProtocol-944 (26.10) 与 BedrockProtocol-main (26.40) 两个版本协议库源码，
// 确认 SetScorePacket / SetDisplayObjectivePacket / RemoveObjectivePacket 在 26.10 与 26.40
// 之间 **格式完全一致**，均为 BDS 原生字节格式（非 Cereal variant 字符串格式）。
//
// 之前误判 "Glacie rc.1 严格化要求 Cereal variant 字符串格式" 是错误的：rc.1/rc.2 的差异
// 并非数据包格式变化，而是字段合法性校验更严格。MeowSidebar 只需严格按 BDS 原生字节格式
// 构造即可被 26.10 原生客户端与 26.40 (经 Glacie 转换) 客户端同时正确解析。
//
// === SetScorePacket (ID=108) 原生字节格式 ===
//   Byte(packetType)                         // 0=Change / 1=Remove (全局，只写一次)
//   VarUInt(count)                           // entry 总数
//   for each entry:
//     VarInt64(scoreboardId)                 // ZigZag 64-bit (int64_t)
//     String(objectiveName)                  // "_msb"
//     SignedInt32(scoreValue)                // 4 字节小端 (int32_t) ← 非 VarInt!
//     if packetType == Change:
//       Byte(identityType)                   // 1=Player / 2=Entity / 3=FakePlayer
//       if Player or Entity: VarInt64(actorUniqueId)
//       if FakePlayer:        String(fakePlayerName)
//     // Remove 时无 identityType / 无 payload
//
// === SetDisplayObjectivePacket (ID=107) ===
//   String(displaySlotName)    // "sidebar"
//   String(objectiveName)      // "_msb"
//   String(displayName)        // 标题
//   String(criteriaName)       // "dummy"
//   VarInt(sortOrder)          // ZigZag, 0=Ascending / 1=Descending
//
// === RemoveObjectivePacket (ID=106) ===
//   String(objectiveName)      // "_msb"
//
// 关键点：
// - scoreValue 必须用 4 字节小端 (writeSignedInt32)，不能用 VarInt/ZigZag
// - scoreboardId 必须用 VarInt64 (ZigZag 64-bit)，不能用 32-bit VarInt
// - FakePlayer 条目无需额外发送 SetScoreboardIdentityPacket (仅 Player/Entity 需要)

namespace {

// ===== 手动字节流辅助 =====

// 写入无符号 VarUInt（小端基128变长编码）
void writeUnsignedVarInt(std::string& buf, uint32_t value) {
    while (value >= 0x80) {
        buf.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    buf.push_back(static_cast<char>(value));
}

// 写入有符号 VarInt (ZigZag 32-bit -> VarUInt)
// （用于 SetDisplayObjectivePacket.sortOrder）
void writeVarInt(std::string& buf, int32_t value) {
    uint32_t zz = (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31);
    writeUnsignedVarInt(buf, zz);
}

// 写入有符号 VarInt64 (ZigZag 64-bit -> VarUInt)
// （用于 SetScorePacket.scoreboardId，必须是 64-bit ZigZag）
void writeVarInt64(std::string& buf, int64_t value) {
    uint64_t zz = (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
    while (zz >= 0x80) {
        buf.push_back(static_cast<char>((zz & 0x7F) | 0x80));
        zz >>= 7;
    }
    buf.push_back(static_cast<char>(zz));
}

// 写入 4 字节小端有符号整数 (int32_t)
// （用于 SetScorePacket.scoreValue，必须 4LE，不能用 VarInt/ZigZag）
void writeSignedInt32(std::string& buf, int32_t value) {
    buf.push_back(static_cast<char>(value & 0xFF));
    buf.push_back(static_cast<char>((value >> 8) & 0xFF));
    buf.push_back(static_cast<char>((value >> 16) & 0xFF));
    buf.push_back(static_cast<char>((value >> 24) & 0xFF));
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

// MCBE 数据包头 ID（与协议 wire ID 一致）
constexpr uint32_t PKT_ID_SET_DISPLAY_OBJECTIVE = 107; // SetDisplayObjective
constexpr uint32_t PKT_ID_REMOVE_OBJECTIVE      = 106; // RemoveObjective
constexpr uint32_t PKT_ID_SET_SCORE             = 108; // SetScore

// SetScorePacket::PacketType (与协议库枚举一致)
constexpr uint8_t SCORE_PACKET_TYPE_CHANGE = 0;
constexpr uint8_t SCORE_PACKET_TYPE_REMOVE = 1;

// SetScorePacket::IdentityType (与协议库枚举一致)
constexpr uint8_t SCORE_IDENTITY_PLAYER     = 1;
constexpr uint8_t SCORE_IDENTITY_ENTITY     = 2;
constexpr uint8_t SCORE_IDENTITY_FAKE_PLAYER = 3;

// scoreboardId 基址：偏移到 100000 避免与 BDS 内部分配的小 ID 冲突
// （BDS 的 scoreboardId 从 1 递增）。MeowSidebar 独占管理 "_msb" objective，
// 使用稳定 ID = SCOREBOARD_ID_BASE + lineIndex，每次刷新复用同一批 ID。
//
// 根因确认：sort=2 错误源于 ProtocolLib rc.1 的 Cereal 数值 variant 序列化
// （identityType 写为数值 index），26.40 客户端无法解析。rc.2 改为字符串 tag
// 序列化（如 "changefakeplayer"）后修复。稳定 ID 方案下，Remove 旧条目后
// Change 同 ID 新内容由 rc.2 正确翻译，不会触发 sort=2，也不会产生身份累积。
constexpr int64_t SCOREBOARD_ID_BASE = 100000;

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
            // 检测 [array, number] 嵌套格式（如 [["str1","str2"], 1000]）
            if (!arr[i].empty() && arr[i][0].is_array()) {
                indexes[i].max = static_cast<int>(arr[i][0].size()) - 1;
                // 第二个元素为旋转间隔（ms）
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
            // 检测 [array, number] 嵌套格式：使用内层数组作为旋转值
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
                // 有自定义间隔：累计时间，达到间隔才旋转
                idx.elapsed += deltaMs;
                if (idx.elapsed >= idx.interval) {
                    idx.elapsed = 0;
                    idx.current++;
                    if (idx.current > idx.max) idx.current = 0;
                }
            } else {
                // 无自定义间隔：每次刷新都旋转
                idx.current++;
                if (idx.current > idx.max) idx.current = 0;
            }
        }
    }
}

// ===== SidebarManager 主逻辑 =====

void SidebarManager::init() {
    // 重建 AIM
    auto const& titleCfg = ConfigManager::getInstance().getTitleConfig();
    auto const& dataCfg = ConfigManager::getInstance().getDataConfig();
    {
        std::lock_guard<std::mutex> lock(mAIMMutex);
        mTitleAIM.rebuild(titleCfg.is_array() ? titleCfg : json::array({titleCfg}));
        mDataAIM.rebuild(dataCfg.is_array() ? dataCfg : json::array({dataCfg}));
        mTitleElapsedMs = 0;
    }

    // 注册主线程 tick 事件监听器（所有 BDS API 调用在主线程执行，避免线程安全问题）
    auto& bus = ll::event::EventBus::getInstance();
    mTickListener = bus.emplaceListener<ll::event::world::ServerLevelTickEvent>(
        [this](ll::event::world::ServerLevelTickEvent&) {
            this->onTick();
        }
    );
    mLastRefreshTimeValid = false;
}

void SidebarManager::shutdown() {
    // 移除 tick 事件监听器
    if (mTickListener) {
        auto& bus = ll::event::EventBus::getInstance();
        bus.removeListener(mTickListener);
        mTickListener = nullptr;
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
    {
        std::lock_guard<std::mutex> lock(mAIMMutex);
        mTitleAIM.rebuild(titleCfg.is_array() ? titleCfg : json::array({titleCfg}));
        mDataAIM.rebuild(dataCfg.is_array() ? dataCfg : json::array({dataCfg}));
        mTitleElapsedMs = 0;
    }
}

void SidebarManager::onTick() {
    // 基于配置的 hz 频率节流刷新（ServerLevelTickEvent 每 tick ~50ms 触发一次）
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
        return; // 未达到刷新间隔，跳过
    }

    // 达到刷新间隔，执行刷新
    int deltaMs = static_cast<int>(elapsed);
    mLastRefreshTime = now;

    try {
        refreshAll(deltaMs);
    } catch (...) {
        // 防止异常影响主线程
    }
}

void SidebarManager::refreshAll(int deltaMs) {
    auto level = ll::service::getLevel();
    if (!level) return;

    // 获取所有在线玩家
    level->forEachPlayer([this](Player& player) -> bool {
        try {
            processPlayer(&player);
        } catch (...) {}
        return true;
    });

    // AIM 前进一步（动画效果，按各自间隔旋转）
    std::lock_guard<std::mutex> lock(mAIMMutex);
    // 数据行每次都旋转
    mDataAIM.incrementAll(deltaMs);

    // 标题按 title_refresh_interval 节流旋转，避免每 tick 都触发完整重建
    int titleInterval = ConfigManager::getInstance().getTitleRefreshInterval();
    if (titleInterval > 0) {
        mTitleElapsedMs += deltaMs;
        if (mTitleElapsedMs >= titleInterval) {
            mTitleAIM.incrementAll(mTitleElapsedMs);
            mTitleElapsedMs = 0;
        }
    } else {
        // 未配置节流：每次都旋转（保持原行为）
        mTitleAIM.incrementAll(deltaMs);
    }
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
    std::vector<std::string> titleParts, dataParts;
    {
        std::lock_guard<std::mutex> lock(mAIMMutex);
        titleParts = mTitleAIM.getCurrentValues();
        dataParts = mDataAIM.getCurrentValues();
    }

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

    // 获取旧状态（用于增量更新，移除多余条目）
    std::string oldTitle;
    int oldLineCount = 0;
    bool wasActive = false;
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        auto it = mPlayerStates.find(xuid);
        if (it != mPlayerStates.end()) {
            oldTitle      = it->second.title;
            oldLineCount  = it->second.lineCount;
            wasActive     = it->second.active;
        }
    }

    // 发送侧边栏数据包（带旧状态进行增量更新）
    sendSidebarPackets(player, title, uniqueLines, oldTitle, oldLineCount, wasActive);

    // 更新状态
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        auto& state = mPlayerStates[xuid];
        state.title = title;
        state.lineCount = static_cast<int>(uniqueLines.size());
        state.active = true;
    }
}

void SidebarManager::sendSidebarPackets(
    Player* player,
    std::string const& title,
    std::vector<std::string> const& lines,
    std::string const& oldTitle,
    int oldLineCount,
    bool wasActive
) {
    if (!player || lines.empty()) return;

    try {
        bool titleChanged = (title != oldTitle) || !wasActive;
        int  newLineCount = static_cast<int>(lines.size());

        if (titleChanged) {
            // ===== 标题变更或首次显示：完全重建 =====
            // 1. 移除旧 objective（清除所有旧分数条目及关联身份，防止残留）
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

            // 3. 添加所有新条目（BDS 原生字节格式）
            //    结构: VarUInt(108) + Byte(0=Change) + VarUInt(count) + 每条:
            //      VarInt64(sbId, ZigZag) + String("_msb") + SignedInt32(scoreValue 4LE) +
            //      Byte(3=FakePlayer) + String(fakePlayerName)
            //
            //    scoreboardId 使用稳定 ID：SCOREBOARD_ID_BASE + lineIndex（每次复用）
            std::string scorePkt;
            writeUnsignedVarInt(scorePkt, PKT_ID_SET_SCORE);
            writeByte(scorePkt, SCORE_PACKET_TYPE_CHANGE);          // packetType = Change
            writeUnsignedVarInt(scorePkt, static_cast<uint32_t>(lines.size()));
            for (int i = 0; i < newLineCount; i++) {
                int64_t sbId = SCOREBOARD_ID_BASE + static_cast<int64_t>(i);
                writeVarInt64(scorePkt, sbId);                      // scoreboardId (VarInt64 ZigZag)
                writeString(scorePkt, SIDEBAR_OBJECTIVE);           // objectiveName
                writeSignedInt32(scorePkt, i + 1);                  // scoreValue (4LE, Ascending: 小在上)
                writeByte(scorePkt, SCORE_IDENTITY_FAKE_PLAYER);    // identityType = FakePlayer
                writeString(scorePkt, lines[i]);                    // fakePlayerName
            }
            sendRawPacket(player, scorePkt);
        } else {
            // ===== 标题未变：增量更新（稳定 ID 复用） =====
            // 先 Remove 旧条目（按旧行数），再 Change 新条目（同批稳定 ID，新内容）。
            // scoreboardId = SCOREBOARD_ID_BASE + lineIndex，每次刷新复用相同 ID。
            // rc.2 ProtocolLib 将 identityType 正确序列化为 Cereal 字符串 tag，
            // 26.40 客户端可正确解析 Remove + Change 同 ID 的操作，不触发 sort=2。
            // 稳定 ID 复用避免 generation 方案的身份累积问题。

            // 1. 移除所有旧条目（BDS 原生字节格式 Remove）
            //    结构: VarUInt(108) + Byte(1=Remove) + VarUInt(count) + 每条:
            //      VarInt64(sbId, ZigZag) + String("_msb") + SignedInt32(scoreValue 4LE)
            //    (Remove 时无 identityType / 无 payload)
            if (oldLineCount > 0) {
                std::string removeScorePkt;
                writeUnsignedVarInt(removeScorePkt, PKT_ID_SET_SCORE);
                writeByte(removeScorePkt, SCORE_PACKET_TYPE_REMOVE);     // packetType = Remove
                writeUnsignedVarInt(removeScorePkt, static_cast<uint32_t>(oldLineCount));
                for (int i = 0; i < oldLineCount; i++) {
                    int64_t sbId = SCOREBOARD_ID_BASE + static_cast<int64_t>(i);
                    writeVarInt64(removeScorePkt, sbId);                 // scoreboardId (VarInt64 ZigZag)
                    writeString(removeScorePkt, SIDEBAR_OBJECTIVE);       // objectiveName
                    writeSignedInt32(removeScorePkt, 0);                  // scoreValue (Remove 也写)
                }
                sendRawPacket(player, removeScorePkt);
            }

            // 2. 添加所有新条目（BDS 原生字节格式 Change + FakePlayer）
            //    复用同一批稳定 ID（SCOREBOARD_ID_BASE + i）
            std::string scorePkt;
            writeUnsignedVarInt(scorePkt, PKT_ID_SET_SCORE);
            writeByte(scorePkt, SCORE_PACKET_TYPE_CHANGE);          // packetType = Change
            writeUnsignedVarInt(scorePkt, static_cast<uint32_t>(lines.size()));
            for (int i = 0; i < newLineCount; i++) {
                int64_t sbId = SCOREBOARD_ID_BASE + static_cast<int64_t>(i);
                writeVarInt64(scorePkt, sbId);                      // scoreboardId (VarInt64 ZigZag)
                writeString(scorePkt, SIDEBAR_OBJECTIVE);           // objectiveName
                writeSignedInt32(scorePkt, i + 1);                  // scoreValue (4LE, Ascending)
                writeByte(scorePkt, SCORE_IDENTITY_FAKE_PLAYER);    // identityType = FakePlayer
                writeString(scorePkt, lines[i]);                    // fakePlayerName
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

void SidebarManager::onPlayerDisconnect(Player* player) {
    if (!player) return;
    std::string xuid;
    try { xuid = player->getXuid(); } catch (...) { return; }
    if (xuid.empty()) return;
    // 断连时不发包（连接已关闭，发了也收不到），直接 erase 状态条目防止内存泄漏。
    // 玩家重连后是全新连接，客户端 IdentityDictionary 已清空，
    // 稳定 ID 从 SCOREBOARD_ID_BASE 重新开始分配，无冲突风险。
    std::lock_guard<std::mutex> lock(mStateMutex);
    mPlayerStates.erase(xuid);
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
