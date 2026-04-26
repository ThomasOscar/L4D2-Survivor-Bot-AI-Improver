/**
 * AI Task: Navigate
 * 
 * Priority Order: 3
 * Handles following the team leader, maintaining formation,
 * avoiding bunching, and detecting/resolving stuck situations.
 *
 * Aggressive Leader Mode:
 *   When all human players are dead AND no defib is available (no bot
 *   carries one, none on the ground nearby), a bot leader is elected
 *   to aggressively push the map forward. Bots will NOT flee from
 *   threats — they fight while advancing. If a rescue closet is found
 *   ahead, the leader navigates the team toward it to rescue dead
 *   teammates. When a human player is revived, the mode ends and
 *   normal player-leader behavior resumes.
 */

#if defined _task_navigate_included
    #endinput
#endif
#define _task_navigate_included

// Navigation constants
#define NAV_FOLLOW_FAR          400.0   // Start moving toward leader
#define NAV_FOLLOW_CLOSE        150.0   // Stop following (too close)
#define NAV_SPREAD_DIST         80.0    // Min distance between bots
#define NAV_STUCK_CHECK_TIME    2.0     // Seconds between stuck checks
#define NAV_STUCK_THRESHOLD     100.0   // Squared distance threshold for stuck
#define NAV_STUCK_MAX_TIME      6.0     // Max stuck time before random escape
#define NAV_SLOW_SPEED          200.0   // Slow follow speed
#define NAV_DEFIB_SEARCH_RANGE  800.0   // Must match DEFIB_SEARCH_RANGE in task_defib.inc

// Aggressive leader mode constants
#define NAV_RESCUE_CLOSET_SEARCH_RANGE  2000.0  // How far ahead to search for rescue closets
#define NAV_RESCUE_CLOSET_CHECK_INTERVAL 10.0   // Re-scan for closets every N seconds
#define NAV_TEAM_COHESION_RANGE         600.0   // Max distance non-leader bots can be from leader before teleport
#define NAV_TEAM_COHESION_CHECK_TIME    3.0     // Seconds before teleporting a bot that's too far

// Lv6 Cheat mode constants
#define LV6_TELEPORT_DIST     800.0    // Bot farther than this from player triggers teleport timer
#define LV6_FOLLOW_RANGE      200.0    // Lv6: tight follow range around player
#define LV6_SPREAD_DIST       120.0    // Lv6: slightly larger spread to not block player

// Per-bot navigation state
static float g_fNavLastPos[MAXPLAYERS + 1][3];
static float g_fNavLastCheckTime[MAXPLAYERS + 1];
static int g_iNavStuckCount[MAXPLAYERS + 1];

// Lv6 Cheat: per-bot teleport timer
static float g_fLv6LastTeleport[MAXPLAYERS + 1];

// Aggressive leader mode state
static bool g_bAggressiveLeaderMode = false;      // Currently in aggressive leader mode
static int g_iLeaderBot = -1;                      // The elected leader bot client index
static float g_fRescueClosetPos[3];                // Nearest rescue closet position
static bool g_bHasRescueCloset = false;             // Whether a rescue closet was found
static float g_fLastClosetCheckTime = 0.0;         // Last time we scanned for closets
static float g_fFarFromLeaderSince[MAXPLAYERS + 1]; // When each bot first became too far from leader
static float g_fLastDefibNearbyCheckTime = 0.0;    // Throttle for DefibExistsNearby scan
static bool g_bDefibNearby = false;                 // Cached result of DefibExistsNearby
static float g_fLastFlowSampleTime = 0.0;           // Throttle for leader flow direction sampling
static float g_fCachedFlowTarget[3];                // Cached best forward direction target
static bool g_bCachedFlowFound = false;             // Whether cached target is valid

// ============================================================
// Aggressive Leader Mode
// ============================================================

/**
 * Check if any survivor teammate is dead AND actually placed in a rescue closet
 * waiting for someone to open the door. In L4D2, not all dead players get placed
 * in closets — only when the Director creates rescuable survivors ahead.
 */
static bool TaskNavigate_AnyTeammateInRescueCloset() {
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (IsPlayerAlive(i)) continue;
        // Only count dead players who are actually waiting in a rescue closet
        if (HasEntProp(i, Prop_Send, "m_isWaitingToRescue")) {
            if (GetEntProp(i, Prop_Send, "m_isWaitingToRescue") != 0) return true;
        }
    }
    return false;
}

/**
 * Check if any survivor teammate is dead (for defib/revive purposes).
 */
static bool TaskNavigate_AnyTeammateDead() {
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (!IsPlayerAlive(i)) return true;
    }
    return false;
}

/**
 * Check if any human survivor is alive.
 */
static bool TaskNavigate_AnyHumanAlive() {
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i) || IsFakeClient(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (IsPlayerAlive(i)) return true;
    }
    return false;
}

/**
 * Check if any bot has a defibrillator in slot 3.
 */
static bool TaskNavigate_AnyBotHasDefib() {
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i) || !IsFakeClient(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (!IsPlayerAlive(i)) continue;
        int weapon = GetPlayerWeaponSlot(i, 3);
        if (weapon != -1) {
            char cls[64];
            GetEntityClassname(weapon, cls, sizeof(cls));
            if (StrEqual(cls, "weapon_defibrillator")) return true;
        }
    }
    return false;
}

/**
 * Check if a defibrillator exists on the ground or as a spawn within range
 * of any alive survivor bot. This MUST match DEFIB_SEARCH_RANGE so that
 * when it returns true, the defib task can actually find and reach the defib.
 * Results are cached for 5 seconds to avoid expensive entity scans every tick.
 */
static bool TaskNavigation_DefibExistsNearby() {
    float curTime = GetGameTime();
    if (curTime - g_fLastDefibNearbyCheckTime < 5.0) {
        return g_bDefibNearby;
    }
    g_fLastDefibNearbyCheckTime = curTime;

    g_bDefibNearby = false;

    // Get reference position: most-forward alive survivor
    float refPos[3];
    bool hasRef = false;
    float bestFlow = -1.0;
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        float flow = L4D2Direct_GetFlowDistance(i);
        if (flow > bestFlow) {
            bestFlow = flow;
            GetClientAbsOrigin(i, refPos);
            hasRef = true;
        }
    }
    if (!hasRef) return false;

    int ent = -1;
    int safetyCount = 0;
    while ((ent = FindEntityByClassname(ent, "weapon_defibrillator")) != -1) {
        if (++safetyCount > 256) break;
        if (!IsValidEntity(ent)) { ent++; continue; }
        int owner = GetEntPropEnt(ent, Prop_Data, "m_hOwnerEntity");
        if (owner > 0 && owner <= MaxClients && IsPlayerAlive(owner)) continue;

        // Range check: must be reachable by defib task
        float entPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);
        float dist = AI_GetVectorDistance(refPos, entPos);
        if (dist > NAV_DEFIB_SEARCH_RANGE) continue;

        g_bDefibNearby = true;
        return true;
    }
    ent = -1;
    safetyCount = 0;
    while ((ent = FindEntityByClassname(ent, "weapon_defibrillator_spawn")) != -1) {
        if (++safetyCount > 256) break;
        if (!IsValidEntity(ent)) { ent++; continue; }
        int offset = FindDataMapInfo(ent, "m_nCount");
        if (offset > 0) {
            if (GetEntProp(ent, Prop_Data, "m_nCount") <= 0) continue;
        }

        // Range check: must be reachable by defib task
        float entPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);
        float dist = AI_GetVectorDistance(refPos, entPos);
        if (dist > NAV_DEFIB_SEARCH_RANGE) continue;

        g_bDefibNearby = true;
        return true;
    }
    return false;
}

/**
 * Update aggressive leader mode state.
 * Enters aggressive mode when: all humans dead AND no defib available.
 * Exits when: any human is alive again (rescued from closet or defibbed).
 */
static void TaskNavigate_UpdateAggressiveMode() {
    bool anyHumanAlive = TaskNavigate_AnyHumanAlive();

    if (anyHumanAlive) {
        // A human is alive — exit aggressive mode
        if (g_bAggressiveLeaderMode) {
            g_bAggressiveLeaderMode = false;
            g_iLeaderBot = -1;
            g_bHasRescueCloset = false;
            if (g_bDebugMode) {
                PrintToServer("[AI Bot] Aggressive leader mode OFF — human player is alive");
            }
        }
        return;
    }

    // All humans dead — check if we can use defib
    if (g_bDefibEnabled) {
        if (TaskNavigate_AnyBotHasDefib() || TaskNavigation_DefibExistsNearby()) {
            // Defib is available — let the defib task handle it, don't go aggressive yet
            if (g_bAggressiveLeaderMode) {
                g_bAggressiveLeaderMode = false;
                g_iLeaderBot = -1;
                g_bHasRescueCloset = false;
            }
            return;
        }
    }

    // All humans dead + no defib available — enter aggressive leader mode
    if (!g_bAggressiveLeaderMode) {
        g_bAggressiveLeaderMode = true;
        g_bHasRescueCloset = false;
        g_fLastClosetCheckTime = 0.0;

        // Elect leader: the bot with highest flow distance
        g_iLeaderBot = -1;
        float bestFlow = -1.0;
        for (int i = 1; i <= MaxClients; i++) {
            if (!IsClientInGame(i) || !IsFakeClient(i)) continue;
            if (GetClientTeam(i) != 2 || !IsPlayerAlive(i)) continue;
            if (GetEntProp(i, Prop_Send, "m_isIncapacitated") != 0) continue;
            float flow = L4D2Direct_GetFlowDistance(i);
            if (flow > bestFlow) {
                bestFlow = flow;
                g_iLeaderBot = i;
            }
        }

        if (g_iLeaderBot > 0) {
            char name[64]; GetClientName(g_iLeaderBot, name, sizeof(name));
            AI_DebugDecision(g_iLeaderBot, "Aggressive leader elected (all humans dead, no defib)");
        } else {
            if (g_bDebugMode) {
                PrintToServer("[AI Bot] Aggressive leader mode ON — but no valid leader found!");
            }
        }
    }
}

/**
 * Find the nearest rescue closet nav area and cache its position.
 * Uses the NAV_SPAWN_RESCUE_CLOSET attribute on nav areas.
 */
static void TaskNavigate_FindRescueCloset() {
    float curTime = GetGameTime();
    if ((curTime - g_fLastClosetCheckTime) < NAV_RESCUE_CLOSET_CHECK_INTERVAL) return;
    g_fLastClosetCheckTime = curTime;

    // Get reference position: leader bot in aggressive mode, or most-forward alive survivor
    float refPos[3];
    bool hasRef = false;

    if (g_bAggressiveLeaderMode && g_iLeaderBot > 0 && IsClientInGame(g_iLeaderBot) && IsPlayerAlive(g_iLeaderBot)) {
        GetClientAbsOrigin(g_iLeaderBot, refPos);
        hasRef = true;
    } else {
        // Find the most-forward alive survivor as reference
        float bestFlow = -1.0;
        for (int i = 1; i <= MaxClients; i++) {
            if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
            if (GetClientTeam(i) != 2) continue;
            float flow = L4D2Direct_GetFlowDistance(i);
            if (flow > bestFlow) {
                bestFlow = flow;
                GetClientAbsOrigin(i, refPos);
                hasRef = true;
            }
        }
    }

    if (!hasRef) {
        g_bHasRescueCloset = false;
        return;
    }

    float bestDist = NAV_RESCUE_CLOSET_SEARCH_RANGE;
    g_bHasRescueCloset = false;

    // Search nav areas with NAV_SPAWN_RESCUE_CLOSET flag
    int ent = -1;
    int closetCount = 0;
    while ((ent = FindEntityByClassname(ent, "info_survivor_rescue")) != -1) {
        if (++closetCount > 32) break;  // Safety: don't iterate forever on broken maps
        if (!IsValidEntity(ent)) { ent++; continue; }

        float closetPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", closetPos);
        float dist = AI_GetVectorDistance(refPos, closetPos);

        if (dist < bestDist) {
            // Check that closet is AHEAD of leader (higher flow), not behind
            // Simple check: closet should be farther from map start than leader
            bestDist = dist;
            g_fRescueClosetPos = closetPos;
            g_bHasRescueCloset = true;
        }
    }

    if (g_bDebugMode && g_bHasRescueCloset) {
        PrintToServer("[AI Bot] Rescue closet found at (%.0f, %.0f, %.0f), dist=%.0f",
            g_fRescueClosetPos[0], g_fRescueClosetPos[1], g_fRescueClosetPos[2], bestDist);
    }
}

/**
 * Query whether aggressive leader mode is active.
 * Used by other tasks (e.g. Rescue) to suppress Tank-avoidance.
 */
bool TaskNavigate_IsAggressiveLeaderMode() {
    return g_bAggressiveLeaderMode;
}

/**
 * Check if navigate task should activate for this bot.
 */
bool TaskNavigate_ShouldUpdate(int client) {
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;

    // Update aggressive leader mode at every check
    TaskNavigate_UpdateAggressiveMode();

    // When bot has defib and a dead teammate exists, navigate toward the death position
    // instead of following the leader. This is critical for the Defib task to work.
    // Also allow navigation when searching for a defib (hasMoveTarget set by Defib Mode B).
    if (g_bDefibEnabled) {
        bool hasDeadTeammate = false;
        for (int i = 1; i <= MaxClients; i++) {
            if (i == client) continue;
            if (!IsClientInGame(i) || GetClientTeam(i) != 2) continue;
            if (!IsPlayerAlive(i)) { hasDeadTeammate = true; break; }
        }
        if (hasDeadTeammate) {
            // Bot has defib → navigate to death position
            int weapon = GetPlayerWeaponSlot(client, 3);
            if (weapon != -1) {
                char cls[64];
                GetEntityClassname(weapon, cls, sizeof(cls));
                if (StrEqual(cls, "weapon_defibrillator")) {
                    return true;
                }
            }
            // Bot doesn't have defib but has moveTarget → navigate to defib search target
            if (g_BotState[client].hasMoveTarget) {
                return true;
            }
        }
    }

    // In aggressive leader mode, ALWAYS navigate — push forward!
    // But if pathfinding is disabled, don't enter aggressive push mode
    if (g_bAggressiveLeaderMode && g_bPathfindingEnabled) {
        if (client == g_iLeaderBot) {
            FormatEx(g_BotState[client].taskSwitchReason, sizeof(BotState::taskSwitchReason),
                "Leader push%s", g_bHasRescueCloset ? " → closet" : "");
        } else {
            FormatEx(g_BotState[client].taskSwitchReason, sizeof(BotState::taskSwitchReason),
                "Following leader");
        }
        return true;
    }

    // When keepalive is disabled, stop navigating if all human players are dead
    // UNLESS a dead teammate exists and defib is enabled (bot may need to reach a defib or body)
    // OR keepalive is enabled (bots should always push forward)
    if (!g_bKeepaliveEnabled) {
        bool hasDeadHuman = false;
        bool hasAliveHuman = false;
        for (int i = 1; i <= MaxClients; i++) {
            if (!IsClientInGame(i) || IsFakeClient(i) || GetClientTeam(i) != 2)
                continue;
            if (IsPlayerAlive(i))
                hasAliveHuman = true;
            else
                hasDeadHuman = true;
        }
        if (!hasAliveHuman && hasDeadHuman && !g_bDefibEnabled && !g_bKeepaliveEnabled)
            return false;
    }

    return true;
}

/**
 * Execute navigation logic: follow leader, spread out, unstick.
 * @return false (never locks — lowest priority, always interruptible).
 */
bool TaskNavigate_Update(int client, int &buttons, float vel[3]) {
    float curTime = GetGameTime();

    // === Step 0: Navigate toward defib-related target (death position or defib pickup) ===
    // This takes priority over following the leader — the bot needs to reach the corpse/defib.
    if (g_bDefibEnabled && g_BotState[client].hasMoveTarget) {
        bool shouldNavigateToTarget = false;
        
        int weapon = GetPlayerWeaponSlot(client, 3);
        if (weapon != -1) {
            char cls[64];
            GetEntityClassname(weapon, cls, sizeof(cls));
            if (StrEqual(cls, "weapon_defibrillator")) {
                // Bot has defib + dead teammate → navigate to death position
                for (int i = 1; i <= MaxClients; i++) {
                    if (i == client) continue;
                    if (!IsClientInGame(i) || GetClientTeam(i) != 2) continue;
                    if (!IsPlayerAlive(i)) { shouldNavigateToTarget = true; break; }
                }
            }
        }
        
        // Also navigate if searching for defib (no defib yet but hasMoveTarget set by Mode B)
        if (!shouldNavigateToTarget) {
            for (int i = 1; i <= MaxClients; i++) {
                if (i == client) continue;
                if (!IsClientInGame(i) || GetClientTeam(i) != 2) continue;
                if (!IsPlayerAlive(i)) { shouldNavigateToTarget = true; break; }
            }
        }
        
        if (shouldNavigateToTarget) {
            float movePos[3];
            movePos = g_BotState[client].moveTarget;
            float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, movePos);
            if (dist > NAV_FOLLOW_CLOSE) {
                AI_SnapViewToPosition(client, movePos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                AI_ComputeMoveToward(client, movePos, g_BotState[client].absOrigin,
                                      g_BotState[client].eyeAngles, vel);
            }
            return false;  // Don't lock
        }
    }

    // === Step 1: Find follow target (leader) ===
    int leader = TaskNavigate_FindLeader(client);
    if (leader == -1) {
        return false;  // No one to follow
    }

    float leaderPos[3];
    GetClientAbsOrigin(leader, leaderPos);
    float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, leaderPos);

    // === Lv6 Cheat Mode: player-centric navigation ===
    if (g_bCheatMode && !IsFakeClient(leader)) {
        // Find nearest alive human player to orbit
        int nearestHuman = -1;
        float nearestHumanDist = 999999.0;
        for (int i = 1; i <= MaxClients; i++) {
            if (i == client) continue;
            if (!IsClientInGame(i) || IsFakeClient(i)) continue;
            if (GetClientTeam(i) != 2 || !IsPlayerAlive(i)) continue;
            float hPos[3];
            GetClientAbsOrigin(i, hPos);
            float hDist = AI_GetVectorDistance(g_BotState[client].absOrigin, hPos);
            if (hDist < nearestHumanDist) {
                nearestHumanDist = hDist;
                nearestHuman = i;
            }
        }

        if (nearestHuman > 0) {
            float humanPos[3];
            GetClientAbsOrigin(nearestHuman, humanPos);
            float humanDist = AI_GetVectorDistance(g_BotState[client].absOrigin, humanPos);

            // Periodic teleport: if too far from player, teleport after interval
            if (humanDist > LV6_TELEPORT_DIST) {
                if (g_fLv6LastTeleport[client] <= 0.0) {
                    g_fLv6LastTeleport[client] = curTime;
                }
                float farDuration = curTime - g_fLv6LastTeleport[client];
                if (farDuration >= g_fLv6TeleportInterval) {
                    // Teleport directly to player with spread offset
                    float teleportPos[3];
                    teleportPos = humanPos;
                    // Position bots in a spread pattern around the player
                    int botIndex = 0;
                    for (int bi = 1; bi < client; bi++) {
                        if (IsClientInGame(bi) && IsFakeClient(bi) && GetClientTeam(bi) == 2 && IsPlayerAlive(bi)) {
                            botIndex++;
                        }
                    }
                    float angle = float(botIndex) * 90.0;  // Spread at 90° intervals
                    teleportPos[0] += Cosine(DegToRad(angle)) * 80.0;
                    teleportPos[1] += Sine(DegToRad(angle)) * 80.0;
                    TeleportEntity(client, teleportPos, NULL_VECTOR, NULL_VECTOR);
                    g_fLv6LastTeleport[client] = 0.0;
                    g_fFarFromLeaderSince[client] = 0.0;
                    g_iNavStuckCount[client] = 0;
                    g_BotState[client].isStuck = false;
                    AI_ResetTeleportState(client);
                    AI_DebugDecision(client, "Lv6 teleport -> player (dist=%.0f, waited=%.1fs)", humanDist, farDuration);
                    return false;
                }
            } else {
                g_fLv6LastTeleport[client] = 0.0;  // Reset timer when close enough
            }

            // Tight follow around the player
            if (humanDist > LV6_FOLLOW_RANGE) {
                AI_ComputeMoveToward(client, humanPos, g_BotState[client].absOrigin,
                                      g_BotState[client].eyeAngles, vel);
            } else if (humanDist > LV6_FOLLOW_RANGE * 0.5) {
                AI_ComputeMoveToward(client, humanPos, g_BotState[client].absOrigin,
                                      g_BotState[client].eyeAngles, vel, NAV_SLOW_SPEED);
            }

            // Spread out from other bots (Lv6 uses wider spread)
            TaskNavigate_SpreadLv6(client, vel);

            // Skip normal navigation logic below
            return false;
        }
        // No human alive — fall through to normal/aggressive logic
    }

    // === Step 1.5: Aggressive leader mode — push forward ===
    if (g_bAggressiveLeaderMode) {
        bool isLeader = (client == g_iLeaderBot);

        if (isLeader) {
            // Leader bot: push forward aggressively
            // Navigate to rescue closet only if a teammate is actually waiting inside
            if (TaskNavigate_AnyTeammateInRescueCloset()) {
                TaskNavigate_FindRescueCloset();
            } else {
                g_bHasRescueCloset = false;
            }

            if (g_bHasRescueCloset) {
                // Navigate toward rescue closet and open it
                float closetDist = AI_GetVectorDistance(g_BotState[client].absOrigin, g_fRescueClosetPos);
                if (closetDist > 80.0) {
                    AI_SnapViewToPosition(client, g_fRescueClosetPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                    AI_ComputeMoveToward(client, g_fRescueClosetPos, g_BotState[client].absOrigin,
                                          g_BotState[client].eyeAngles, vel);
                } else {
                    // Close enough — open the closet door
                    buttons |= IN_USE;
                    AI_SnapViewToPosition(client, g_fRescueClosetPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                }
            } else {
                // No rescue closet target — push forward along the map
                // Use flow distance to find the best forward direction.
                // Sample directions only every 0.5s to avoid expensive trace rays every tick.
                float curTimeSample = GetGameTime();
                if ((curTimeSample - g_fLastFlowSampleTime) >= 0.5) {
                    g_fLastFlowSampleTime = curTimeSample;
                    g_bCachedFlowFound = false;

                    float myFlow = L4D2Direct_GetFlowDistance(client);
                    float bestFlow = myFlow;

                    // Sample 8 directions around the bot
                    for (int dir = 0; dir < 8; dir++) {
                        float angle = float(dir) * 45.0;
                        float dirVec[3];
                        dirVec[0] = Cosine(DegToRad(angle));
                        dirVec[1] = Sine(DegToRad(angle));
                        dirVec[2] = 0.0;

                        float samplePos[3];
                        samplePos[0] = g_BotState[client].absOrigin[0] + dirVec[0] * 300.0;
                        samplePos[1] = g_BotState[client].absOrigin[1] + dirVec[1] * 300.0;
                        samplePos[2] = g_BotState[client].absOrigin[2];

                        // Check this direction isn't blocked by a wall
                        Handle trace = TR_TraceRayFilterEx(g_BotState[client].absOrigin, samplePos,
                                                            MASK_PLAYERSOLID_BRUSHONLY, RayType_EndPoint,
                                                            AI_TraceFilter_NoPlayers, 0);
                        if (TR_DidHit(trace)) {
                            delete trace;
                            continue;  // Blocked by wall, skip this direction
                        }
                        delete trace;

                        // Check flow distance at this point
                        Address navArea = L4D2Direct_GetTerrorNavArea(samplePos);
                        if (navArea != Address_Null) {
                            float sampleFlow = L4D2Direct_GetTerrorNavAreaFlow(navArea);
                            if (sampleFlow > bestFlow) {
                                bestFlow = sampleFlow;
                                g_fCachedFlowTarget = samplePos;
                                g_bCachedFlowFound = true;
                            }
                        }
                    }
                }

                if (g_bCachedFlowFound) {
                    AI_ComputeMoveToward(client, g_fCachedFlowTarget, g_BotState[client].absOrigin,
                                          g_BotState[client].eyeAngles, vel);
                }
            }
        } else {
            // Non-leader bot: follow the leader closely
            // In aggressive mode, use a tighter follow range
            if (dist > NAV_FOLLOW_CLOSE) {
                AI_ComputeMoveToward(client, leaderPos, g_BotState[client].absOrigin,
                                      g_BotState[client].eyeAngles, vel);
            }

            // === Team cohesion: teleport to leader if too far for too long ===
            if (dist > NAV_TEAM_COHESION_RANGE) {
                if (g_fFarFromLeaderSince[client] <= 0.0) {
                    g_fFarFromLeaderSince[client] = curTime;
                }
                float farDuration = curTime - g_fFarFromLeaderSince[client];
                if (farDuration >= NAV_TEAM_COHESION_CHECK_TIME) {
                    // Teleport directly to leader's position with a small offset
                    float teleportPos[3];
                    teleportPos = leaderPos;
                    // Add random offset so bots don't stack on the exact same spot
                    teleportPos[0] += GetRandomFloat(-50.0, 50.0);
                    teleportPos[1] += GetRandomFloat(-50.0, 50.0);
                    TeleportEntity(client, teleportPos, NULL_VECTOR, NULL_VECTOR);
                    g_fFarFromLeaderSince[client] = 0.0;
                    g_iNavStuckCount[client] = 0;
                    g_BotState[client].isStuck = false;
                    AI_ResetTeleportState(client);
                    AI_DebugDecision(client, "Team cohesion teleport -> leader (dist=%.0f, far=%.1fs)", dist, farDuration);
                }
            } else {
                g_fFarFromLeaderSince[client] = 0.0;
            }
        }
    } else {
        // === Step 2: Normal follow logic ===
        if (dist > g_fFollowRange) {
            // Far away — full speed toward leader
            AI_ComputeMoveToward(client, leaderPos, g_BotState[client].absOrigin,
                                  g_BotState[client].eyeAngles, vel);
        } else if (dist > g_fFollowRange * 0.5) {
            // Medium distance — slow follow
            AI_ComputeMoveToward(client, leaderPos, g_BotState[client].absOrigin,
                                  g_BotState[client].eyeAngles, vel, NAV_SLOW_SPEED);
        }
        // else: close enough, don't move toward leader

        // Team cohesion in normal mode too: teleport if extremely far
        if (dist > NAV_TEAM_COHESION_RANGE) {
            if (g_fFarFromLeaderSince[client] <= 0.0) {
                g_fFarFromLeaderSince[client] = curTime;
            }
            float farDuration = curTime - g_fFarFromLeaderSince[client];
            if (farDuration >= NAV_TEAM_COHESION_CHECK_TIME) {
                float teleportPos[3];
                teleportPos = leaderPos;
                teleportPos[0] += GetRandomFloat(-50.0, 50.0);
                teleportPos[1] += GetRandomFloat(-50.0, 50.0);
                TeleportEntity(client, teleportPos, NULL_VECTOR, NULL_VECTOR);
                g_fFarFromLeaderSince[client] = 0.0;
                g_iNavStuckCount[client] = 0;
                g_BotState[client].isStuck = false;
                AI_ResetTeleportState(client);
                AI_DebugDecision(client, "Team cohesion teleport -> leader (dist=%.0f, far=%.1fs)", dist, farDuration);
            }
        } else {
            g_fFarFromLeaderSince[client] = 0.0;
        }
    }

    // === Step 2.5: Navigate toward rescue closet if a teammate is actually waiting inside ===
    // Only navigate when someone is in a rescue closet (m_isWaitingToRescue), not just dead.
    if (TaskNavigate_AnyTeammateInRescueCloset()) {
        TaskNavigate_FindRescueCloset();
        if (g_bHasRescueCloset) {
            float closetDist = AI_GetVectorDistance(g_BotState[client].absOrigin, g_fRescueClosetPos);
            if (closetDist > 80.0) {
                // Move toward the rescue closet
                AI_SnapViewToPosition(client, g_fRescueClosetPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                AI_ComputeMoveToward(client, g_fRescueClosetPos, g_BotState[client].absOrigin,
                                      g_BotState[client].eyeAngles, vel);
            } else {
                // Close enough — press USE to open the rescue closet door
                buttons |= IN_USE;
                AI_SnapViewToPosition(client, g_fRescueClosetPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
            }
        }
    }

    // === Step 3: Spread out if bunched with other bots ===
    TaskNavigate_Spread(client, vel);

    // === Step 4: Stuck detection and escape ===
    if (g_bUnstickEnabled && (curTime - g_fNavLastCheckTime[client]) >= NAV_STUCK_CHECK_TIME) {
        bool isStuck = AI_IsPlayerStuck(client, g_BotState[client].absOrigin,
                                         g_fNavLastPos[client], NAV_STUCK_THRESHOLD);

        if (isStuck && dist > g_fFollowRange * 0.5) {
            // Should be moving but isn't
            g_iNavStuckCount[client]++;
            g_BotState[client].isStuck = true;
            g_BotState[client].stuckTime = curTime;

            // Escalating unstick attempts
            if (g_iNavStuckCount[client] <= 2) {
                // Try jumping
                AI_PressJumpButton(buttons);
            } else if (g_iNavStuckCount[client] <= 4) {
                // Try crouching + jumping
                AI_PressCrouchButton(buttons);
                AI_PressJumpButton(buttons);
            } else if (g_iNavStuckCount[client] <= 6) {
                // Random strafe to escape
                bool strafeRight = ((GetGameTickCount() + client) % 2 == 0);
                AI_ComputeStrafe(g_BotState[client].eyeAngles, vel, strafeRight);
                AI_PressJumpButton(buttons);
            } else {
                // All escape maneuvers failed — try teleport near leader
                float stuckElapsed = curTime - g_BotState[client].stuckTime;
                if (dist > g_fTeleportRange && stuckElapsed > g_fSaveTeleportTime) {
                    if (AI_TryStuckTeleport(client, leaderPos, dist, stuckElapsed, g_fTeleportRange, g_fSaveTeleportTime)) {
                        if (g_bDebugMode) {
                            PrintToServer("[AI Bot] Navigate teleport: bot %d -> leader %d (dist=%.0f, attempt=%d)",
                                client, leader, dist, g_BotState[client].teleportAttempts);
                        }
                        // Reset stuck counter after teleport
                        g_iNavStuckCount[client] = 0;
                        g_BotState[client].isStuck = false;
                    } else if (AI_ShouldAbandonNavigation(client)) {
                        // Teleport failed too many times — reset and keep trying normal movement
                        AI_ResetTeleportState(client);
                        g_iNavStuckCount[client] = 0;
                        g_BotState[client].isStuck = false;
                    }
                }
                // Reset counter after max escape attempts
                if (g_iNavStuckCount[client] > 12) {
                    g_iNavStuckCount[client] = 0;
                }
            }
        } else {
            // Not stuck
            g_iNavStuckCount[client] = 0;
            g_BotState[client].isStuck = false;
            AI_ResetTeleportState(client);
        }

        // Store position for next check
        g_fNavLastPos[client][0] = g_BotState[client].absOrigin[0];
        g_fNavLastPos[client][1] = g_BotState[client].absOrigin[1];
        g_fNavLastPos[client][2] = g_BotState[client].absOrigin[2];
        g_fNavLastCheckTime[client] = curTime;
    }

    return false;  // Never lock
}

/**
 * Reset navigation state.
 */
void TaskNavigate_Reset(int client) {
    g_BotState[client].isStuck = false;
    g_BotState[client].stuckTime = 0.0;
    g_iNavStuckCount[client] = 0;
    g_fNavLastCheckTime[client] = 0.0;
    g_fFarFromLeaderSince[client] = 0.0;
    g_fLv6LastTeleport[client] = 0.0;
}

/**
 * Reset aggressive leader mode globals. Called on map start.
 */
void TaskNavigate_ResetGlobals() {
    g_bAggressiveLeaderMode = false;
    g_iLeaderBot = -1;
    g_bHasRescueCloset = false;
    g_fLastClosetCheckTime = 0.0;
    g_fLastDefibNearbyCheckTime = 0.0;
    g_bDefibNearby = false;
    for (int i = 0; i <= MaxClients; i++) {
        g_fFarFromLeaderSince[i] = 0.0;
        g_fLv6LastTeleport[i] = 0.0;
    }
}

// ============================================================
// Internal helpers
// ============================================================

/**
 * Find the best leader for this bot to follow.
 * In aggressive leader mode, the elected leader is always the leader.
 * Otherwise, prefers human players; falls back to the most-forward bot.
 */
static int TaskNavigate_FindLeader(int client) {
    // In aggressive leader mode, use the elected leader
    if (g_bAggressiveLeaderMode && g_iLeaderBot > 0) {
        if (IsClientInGame(g_iLeaderBot) && IsPlayerAlive(g_iLeaderBot) &&
            GetClientTeam(g_iLeaderBot) == 2 &&
            GetEntProp(g_iLeaderBot, Prop_Send, "m_isIncapacitated") == 0) {
            // If this bot IS the leader, return self (leader doesn't follow anyone)
            if (client == g_iLeaderBot) return -1;
            return g_iLeaderBot;
        }
        // Leader is dead/incapped — re-elect a new leader immediately
        g_iLeaderBot = -1;
        float bestFlow = -1.0;
        for (int i = 1; i <= MaxClients; i++) {
            if (!IsClientInGame(i) || !IsFakeClient(i)) continue;
            if (GetClientTeam(i) != 2 || !IsPlayerAlive(i)) continue;
            if (GetEntProp(i, Prop_Send, "m_isIncapacitated") != 0) continue;
            float flow = L4D2Direct_GetFlowDistance(i);
            if (flow > bestFlow) {
                bestFlow = flow;
                g_iLeaderBot = i;
            }
        }
        if (g_iLeaderBot > 0) {
            AI_DebugDecision(g_iLeaderBot, "New leader elected (previous leader died)");
            if (client == g_iLeaderBot) return -1;
            return g_iLeaderBot;
        }
        // No valid leader found — fall through to normal logic
        g_bAggressiveLeaderMode = false;
    }

    int bestHuman = -1;
    float bestHumanDist = 999999.0;

    int bestBot = -1;
    float bestBotFlow = -1.0;

    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 2) continue;

        // Skip incapacitated players — they cannot lead
        if (GetEntProp(i, Prop_Send, "m_isIncapacitated") != 0) continue;

        // Skip players pinned by special infected (smoker, charger, jockey, hunter)
        if (GetEntProp(i, Prop_Send, "m_tongueOwner") > 0) continue;
        if (GetEntProp(i, Prop_Send, "m_pummelAttacker") > 0) continue;
        if (GetEntProp(i, Prop_Send, "m_jockeyAttacker") > 0) continue;
        if (GetEntProp(i, Prop_Send, "m_pounceAttacker") > 0) continue;

        if (!IsFakeClient(i)) {
            // Human player — prefer closest
            float pos[3];
            GetClientAbsOrigin(i, pos);
            float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, pos);
            if (dist < bestHumanDist) {
                bestHumanDist = dist;
                bestHuman = i;
            }
        } else {
            // Bot — pick the one furthest along the map (highest flow distance)
            // This ensures the most forward bot leads the group
            float flow = L4D2Direct_GetFlowDistance(i);
            if (flow > bestBotFlow) {
                bestBotFlow = flow;
                bestBot = i;
            }
        }
    }

    // Prefer human players; when no humans alive, pick the most forward bot as leader
    return (bestHuman != -1) ? bestHuman : bestBot;
}

/**
 * Push bot away from nearby bots to avoid bunching.
 */
static void TaskNavigate_Spread(int client, float vel[3]) {
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (!IsFakeClient(i)) continue;  // Only spread from other bots

        float otherPos[3];
        GetClientAbsOrigin(i, otherPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, otherPos);

        if (dist < NAV_SPREAD_DIST && dist > 1.0) {
            // Push away from this bot
            float pushDir[3];
            pushDir[0] = g_BotState[client].absOrigin[0] - otherPos[0];
            pushDir[1] = g_BotState[client].absOrigin[1] - otherPos[1];
            pushDir[2] = 0.0;
            NormalizeVector(pushDir, pushDir);

            // Add a small velocity component in the push direction
            float fwd[3], rgt[3];
            float angles[3];
            angles[0] = 0.0;
            angles[1] = g_BotState[client].eyeAngles[1];
            angles[2] = 0.0;
            GetAngleVectors(angles, fwd, rgt, NULL_VECTOR);

            float pushForward = (pushDir[0] * fwd[0] + pushDir[1] * fwd[1]) * 100.0;
            float pushRight = (pushDir[0] * rgt[0] + pushDir[1] * rgt[1]) * 100.0;

            vel[0] += pushForward;
            vel[1] += pushRight;
            break;  // Only spread from one nearby bot per frame
        }
    }
}

/**
 * Lv6 Cheat: wider spread to avoid blocking the rushing player.
 */
static void TaskNavigate_SpreadLv6(int client, float vel[3]) {
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (!IsFakeClient(i)) continue;

        float otherPos[3];
        GetClientAbsOrigin(i, otherPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, otherPos);

        if (dist < LV6_SPREAD_DIST && dist > 1.0) {
            float pushDir[3];
            pushDir[0] = g_BotState[client].absOrigin[0] - otherPos[0];
            pushDir[1] = g_BotState[client].absOrigin[1] - otherPos[1];
            pushDir[2] = 0.0;
            NormalizeVector(pushDir, pushDir);

            float fwd[3], rgt[3];
            float angles[3];
            angles[0] = 0.0;
            angles[1] = g_BotState[client].eyeAngles[1];
            angles[2] = 0.0;
            GetAngleVectors(angles, fwd, rgt, NULL_VECTOR);

            float pushForward = (pushDir[0] * fwd[0] + pushDir[1] * fwd[1]) * 150.0;
            float pushRight = (pushDir[0] * rgt[0] + pushDir[1] * rgt[1]) * 150.0;

            vel[0] += pushForward;
            vel[1] += pushRight;
            break;
        }
    }
}
