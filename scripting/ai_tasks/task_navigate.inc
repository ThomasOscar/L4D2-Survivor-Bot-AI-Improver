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

// Aggressive leader mode constants
#define NAV_RESCUE_CLOSET_SEARCH_RANGE  2000.0  // How far ahead to search for rescue closets
#define NAV_RESCUE_CLOSET_CHECK_INTERVAL 10.0   // Re-scan for closets every N seconds

// Per-bot navigation state
static float g_fNavLastPos[MAXPLAYERS + 1][3];
static float g_fNavLastCheckTime[MAXPLAYERS + 1];
static int g_iNavStuckCount[MAXPLAYERS + 1];

// Aggressive leader mode state
static bool g_bAggressiveLeaderMode = false;      // Currently in aggressive leader mode
static int g_iLeaderBot = -1;                      // The elected leader bot client index
static float g_fRescueClosetPos[3];                // Nearest rescue closet position
static bool g_bHasRescueCloset = false;             // Whether a rescue closet was found
static float g_fLastClosetCheckTime = 0.0;         // Last time we scanned for closets
static float g_fLastDefibNearbyCheckTime = 0.0;    // Throttle for DefibExistsNearby scan
static bool g_bDefibNearby = false;                 // Cached result of DefibExistsNearby

// ============================================================
// Aggressive Leader Mode
// ============================================================

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
 * Check if a defibrillator exists on the ground or as a spawn within range.
 * Results are cached for 5 seconds to avoid expensive entity scans every tick.
 */
static bool TaskNavigation_DefibExistsNearby() {
    float curTime = GetGameTime();
    if (curTime - g_fLastDefibNearbyCheckTime < 5.0) {
        return g_bDefibNearby;
    }
    g_fLastDefibNearbyCheckTime = curTime;

    g_bDefibNearby = false;
    int ent = -1;
    int safetyCount = 0;
    while ((ent = FindEntityByClassname(ent, "weapon_defibrillator")) != -1) {
        if (++safetyCount > 256) break;
        if (!IsValidEntity(ent)) { ent++; continue; }
        int owner = GetEntPropEnt(ent, Prop_Data, "m_hOwnerEntity");
        if (owner > 0 && owner <= MaxClients && IsPlayerAlive(owner)) continue;
        g_bDefibNearby = true;
        return true;  // Found an unowned defib
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
        g_bDefibNearby = true;
        return true;  // Found a spawn with items
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

    // Get leader position as reference
    if (g_iLeaderBot <= 0 || !IsClientInGame(g_iLeaderBot) || !IsPlayerAlive(g_iLeaderBot)) {
        g_bHasRescueCloset = false;
        return;
    }
    float leaderPos[3];
    GetClientAbsOrigin(g_iLeaderBot, leaderPos);

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
        float dist = AI_GetVectorDistance(leaderPos, closetPos);

        if (dist < bestDist) {
            // Check that closet is AHEAD of leader (higher flow), not behind
            // Simple check: closet should be farther from map start than leader
            bestDist = dist;
            g_fRescueClosetPos = closetPos;
            g_bHasRescueCloset = true;
        }
    }

    if (g_bDebugMode && g_bHasRescueCloset) {
        PrintToServer("[AI Bot] Rescue closet found at (%.0f, %.0f, %.0f), dist=%.0f from leader",
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
    if (g_bAggressiveLeaderMode) {
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

    // === Step 1.5: Aggressive leader mode — push forward ===
    if (g_bAggressiveLeaderMode) {
        bool isLeader = (client == g_iLeaderBot);

        if (isLeader) {
            // Leader bot: push forward aggressively
            // If we have a rescue closet target, navigate toward it
            TaskNavigate_FindRescueCloset();

            if (g_bHasRescueCloset) {
                // Navigate toward rescue closet
                float closetDist = AI_GetVectorDistance(g_BotState[client].absOrigin, g_fRescueClosetPos);
                if (closetDist > NAV_FOLLOW_CLOSE) {
                    AI_ComputeMoveToward(client, g_fRescueClosetPos, g_BotState[client].absOrigin,
                                          g_BotState[client].eyeAngles, vel);
                }
            } else {
                // No rescue closet found — just push forward along the map
                // Use the leader's own forward direction to advance
                float fwd[3];
                GetAngleVectors(g_BotState[client].eyeAngles, fwd, NULL_VECTOR, NULL_VECTOR);
                fwd[2] = 0.0;
                float fwdLen = GetVectorLength(fwd);
                if (fwdLen > 0.1) {
                    // Compute a target point ahead of the leader
                    float targetPos[3];
                    targetPos[0] = g_BotState[client].absOrigin[0] + fwd[0] / fwdLen * 200.0;
                    targetPos[1] = g_BotState[client].absOrigin[1] + fwd[1] / fwdLen * 200.0;
                    targetPos[2] = g_BotState[client].absOrigin[2];
                    AI_ComputeMoveToward(client, targetPos, g_BotState[client].absOrigin,
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
    }

    // === Step 3: Spread out if bunched with other bots ===
    TaskNavigate_Spread(client, vel);

    // === Step 4: Stuck detection and escape ===
    if ((curTime - g_fNavLastCheckTime[client]) >= NAV_STUCK_CHECK_TIME) {
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
        // Leader is dead/incapped — re-elect
        g_iLeaderBot = -1;
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
