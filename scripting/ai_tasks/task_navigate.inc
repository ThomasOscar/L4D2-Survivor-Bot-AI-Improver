/**
 * AI Task: Navigate
 * 
 * Priority Order: 3
 * Handles following the team leader, maintaining formation,
 * avoiding bunching, and detecting/resolving stuck situations.
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

// Per-bot navigation state
static float g_fNavLastPos[MAXPLAYERS + 1][3];
static float g_fNavLastCheckTime[MAXPLAYERS + 1];
static int g_iNavStuckCount[MAXPLAYERS + 1];

/**
 * Check if navigate task should activate for this bot.
 */
bool TaskNavigate_ShouldUpdate(int client) {
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;

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

    // When keepalive is disabled, stop navigating if all human players are dead
    // UNLESS a dead teammate exists and defib is enabled (bot may need to reach a defib or body)
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
        if (!hasAliveHuman && hasDeadHuman && !g_bDefibEnabled)
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

    // === Step 2: Follow logic ===
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
            } else {
                // Random strafe to escape
                bool strafeRight = ((GetGameTickCount() + client) % 2 == 0);
                AI_ComputeStrafe(g_BotState[client].eyeAngles, vel, strafeRight);
                AI_PressJumpButton(buttons);
                // Reset counter after max attempts
                if (g_iNavStuckCount[client] > 8) {
                    g_iNavStuckCount[client] = 0;
                }
            }
        } else {
            // Not stuck
            g_iNavStuckCount[client] = 0;
            g_BotState[client].isStuck = false;
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

// ============================================================
// Internal helpers
// ============================================================

/**
 * Find the best leader for this bot to follow.
 * Prefers human players; falls back to lowest-index alive bot.
 */
static int TaskNavigate_FindLeader(int client) {
    int bestHuman = -1;
    float bestHumanDist = 999999.0;

    int bestBot = -1;

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
            // Bot — take first valid (lowest index)
            if (bestBot == -1) {
                bestBot = i;
            }
        }
    }

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
