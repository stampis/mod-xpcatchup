/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>
 */

#include "RandomPlayerbotMgr.h"
#include "XPCatchup.h"
#include "Chat.h"
#include "Group.h"
#include "WorldPacket.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ScriptMgr.h"
#include "World.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

// Static member definitions
namespace XPCatchup
{
    bool _enabled = true;
    uint32 _levelWindow = 5;
    uint32 _distribution = 0;
    uint32 _threshold = 10;
    bool _requireRealMaster = true;
    bool _chatdebug = false;
    bool _logging = false;

    std::unordered_map<ObjectGuid, PendingXP> _pendingXP;
}

using namespace XPCatchup;

// Helper: Check if a player is within the level window of the reference player
static bool IsWithinLevelWindow(Player* member, Player* reference)
{
    int8 levelDiff = std::abs(reference->GetLevel() - member->GetLevel());
    return levelDiff <= static_cast<int8>(_levelWindow);
}

static bool IsExcludedPlayer(Player* player)
{
    if (!player)
        return true;

     return sRandomPlayerbotMgr.IsRandomBot(player);
}

static bool IsExpectedContributor(Player* member, Unit* victim, Player* currentPlayer)
{
    if (!member || !victim)
        return false;

    // Random bots are completely ignored by XP Catch-Up.
    if (IsExcludedPlayer(member))
        return false;

    return member == currentPlayer || member->IsAtGroupRewardDistance(victim);
}

static float GetProgressRatio(Player* player)
{
    uint32 nextLevelXP = player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
    return (nextLevelXP > 0) ? (float)player->GetUInt32Value(PLAYER_XP) / nextLevelXP : 0.0f;
}

// Reference player = highest-level player eligible for XP on this kill.
// Ties on level are broken by progress ratio toward the next level.
static Player* FindReferencePlayer(Group* group, Unit* victim, Player* currentPlayer)
{
    if (!group || !victim)
        return nullptr;

    Player* reference = nullptr;
    uint8 highestLevel = 0;
    float highestRatio = -1.0f;

    group->DoForAllMembers([&](Player* member)
    {
        if (!IsExpectedContributor(member, victim, currentPlayer))
            return;

        uint8 memberLevel = member->GetLevel();
        float memberRatio = GetProgressRatio(member);

        if (!reference || memberLevel > highestLevel || (memberLevel == highestLevel && memberRatio > highestRatio))
        {
            reference = member;
            highestLevel = memberLevel;
            highestRatio = memberRatio;
        }
    });

    return reference;
}

// Count the players that AzerothCore will actually process for kill XP on this victim.
// This must match KillRewarder group iteration more closely than GetMembersCount(),
// otherwise the pool can wait forever on offline/out-of-range members.
static uint32 GetExpectedContributorCount(Group* group, Unit* victim, Player* currentPlayer)
{
    if (!group || !victim)
        return 0;

    uint32 count = 0;
    group->DoForAllMembers([&](Player* member)
    {
        if (IsExpectedContributor(member, victim, currentPlayer))
            ++count;
    });

    return count;
}

// Helper: Find the single lowest-XP eligible member
static Player* FindLowestXPMember(Group* group, Player* reference, Unit* victim, Player* currentPlayer, ObjectGuid excludeGUID)
{
    if (!reference)
        return nullptr;

    Player* lowest = nullptr;
    float lowestRatio = 999999.0f;

    group->DoForAllMembers([&](Player* member)
    {
        if (!IsExpectedContributor(member, victim, currentPlayer))
            return;
        if (member->GetGUID() == excludeGUID)
            return;
        if (!IsWithinLevelWindow(member, reference))
            return;

        float ratio = GetProgressRatio(member);

        if (ratio < lowestRatio)
        {
            lowestRatio = ratio;
            lowest = member;
        }
    });

    return lowest;
}

// Helper: Find the reference player's XP deficit weight
static int32 GetDeficit(Player* member, Player* reference)
{
    float referenceRatio = GetProgressRatio(reference);
    uint32 referenceLevel = reference->GetLevel();

    float memberRatio = GetProgressRatio(member);
    uint32 levelDeficit = referenceLevel - member->GetLevel();
    return static_cast<int32>(levelDeficit * 10000.0f)
         + static_cast<int32>((referenceRatio - memberRatio) * 10000.0f);
}

// Helper: Find dynamic targets with weighted XP distribution
static std::vector<TargetShare> FindDynamicTargets(Group* group, Player* reference, Unit* victim, Player* currentPlayer)
{
    std::vector<TargetShare> targets;
    uint32 totalWeight = 0;
    float referenceRatio = GetProgressRatio(reference);
    uint32 nonContributors = 0;
    uint32 outsideWindow = 0;
    std::vector<std::string> sampleIncluded;
    std::vector<std::string> sampleOutsideWindow;

    group->DoForAllMembers([&](Player* member)
    {
        if (!IsExpectedContributor(member, victim, currentPlayer))
        {
            ++nonContributors;
            return;
        }

        float memberRatio = GetProgressRatio(member);
        int32 deficit = GetDeficit(member, reference);
        int32 levelDiff = static_cast<int32>(reference->GetLevel()) - static_cast<int32>(member->GetLevel());

        if (!IsWithinLevelWindow(member, reference))
        {
            ++outsideWindow;
            if (sampleOutsideWindow.size() < 5)
            {
                std::ostringstream oss;
                oss << member->GetName() << "(lvl " << static_cast<uint32>(member->GetLevel()) << ", diff " << levelDiff << ")";
                sampleOutsideWindow.push_back(oss.str());
            }
            return;
        }

        uint32 weight = deficit > 0 ? static_cast<uint32>(deficit) : 1;
        totalWeight += weight;
        targets.push_back({member, weight});

        if (sampleIncluded.size() < 5)
        {
            std::ostringstream oss;
            oss << member->GetName() << "(lvl " << static_cast<uint32>(member->GetLevel()) << ", w " << weight << ", d " << deficit << ")";
            sampleIncluded.push_back(oss.str());
        }
    });

    if (_logging)
    {
        std::ostringstream oss;
        oss << "[XP Catch-Up] Dynamic: ref=" << reference->GetName()
            << " lvl=" << static_cast<uint32>(reference->GetLevel())
            << " ratio=" << std::fixed << std::setprecision(4) << referenceRatio
            << " targets=" << targets.size()
            << " weight=" << totalWeight
            << " outWin=" << outsideWindow
            << " nonXP=" << nonContributors;

        if (!sampleIncluded.empty())
        {
            oss << "\n  in: ";
            for (size_t i = 0; i < sampleIncluded.size(); ++i)
            {
                if (i)
                    oss << ", ";
                oss << sampleIncluded[i];
            }
            if (targets.size() > sampleIncluded.size())
                oss << ", ...";
        }

        if (!sampleOutsideWindow.empty())
        {
            oss << "\n  out: ";
            for (size_t i = 0; i < sampleOutsideWindow.size(); ++i)
            {
                if (i)
                    oss << ", ";
                oss << sampleOutsideWindow[i];
            }
            if (outsideWindow > sampleOutsideWindow.size())
                oss << ", ...";
        }

        LOG_INFO("XPCatchup", "{}", oss.str());
    }

    return targets;
}

// Check if any group member has >= threshold XP deficit relative to the reference player.
// Returns false when the group is close enough that catchup is unnecessary.
static bool HasCatchupNeed(Group* group, Player* reference, Unit* victim, Player* currentPlayer, bool shouldLog)
{
    float referenceRatio = GetProgressRatio(reference);
    bool found = false;
    uint32 eligibleMembers = 0;
    uint32 membersBehindThreshold = 0;
    float maxDeficitPct = 0.0f;
    std::vector<std::string> behindSample;

    group->DoForAllMembers([&](Player* member)
    {
        if (!IsExpectedContributor(member, victim, currentPlayer))
            return;
        if (!IsWithinLevelWindow(member, reference))
            return;

        ++eligibleMembers;
        float memberRatio = GetProgressRatio(member);
        float deficitPct = (referenceRatio - memberRatio) * 100.0f;
        maxDeficitPct = std::max(maxDeficitPct, deficitPct);

        if ((referenceRatio - memberRatio) >= _threshold * 0.01f)
        {
            found = true;
            ++membersBehindThreshold;

            if (behindSample.size() < 5)
            {
                std::ostringstream oss;
                oss << member->GetName() << "(" << std::fixed << std::setprecision(2) << deficitPct << "%)";
                behindSample.push_back(oss.str());
            }
        }
    });

    if (_logging && shouldLog)
    {
        std::ostringstream oss;
        oss << "[XP Catch-Up] Check: ref=" << reference->GetName()
            << " lvl=" << static_cast<uint32>(reference->GetLevel())
            << " th=" << _threshold << "%"
            << " elig=" << eligibleMembers
            << " behind=" << membersBehindThreshold
            << " max=" << std::fixed << std::setprecision(2) << maxDeficitPct << "%"
            << " result=" << (found ? "need" : "skip");

        if (!behindSample.empty())
        {
            oss << "\n  behind: ";
            for (size_t i = 0; i < behindSample.size(); ++i)
            {
                if (i)
                    oss << ", ";
                oss << behindSample[i];
            }
            if (membersBehindThreshold > behindSample.size())
                oss << ", ...";
        }

        LOG_INFO("XPCatchup", "{}", oss.str());
    }

    return found;
}

// Helper: Distribute XP pool among targets proportionally by weight
static void DistributeXP(uint32 pool, std::vector<TargetShare>& targets, Unit* victim, Player* reference, Group* group)
{
    if (targets.empty())
        return;

    uint32 totalWeight = 0;
    for (auto& t : targets)
        totalWeight += t.weight;

    uint32 distributed = 0;
    std::vector<uint32> shares;
    std::vector<int32> deficits;

    // Save XP deficits before giving XP (state before this kill)
    float referenceRatio = GetProgressRatio(reference);
    uint32 referenceLevel = reference->GetLevel();

    for (auto& t : targets)
    {
        float memberRatio = GetProgressRatio(t.player);
        uint32 levelDeficit = referenceLevel - t.player->GetLevel();
        int32 totalDeficit = static_cast<int32>(levelDeficit * 10000.0f)
                           + static_cast<int32>((referenceRatio - memberRatio) * 10000.0f);
        deficits.push_back(totalDeficit);
    }

    // Give each target their proportional share
    for (auto& t : targets)
    {
        uint32 share = (pool * t.weight) / totalWeight;
        t.player->GiveXP(share, victim, 1.0f);
        distributed += share;
        shares.push_back(share);
    }

    // Handle rounding: give 1 extra XP to the first target to close the gap
    if (distributed < pool)
    {
        shares[0] += pool - distributed;
        targets[0].player->GiveXP(pool - distributed, victim, 1.0f);
    }

    // Debug: log what each character received to party chat
    if (_chatdebug)
    {
        size_t idx = 0;
        for (auto& t : targets)
        {
            uint32 share = shares[idx];
            int32 deficit = deficits[idx];
            int32 displayDeficit = std::max(0, deficit);
            float deficitPct = displayDeficit / 100.0f;

            if (_logging)
            {
                LOG_INFO("XPCatchup", "[XP Catch-Up] {} received {} XP (weight {}; deficit: {:.2f}%)",
                    t.player->GetName(), share, t.weight, deficitPct);
            }

            std::ostringstream msg;
            msg << "[XP Catch-Up] received " << share << " XP (weight " << t.weight
                << "; deficit: " << std::fixed << std::setprecision(2) << deficitPct << "%)";

            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, LANG_UNIVERSAL, t.player, nullptr, msg.str());
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->GetSession())
                    member->GetSession()->SendPacket(&data);
            }
            idx++;
        }
    }
    else
    {
        // log distribution summary to console (if _logging)
        if (_logging)
        {
            std::ostringstream oss;
            oss << "[XP Catch-Up] Distribution: total=" << pool
                << " targets=" << targets.size();

            size_t perLine = 0;
            for (auto& t : targets)
            {
                std::ostringstream entry;
                entry << t.player->GetName() << "=" << (pool * t.weight) / totalWeight;
                std::string part = entry.str();

                if (perLine == 0)
                {
                    oss << "\n  out: " << part;
                    perLine = 1;
                }
                else if (perLine >= 6)
                {
                    oss << "\n  out: " << part;
                    perLine = 1;
                }
                else
                {
                    oss << ", " << part;
                    ++perLine;
                }
            }

            LOG_INFO("XPCatchup", "{}", oss.str());
        }
    }
}

// Main hook: OnPlayerGiveXP
void XPCatchupPlayerScript::OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource)
{
    // Random bots are completely ignored by XP Catch-Up.
    if (IsExcludedPlayer(player))
        return;

    // Only intercept kill XP
    if (xpSource != XPSOURCE_KILL)
        return;

    // Only if player is in a group
    Group* group = player->GetGroup();
    if (!group)
        return;

    // Check if module is enabled
    if (!_enabled)
        return;

    // Use the highest-level eligible player for this kill as the reference point.
    // Catchup pauses once everyone eligible is within the configured threshold of that player.
    Player* reference = FindReferencePlayer(group, victim, player);
    if (!reference)
        return;

    bool shouldLogThisKill = _logging && player == reference;

    if (!HasCatchupNeed(group, reference, victim, player, shouldLogThisKill))
    {
        if (shouldLogThisKill)
        {
            LOG_INFO("XPCatchup", "[XP Catch-Up] Skip: ref={} lvl={} mob={} all within {}%",
                reference->GetName(), reference->GetLevel(), victim->GetEntry(), _threshold);
        }
        return;
    }

    // Check if this victim is already pending
    auto it = _pendingXP.find(victim->GetGUID());
    bool isFirst = (it == _pendingXP.end());

    if (isFirst)
    {
        // First player: their XP goes into the pool
        _pendingXP[victim->GetGUID()] = { amount, 1, time(NULL), { player->GetGUID() } };

        uint32 expectedContributors = GetExpectedContributorCount(group, victim, player);

        if (_logging)
        {
            LOG_INFO("XPCatchup", "[XP Catch-Up] Pool start: mob={} first={} xp={} expect={}",
                victim->GetEntry(), player->GetName(), amount, expectedContributors);
        }

        // Zero this player's amount so only the pool has this XP
        amount = 0;

        if (expectedContributors > 1)
            return;

        it = _pendingXP.find(victim->GetGUID());
    }
    else
    {
        // Not first player — add their XP to the redistribution pool and zero their share
        it->second.total += amount;
        it->second.count++;
        it->second.timestamp = time(NULL);  // Reset timestamp on each contribution

        // Only add contributor if not already in the list (defensive check)
        bool alreadyContributed = false;
        for (const auto& guid : it->second.contributors)
        {
            if (guid == player->GetGUID())
            {
                alreadyContributed = true;
                break;
            }
        }
        if (!alreadyContributed)
            it->second.contributors.push_back(player->GetGUID());

        amount = 0;
    }

    uint32 expectedContributors = GetExpectedContributorCount(group, victim, player);

    if (_logging && it->second.contributors.size() >= expectedContributors)
    {
        LOG_INFO("XPCatchup", "[XP Catch-Up] Pool ready: mob={} xp={} contrib={}/{}",
            victim->GetEntry(), it->second.total, it->second.contributors.size(), expectedContributors);
    }

    // Redistribute pool when all expected XP callbacks for this victim have arrived.
    // Using GetMembersCount() breaks when some group members are offline/out of range.
    if (expectedContributors > 0 && it->second.contributors.size() >= expectedContributors)
    {
        Player* reference = FindReferencePlayer(group, victim, player);
        if (!reference)
        {
            if (_logging)
            {
                LOG_INFO("XPCatchup", "[XP Catch-Up] No reference player found for victim {}. Pool discarded ({})",
                    victim->GetEntry(), it->second.total);
            }
            _pendingXP.erase(it);
            return;
        }

        if (_logging)
        {
            LOG_INFO("XPCatchup", "[XP Catch-Up] Redistribute: ref={} lvl={} xp={} contrib={}",
                reference->GetName(), reference->GetLevel(), it->second.total, it->second.contributors.size());
        }

        if (_distribution == 1)
        {
            auto targets = FindDynamicTargets(group, reference, victim, player);
            DistributeXP(it->second.total, targets, victim, reference, group);
        }
        else
        {
            Player* target = FindLowestXPMember(group, reference, victim, player, ObjectGuid::Empty);
            if (target)
            {
                if (_logging)
                {
                    LOG_INFO("XPCatchup", "[XP Catch-Up] Lowest mode: giving {} XP to {} (ratio {:.4f}) -> reference {} ratio {:.4f}",
                        it->second.total, target->GetName(), GetProgressRatio(target), reference->GetName(), GetProgressRatio(reference));
                }

                float targetRatio = GetProgressRatio(target);
                float referenceRatio = GetProgressRatio(reference);
                uint32 referenceLevel = reference->GetLevel();

                target->GiveXP(it->second.total, victim, 1.0f);

                if (_chatdebug)
                {
                    uint32 levelDeficit = referenceLevel - target->GetLevel();
                    int32 totalDeficit = static_cast<int32>(levelDeficit * 10000.0f)
                                       + static_cast<int32>((referenceRatio - targetRatio) * 10000.0f);
                    int32 deficit = std::max(0, totalDeficit);
                    float deficitPct = deficit / 100.0f;
                    uint32 weight = deficit > 0 ? static_cast<uint32>(deficit) : 1;

                    std::ostringstream msg;
                    msg << "[XP Catch-Up] received " << it->second.total << " XP (weight " << weight
                        << "; deficit: " << std::fixed << std::setprecision(2) << deficitPct << "%)";

                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, LANG_UNIVERSAL, target, nullptr, msg.str());
                    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    {
                        Player* m = ref->GetSource();
                        if (m && m->GetSession())
                            m->GetSession()->SendPacket(&data);
                    }
                }
            }
            else if (_logging)
            {
                LOG_INFO("XPCatchup", "[XP Catch-Up] Lowest mode: no eligible target found for victim {}. Pool discarded ({})",
                    victim->GetEntry(), it->second.total);
            }
        }

        // Clean up pending XP
        _pendingXP.erase(it);
    }
}

// WorldScript: Initialize config
void XPCatchupWorldScript::OnAfterConfigLoad(bool reload)
{
    _enabled           = sConfigMgr->GetOption<bool>("XPCatchup.Enable", true);
    _levelWindow       = sConfigMgr->GetOption<uint32>("XPCatchup.LevelWindow", 5);
    _distribution      = sConfigMgr->GetOption<uint32>("XPCatchup.Distribution", 0);
    _threshold         = sConfigMgr->GetOption<uint32>("XPCatchup.Threshold", 10);
#ifdef MOD_PLAYERBOTS
    _requireRealMaster = sConfigMgr->GetOption<bool>("XPCatchup.RequireRealMaster", true);
#else
    _requireRealMaster = false; // no-op without mod-playerbots
#endif
    _chatdebug         = sConfigMgr->GetOption<bool>("XPCatchup.ChatDebug", false);
    _logging           = sConfigMgr->GetOption<bool>("XPCatchup.Logging", false);

    if (reload)
    {
        LOG_INFO("XPCatchup", "Configuration reloaded. Enable={}, LevelWindow={}, Distribution={}, RequireRealMaster={} (deprecated/ignored), ChatDebug={}, Logging={}",
            _enabled, _levelWindow, _distribution, _requireRealMaster, _chatdebug, _logging);
    }
    else
    {
        //LOG_INFO("XPCatchup", "XP Catch-Up loaded. Enable={}, LevelWindow={}, Distribution={}, RequireRealMaster={} (deprecated/ignored), ChatDebug={}, Logging={}",
        //    _enabled, _levelWindow, _distribution, _requireRealMaster, _chatdebug, _logging);
    }
}

// WorldScript: Final initialization right before the world becomes operational
void XPCatchupWorldScript::OnBeforeWorldInitialized()
{
    LOG_INFO("server.loading", " ");
    LOG_INFO("server.loading", "╔══════════════════════════════════════════════════════════╗");
    LOG_INFO("server.loading", "║                                                          ║");
    LOG_INFO("server.loading", "║               XP Catch-Up Module                         ║");
    LOG_INFO("server.loading", "║                                                          ║");
    LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
    LOG_INFO("server.loading", "║     Dynamically redistributes kill XP so party members   ║");
    LOG_INFO("server.loading", "║     stay on the same progression track — no more         ║");
    LOG_INFO("server.loading", "║     lagging behind!                                      ║");
    LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
    LOG_INFO("server.loading", "║                  Author: Jellypowered                    ║");
    LOG_INFO("server.loading", "║               Licensed under GNU GPL v2                  ║");
    LOG_INFO("server.loading", "╚══════════════════════════════════════════════════════════╝");

    LOG_INFO("XPCatchup", "XP Catch-Up Config loaded with options:");
    LOG_INFO("XPCatchup", "Enable={}, LevelWindow={}, Distribution={}, RequireRealMaster={} (deprecated/ignored), ChatDebug={}, Logging={}",
        _enabled, _levelWindow, _distribution, _requireRealMaster, _chatdebug, _logging);
}

// WorldScript: Cleanup expired entries periodically
void XPCatchupWorldScript::OnUpdate(uint32 diff)
{
    static uint32 cleanupTimer = 0;
    cleanupTimer += diff;
    if (cleanupTimer < 5000)
        return;
    cleanupTimer = 0;

    time_t now = time(NULL);

    // Clean up expired pending XP entries (older than 10 seconds)
    // The timestamp is reset on each contribution, so this only removes truly stale pools
    for (auto it = _pendingXP.begin(); it != _pendingXP.end(); )
    {
        if (difftime(now, it->second.timestamp) > 10)
            it = _pendingXP.erase(it);
        else
            ++it;
    }
}

// Script loader function
void AddXPCatchupScripts()
{
    new XPCatchupPlayerScript();
    new XPCatchupWorldScript();
}
