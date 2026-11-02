// ScoreboardAdapter.cpp - 原生计分板只读适配层实现
#include "ScoreboardAdapter.h"
#include "Config.h"

#include "ll/api/service/Bedrock.h"

#include "mc/world/level/Level.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/scores/DisplayObjective.h"
#include "mc/world/scores/IdentityDefinition.h"
#include "mc/world/scores/IdentityDictionary.h"
#include "mc/world/scores/Objective.h"
#include "mc/world/scores/ObjectiveCriteria.h"
#include "mc/world/scores/Scoreboard.h"
#include "mc/world/scores/ScoreboardId.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace meowsidebar {

namespace {

constexpr char const* SIDEBAR_SLOT = "sidebar";

using Clock = std::chrono::steady_clock;

// 读取 ScoreboardId 对应的身份定义（不信任 mIdentityDef 指针，直接查表避免悬垂）
IdentityDefinition const* findIdentity(Scoreboard& scoreboard, ScoreboardId const& id) {
    auto const& defs = *scoreboard.mIdentityDict->mIdentityDefs;
    auto        it   = defs.find(id);
    return it == defs.end() ? nullptr : &it->second;
}

// 按 actor unique id 解析名字（身份定义里没存名字时的兜底）
std::string resolveActorName(Level& level, int64_t uniqueId) {
    if (uniqueId == 0) return {};
    for (auto* actor : level.getRuntimeActorList()) {
        if (!actor) continue;
        if (actor->getOrCreateUniqueID().rawID == uniqueId) return actor->getNameTag();
    }
    return {};
}

nlohmann::json snapshotToJson(ScoreboardSnapshot const& snap) {
    nlohmann::json j;
    j["name"]     = snap.name;
    j["display"]  = snap.displayName;
    j["criteria"] = snap.criteria;
    j["count"]    = static_cast<int>(snap.entries.size());
    j["slots"]    = snap.displaySlots;

    nlohmann::json entries = nlohmann::json::array();
    for (auto const& entry : snap.entries) {
        nlohmann::json item;
        item["name"]   = entry.name;
        item["score"]  = entry.score;
        item["type"]   = entry.type;
        item["hidden"] = entry.hidden;
        entries.push_back(std::move(item));
    }
    j["entries"] = std::move(entries);
    return j;
}

} // anonymous namespace

ScoreboardAdapter& ScoreboardAdapter::getInstance() {
    static ScoreboardAdapter instance;
    return instance;
}

void ScoreboardAdapter::init() {
    mMainThreadId = std::this_thread::get_id();
    reloadConfig();
}

void ScoreboardAdapter::shutdown() {
    std::lock_guard<std::mutex> lock(mCacheMutex);
    mCache.clear();
    mObjectiveNames.clear();
    mObjectiveNamesCached = false;
    mLastSidebarSnapshot  = ScoreboardSnapshot{};
}

void ScoreboardAdapter::reloadConfig() {
    int ttl = DEFAULT_CACHE_TTL_MS;
    try {
        ttl = ConfigManager::getInstance().getScoreboardCacheMs();
    } catch (...) {
        ttl = DEFAULT_CACHE_TTL_MS;
    }
    if (ttl < 0) ttl = 0;
    if (ttl > 60000) ttl = 60000;
    mCacheTtlMs.store(ttl);

    std::lock_guard<std::mutex> lock(mCacheMutex);
    mCache.clear();
    mObjectiveNames.clear();
    mObjectiveNamesCached = false;
}

bool ScoreboardAdapter::isMainThread() const {
    if (mMainThreadId == std::thread::id{}) return true; // 未初始化时按主线程处理
    return mMainThreadId == std::this_thread::get_id();
}

bool ScoreboardAdapter::capture(std::string const& objectiveName, ScoreboardSnapshot& out) {
    out = ScoreboardSnapshot{};

    auto levelRef = ll::service::getLevel();
    if (!levelRef) return false;
    Level&      level      = *levelRef;
    Scoreboard& scoreboard = level.getScoreboard();

    Objective* objective = scoreboard.getObjective(objectiveName);
    if (!objective) return false;

    out.valid       = true;
    out.name        = objectiveName;
    out.displayName = std::string(*objective->mDisplayName);
    out.criteria    = std::string(*objective->mCriteria.mName);

    for (auto const& kv : *scoreboard.mDisplayObjectives) {
        if (kv.second.mObjective == objective) out.displaySlots.push_back(kv.first);
    }

    out.entries.reserve(objective->mScores->size());
    for (auto const& kv : *objective->mScores) {
        ScoreboardEntry entry;
        entry.id    = kv.first.mRawID;
        entry.score = kv.second;

        if (auto const* def = findIdentity(scoreboard, kv.first)) {
            entry.type   = static_cast<int>(def->mIdentityType);
            entry.hidden = def->mIsHiddenFakePlayer;
            entry.name   = std::string(*def->mPlayerName);

            if (entry.name.empty()) {
                if (entry.type == static_cast<int>(IdentityDefinition::Type::Player)) {
                    entry.name = resolveActorName(level, def->mPlayerId->mActorUniqueId);
                } else if (entry.type == static_cast<int>(IdentityDefinition::Type::Entity)) {
                    entry.name = resolveActorName(level, def->mEntityId->rawID);
                }
            }
        }
        if (entry.name.empty()) entry.name = "#" + std::to_string(entry.id);

        out.entries.push_back(std::move(entry));
    }

    std::sort(
        out.entries.begin(),
        out.entries.end(),
        [](ScoreboardEntry const& a, ScoreboardEntry const& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.name < b.name;
        }
    );
    return true;
}

ScoreboardSnapshot ScoreboardAdapter::getSnapshot(std::string const& objectiveName) {
    auto const now = Clock::now();
    auto const ttl = std::chrono::milliseconds(mCacheTtlMs.load());

    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto                        it = mCache.find(objectiveName);
        if (it != mCache.end() && now - it->second.capturedAt < ttl) {
            return it->second.snapshot;
        }
    }

    // 非主线程：只返回已有缓存，不触碰 BDS 内部结构
    if (!isMainThread()) {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        auto                        it = mCache.find(objectiveName);
        return it != mCache.end() ? it->second.snapshot : ScoreboardSnapshot{};
    }

    ScoreboardSnapshot snapshot;
    capture(objectiveName, snapshot);

    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        // 防御：外部插件可能用任意名字反复查询，限制缓存规模避免无界增长
        if (mCache.size() > 256) mCache.clear();
        auto& entry      = mCache[objectiveName];
        entry.snapshot   = snapshot;
        entry.capturedAt = now;
    }
    return snapshot;
}

ScoreboardSnapshot ScoreboardAdapter::getSidebarSnapshot() {
    if (!isMainThread()) {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        return mLastSidebarSnapshot;
    }

    ScoreboardSnapshot snapshot;
    auto               levelRef = ll::service::getLevel();
    if (levelRef) {
        auto const* display = levelRef->getScoreboard().getDisplayObjective(SIDEBAR_SLOT);
        if (display && display->mObjective) {
            std::string objectiveName(*display->mObjective->mName);
            snapshot = getSnapshot(objectiveName);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mLastSidebarSnapshot = snapshot;
    }
    return snapshot;
}

std::vector<std::string> ScoreboardAdapter::getObjectiveNames() {
    auto const now = Clock::now();
    auto const ttl = std::chrono::milliseconds(mCacheTtlMs.load());

    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        if (mObjectiveNamesCached && now - mObjectiveNamesAt < ttl) return mObjectiveNames;
    }

    if (!isMainThread()) {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        return mObjectiveNames;
    }

    std::vector<std::string> names;
    if (auto levelRef = ll::service::getLevel()) {
        for (auto const* objective : levelRef->getScoreboard().getObjectives()) {
            if (objective) names.push_back(std::string(*objective->mName));
        }
    }
    std::sort(names.begin(), names.end());

    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        mObjectiveNames       = names;
        mObjectiveNamesAt     = now;
        mObjectiveNamesCached = true;
    }
    return names;
}

ScoreboardSelf ScoreboardAdapter::selfFor(Player* player) {
    ScoreboardSelf self;
    if (!player) return self;

    auto levelRef = ll::service::getLevel();
    if (!levelRef) return self;

    ScoreboardId const& id = levelRef->getScoreboard().getScoreboardId(static_cast<Actor const&>(*player));
    if (id == ScoreboardId::INVALID()) return self;

    self.valid = true;
    self.id    = id.mRawID;
    return self;
}

std::string ScoreboardAdapter::expand(std::string const& text, Player* player) {
    if (text.find('{') == std::string::npos) return text;

    auto names = getObjectiveNames();

    ScoreboardSelf self;
    bool           selfResolved = false;

    std::string out;
    out.reserve(text.size() + 16);

    size_t i = 0;
    while (i < text.size()) {
        char const c = text[i];
        if (c != '{') {
            out.push_back(c);
            ++i;
            continue;
        }

        size_t const close = text.find('}', i + 1);
        if (close == std::string::npos) {
            out.append(text, i, std::string::npos);
            break;
        }

        std::string inner = text.substr(i + 1, close - i - 1);

        bool        handled = false;
        std::string value;

        // {sb.list} / {sb.objectives}：列出所有原生 objective
        if (inner == "sb.list" || inner == "sb.objectives" || inner == "scoreboard.list"
            || inner == "scoreboard.objectives") {
            for (size_t k = 0; k < names.size(); ++k) {
                if (k) value += ',';
                value += names[k];
            }
            handled = true;
        } else {
            ScoreboardToken token;
            if (parseScoreboardToken(inner, names, token) == ScoreboardTokenKind::Parsed) {
                ScoreboardSnapshot snapshot =
                    token.sidebar ? getSidebarSnapshot() : getSnapshot(token.objective);

                if (snapshot.valid) {
                    if (!selfResolved) {
                        self         = selfFor(player);
                        selfResolved = true;
                    }
                    handled = formatScoreboardToken(snapshot, token.suffix, self, value);
                } else if (token.sidebar) {
                    // 当前没有原生计分板显示在 sidebar：输出空，避免把 token 暴露给玩家
                    handled = true;
                    value.clear();
                }
                // 具名 objective 不存在：保留原文，方便排查拼写错误
            }
        }

        if (handled) {
            out += value;
        } else {
            out.append(text, i, close - i + 1);
        }
        i = close + 1;
    }

    return out;
}

std::string ScoreboardAdapter::jsonObjectives() {
    nlohmann::json result = nlohmann::json::array();

    if (isMainThread()) {
        if (auto levelRef = ll::service::getLevel()) {
            auto& scoreboard = levelRef->getScoreboard();
            for (auto const* objective : scoreboard.getObjectives()) {
                if (!objective) continue;

                nlohmann::json item;
                item["name"]     = std::string(*objective->mName);
                item["display"]  = std::string(*objective->mDisplayName);
                item["criteria"] = std::string(*objective->mCriteria.mName);
                item["count"]    = static_cast<int>(objective->mScores->size());

                nlohmann::json slots = nlohmann::json::array();
                for (auto const& kv : *scoreboard.mDisplayObjectives) {
                    if (kv.second.mObjective == objective) slots.push_back(kv.first);
                }
                item["slots"] = std::move(slots);

                result.push_back(std::move(item));
            }
        }
    }

    return result.dump();
}

std::string ScoreboardAdapter::jsonObjective(std::string const& objective) {
    if (objective.empty()) return "{}";
    auto snapshot = getSnapshot(objective);
    if (!snapshot.valid) return "{}";
    return snapshotToJson(snapshot).dump();
}

std::string ScoreboardAdapter::jsonSidebar() {
    auto snapshot = getSidebarSnapshot();
    if (!snapshot.valid) return "{}";
    return snapshotToJson(snapshot).dump();
}

std::string ScoreboardAdapter::scoreOf(std::string const& objective, std::string const& entry) {
    if (objective.empty() || entry.empty()) return {};
    auto snapshot = getSnapshot(objective);
    if (!snapshot.valid) return {};
    return entryScoreText(snapshot, entry);
}

} // namespace meowsidebar
