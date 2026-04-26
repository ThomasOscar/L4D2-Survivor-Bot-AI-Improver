/**
 * AI Task: Defibrillator Use
 * 
 * Priority Order: 0 (same as Rescue — saving lives)
 * Tick Interval: 0.0s (every frame)
 *
 * Two modes:
 *   Mode A: Bot has defib -> go revive dead teammate
 *   Mode B: Dead teammate exists but bot has no defib -> search for defib on ground,
 *           swap medkit if needed, then revive.
 *
 * Key design decisions:
 *   - Mode B search does NOT lock (returns false), allowing Combat to run when enemies near
 *   - Unreachable defibs (underground/below floor) are skipped via height check
 *   - Search has a timeout (DEFIB_SEARCH_TIMEOUT) to avoid infinite loops
 *   - Mode A far-range also doesn't lock, letting Navigate handle nav-mesh pathfinding
 *   - Mode A within-range DOES lock to ensure defib use completes
 */

#if defined _task_defib_included
    #endinput
#endif
#define _task_defib_included

// Defib constants
#define DEFIB_USE_RANGE         100.0   // Distance within which to use defibrillator
#define DEFIB_SEARCH_RANGE      800.0   // Range to search for defibs on ground
#define DEFIB_PICKUP_RANGE      80.0    // Range to pick up defib (direct give, not USE-based)
#define DEFIB_SEARCH_TIMEOUT    15.0    // Give up searching after this many seconds
#define DEFIB_HEIGHT_THRESHOLD  200.0   // Max height difference for reachable defib (underground check)
#define DEFIB_STUCK_TELEPORT_TIME 8.0   // Seconds before teleporting if stuck moving to target
#define DEFIB_STUCK_TELEPORT_DIST 300.0 // Min distance to target before teleport is considered

// Per-bot defib search state
static int g_iDefibSearchTarget[MAXPLAYERS + 1];      // Ground defib entity to pick up
static bool g_bDefibSearchMode[MAXPLAYERS + 1];        // true = searching for defib on ground
static float g_fDefibWeaponSwitchTime[MAXPLAYERS + 1]; // Time when weapon switch started
static float g_fDefibSearchStartTime[MAXPLAYERS + 1];  // Time when search mode started
static bool g_bDefibNeedDropMedkit[MAXPLAYERS + 1];    // true = need to drop medkit before picking up defib
static bool g_bDefibSearchIsSpawn[MAXPLAYERS + 1];     // true = search target is a weapon_spawn entity
static float g_fDefibReviveStartTime[MAXPLAYERS + 1];  // Time when Mode A (navigate to corpse) started

/**
 * Check if bot is in search mode (Mode B - looking for defib on ground).
 * Used by the main plugin to avoid clearing IN_USE during pickup.
 */
bool TaskDefib_IsSearchMode(int client) {
    return g_bDefibSearchMode[client];
}

/**
 * Check if any survivor teammate is dead.
 */
static bool TaskDefib_HasDeadTeammate(int client) {
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (!IsPlayerAlive(i)) return true;
    }
    return false;
}

/**
 * Check if a ground defib entity is reachable (not underground/below floor).
 * Defibs that spawn under the map or inside geometry will have a position
 * far below the nearest nav area or the bot's current Z position.
 */
static bool TaskDefib_IsReachable(int client, int defibEnt) {
    float defibPos[3];
    GetEntPropVector(defibEnt, Prop_Data, "m_vecAbsOrigin", defibPos);
    float botPos[3];
    botPos = g_BotState[client].absOrigin;

    // Height difference check: if defib is far below the bot, it's probably underground
    float heightDiff = botPos[2] - defibPos[2];
    if (heightDiff > DEFIB_HEIGHT_THRESHOLD) {
        // Defib is way below the bot — likely underground
        return false;
    }

    // Also check if defib position is absurdly low (below -4000 in Z = underground)
    if (defibPos[2] < -4000.0) {
        return false;
    }

    return true;
}

/**
 * Check if the defibrillator task should activate for this bot.
 * Only the BEST candidate bot should go rescue:
 *   - If any bot already has a defib → only the closest one with defib goes (Mode A)
 *   - If no bot has a defib → only the closest bot to a reachable defib goes (Mode B)
 *   - All other bots should continue fighting/navigating
 */
bool TaskDefib_ShouldUpdate(int client) {
    if (!g_bDefibEnabled) return false;

    // Bot must not be incapacitated or controlled
    if (g_BotState[client].isIncapacitated) return false;
    if (g_BotState[client].controlledByType > 0) return false;

    // Must have a dead teammate
    if (!TaskDefib_HasDeadTeammate(client)) return false;

    // Store first dead teammate as target
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (IsPlayerAlive(i)) continue;
        g_BotState[client].defibTarget = i;
        break;
    }

    // Check if this bot has a defib
    bool hasDefib = false;
    int weapon = GetPlayerWeaponSlot(client, 3);
    if (weapon != -1) {
        char classname[64];
        GetEntityClassname(weapon, classname, sizeof(classname));
        if (StrEqual(classname, "weapon_defibrillator")) {
            hasDefib = true;
        }
    }

    // Determine the BEST candidate bot for the defib task
    // Priority 1: Bot that already has a defib (closest to corpse wins)
    // Priority 2: Bot without defib but closest to a reachable ground/spawn defib
    int bestBot = -1;
    float bestDist = 999999.0;
    bool bestBotHasDefib = false;

    for (int b = 1; b <= MaxClients; b++) {
        if (!IsClientInGame(b)) continue;
        if (GetClientTeam(b) != 2) continue;
        if (!IsPlayerAlive(b)) continue;
        if (g_BotState[b].isIncapacitated) continue;
        if (g_BotState[b].controlledByType > 0) continue;

        int bWeapon = GetPlayerWeaponSlot(b, 3);
        bool bHasDefib = false;
        if (bWeapon != -1) {
            char bCls[64];
            GetEntityClassname(bWeapon, bCls, sizeof(bCls));
            if (StrEqual(bCls, "weapon_defibrillator")) {
                bHasDefib = true;
            }
        }

        // Get distance to death position
        float bDist = 999999.0;
        int deadTarget = -1;
        for (int d = 1; d <= MaxClients; d++) {
            if (d == b) continue;
            if (!IsClientInGame(d) || GetClientTeam(d) != 2) continue;
            if (IsPlayerAlive(d)) continue;
            deadTarget = d;
            float deathPos[3];
            if (g_bHasDeathPosition[d]) {
                deathPos = g_fDeathPosition[d];
            } else {
                GetClientAbsOrigin(d, deathPos);
            }
            float d2 = AI_GetVectorDistance(g_BotState[b].absOrigin, deathPos);
            if (d2 < bDist) bDist = d2;
        }

        if (bHasDefib) {
            // Bot with defib has highest priority
            if (!bestBotHasDefib || bDist < bestDist) {
                bestBot = b;
                bestDist = bDist;
                bestBotHasDefib = true;
            }
        } else if (!bestBotHasDefib) {
            // No bot has defib yet — check who's closest to a reachable defib
            int nearDefib = TaskDefib_FindNearest(b);
            if (nearDefib != -1) {
                float defibPos[3];
                GetEntPropVector(nearDefib, Prop_Data, "m_vecAbsOrigin", defibPos);
                float d2 = AI_GetVectorDistance(g_BotState[b].absOrigin, defibPos);
                if (d2 < bestDist) {
                    bestBot = b;
                    bestDist = d2;
                    bestBotHasDefib = false;
                }
            }
        }
    }

    // Only the best candidate should do the defib task
    if (bestBot != client) {
        // Not the best candidate — skip defib task
        // Also reset our search state if we had one
        if (g_iDefibSearchTarget[client] > 0 || g_bDefibSearchMode[client]) {
            TaskDefib_Reset(client);
        }
        return false;
    }

    // This bot IS the best candidate
    if (hasDefib) {
        // Mode A: Bot already has a defibrillator
        g_bDefibSearchMode[client] = false;
        if (g_bDebugMode) {
            char tName[64]; GetClientName(g_BotState[client].defibTarget, tName, sizeof(tName));
            FormatEx(g_BotState[client].taskSwitchReason, sizeof(BotState::taskSwitchReason),
                "Defib revive %s", tName);
        }
        return true;
    }

    // Mode B: Search for a defib on the ground
    // Check if we already have a VALID search target
    if (g_iDefibSearchTarget[client] > 0 && IsValidEntity(g_iDefibSearchTarget[client])) {
        // Check if search has timed out
        if (g_fDefibSearchStartTime[client] > 0.0 &&
            (GetGameTime() - g_fDefibSearchStartTime[client]) > DEFIB_SEARCH_TIMEOUT) {
            if (g_bDebugMode) {
                char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                AI_DebugDecision(client, "Defib search timed out, giving up");
            }
            TaskDefib_Reset(client);
            return false;
        }
        // Check if target is still reachable
        if (!TaskDefib_IsReachable(client, g_iDefibSearchTarget[client])) {
            if (g_bDebugMode) {
                char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                AI_DebugDecision(client, "Defib target unreachable, finding another");
            }
            g_iDefibSearchTarget[client] = -1;
            // Fall through to find a new one
        } else {
            g_bDefibSearchMode[client] = true;
            if (g_bDebugMode) {
                FormatEx(g_BotState[client].taskSwitchReason, sizeof(BotState::taskSwitchReason),
                    "Defib search");
            }
            return true;
        }
    }

    // Find nearest REACHABLE defib on ground or spawn
    int defibEnt = TaskDefib_FindNearest(client);
    if (defibEnt != -1) {
        g_iDefibSearchTarget[client] = defibEnt;
        g_bDefibSearchMode[client] = true;
        g_fDefibSearchStartTime[client] = GetGameTime();
        return true;
    }

    // Dead teammate but no reachable defib available - can't help
    return false;
}

/**
 * Execute defibrillator task: search for defib, pick it up, then revive.
 * @return true to lock this task (prevent lower-priority tasks).
 */
bool TaskDefib_Update(int client, int &buttons, float vel[3]) {
    int target = g_BotState[client].defibTarget;

    // Debug: trace Update execution (every 3 seconds)
    if (g_bDebugMode) {
        static float g_fLastUpdateTrace[MAXPLAYERS + 1];
        float now = GetGameTime();
        if (now - g_fLastUpdateTrace[client] >= 3.0) {
            g_fLastUpdateTrace[client] = now;
            // Removed verbose per-3s trace — task switches show enough info
        }
    }

    // Validate target is still dead and in-game
    if (target < 1 || target > MaxClients || !IsClientInGame(target) || IsPlayerAlive(target)) {
        TaskDefib_Reset(client);
        return false;
    }

    // === Mode B: Search for and pick up a defib on the ground ===
    if (g_bDefibSearchMode[client]) {
        // Check search timeout
        if (g_fDefibSearchStartTime[client] > 0.0 &&
            (GetGameTime() - g_fDefibSearchStartTime[client]) > DEFIB_SEARCH_TIMEOUT) {
            if (g_bDebugMode) {
                char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                AI_DebugDecision(client, "Defib search timed out, giving up");
            }
            TaskDefib_Reset(client);
            return false;
        }

        int searchTarget = g_iDefibSearchTarget[client];
        if (searchTarget <= 0 || !IsValidEntity(searchTarget)) {
            // Lost our target - try to find another
            int newTarget = TaskDefib_FindNearest(client);
            if (newTarget == -1) {
                if (g_bDebugMode) {
                    char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                    AI_DebugDecision(client, "ModeB: lost target, no replacement");
                }
                TaskDefib_Reset(client);
                return false;
            }
            g_iDefibSearchTarget[client] = newTarget;
            searchTarget = newTarget;
            g_fDefibSearchStartTime[client] = GetGameTime();  // Reset timeout
        }

        // Check if target is still reachable
        if (!TaskDefib_IsReachable(client, searchTarget)) {
            if (g_bDebugMode) {
                char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                AI_DebugDecision(client, "ModeB: target unreachable, finding another");
            }
            g_iDefibSearchTarget[client] = -1;
            // Try to find another reachable defib
            int newTarget = TaskDefib_FindNearest(client);
            if (newTarget == -1) {
                TaskDefib_Reset(client);
                return false;
            }
            g_iDefibSearchTarget[client] = newTarget;
            searchTarget = newTarget;
            g_fDefibSearchStartTime[client] = GetGameTime();
        }

        float defibPos[3];
        GetEntPropVector(searchTarget, Prop_Data, "m_vecAbsOrigin", defibPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, defibPos);

        if (dist <= DEFIB_PICKUP_RANGE) {
            // Close enough - directly give the bot a defibrillator via code
            // Bypasses unreliable USE/drop/pickup mechanics

            if (g_bDefibSearchIsSpawn[client]) {
                // Target is a weapon_spawn on a table/shelf
                // Decrement the spawn count and give defib directly
                int count = GetEntProp(searchTarget, Prop_Data, "m_nCount");
                if (count > 0) {
                    SetEntProp(searchTarget, Prop_Data, "m_nCount", count - 1);
                }
                // Hide spawn if empty
                if (count <= 1) {
                    SetEntityRenderMode(searchTarget, RENDER_NONE);
                }
            } else {
                // Target is a dropped weapon_defibrillator on the ground
                // Remove it since we'll give a new one via GivePlayerItem
                AcceptEntityInput(searchTarget, "Kill");
            }

            // Remove medkit from slot3 if present
            int slot3Wep = GetPlayerWeaponSlot(client, 3);
            if (slot3Wep != -1) {
                char slot3Cls[64];
                GetEntityClassname(slot3Wep, slot3Cls, sizeof(slot3Cls));
                if (!StrEqual(slot3Cls, "weapon_defibrillator")) {
                    RemovePlayerItem(client, slot3Wep);
                    AcceptEntityInput(slot3Wep, "Kill");

                    if (g_bDebugMode) {
                        char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                        PrintToServer("[AI Bot DEBUG] %s ModeB: removed medkit from slot3", dbgN);
                    }
                }
            }

            // Give bot a defibrillator directly
            int newDefib = GivePlayerItem(client, "weapon_defibrillator");
            if (newDefib != -1) {
                // Successfully gave defib - switch to revive mode
                g_bDefibSearchMode[client] = false;
                g_iDefibSearchTarget[client] = -1;
                g_fDefibWeaponSwitchTime[client] = 0.0;
                g_bDefibNeedDropMedkit[client] = false;
                g_bDefibSearchIsSpawn[client] = false;
                g_BotState[client].hasMoveTarget = false;

                if (g_bDebugMode) {
                    char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                    AI_DebugDecision(client, "ModeB: gave defib, switching to revive");
                }
            } else {
                if (g_bDebugMode) {
                    char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                    AI_DebugDecision(client, "ModeB: GivePlayerItem failed!");
                }
                TaskDefib_Reset(client);
                return false;
            }

            return true;  // Lock while transitioning
        }

        // Moving toward the ground defib - LOCK to prevent other tasks overriding movement
        // Check for stuck: if far from target and elapsed time exceeds threshold, teleport
        float elapsed = GetGameTime() - g_fDefibSearchStartTime[client];

        // Abandon if teleport failed too many times
        if (AI_ShouldAbandonNavigation(client)) {
            if (g_bDebugMode) {
                char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                AI_DebugDecision(client, "ModeB: abandon (teleport failed %d times)", g_BotState[client].teleportAttempts);
            }
            TaskDefib_Reset(client);
            return false;
        }

        if (AI_TryStuckTeleport(client, defibPos, dist, elapsed, DEFIB_STUCK_TELEPORT_DIST, DEFIB_STUCK_TELEPORT_TIME)) {
            if (g_bDebugMode) {
                char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                PrintToServer("[AI Bot DEBUG] %s ModeB: teleported near defib (was stuck %.0fs, dist=%.0f)", dbgN, elapsed, dist);
            }
            return true;
        }

        g_BotState[client].moveTarget[0] = defibPos[0];
        g_BotState[client].moveTarget[1] = defibPos[1];
        g_BotState[client].moveTarget[2] = defibPos[2];
        g_BotState[client].hasMoveTarget = true;

        AI_SnapViewToPosition(client, defibPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
        AI_ComputeMoveToward(client, defibPos, g_BotState[client].absOrigin,
                              g_BotState[client].eyeAngles, vel);

        if (g_bDebugMode) {
            static float g_fLastDefibMoveLog[MAXPLAYERS + 1];
            float now = GetGameTime();
            if (now - g_fLastDefibMoveLog[client] >= 2.0) {
                g_fLastDefibMoveLog[client] = now;
                char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                float elapsed = now - g_fDefibSearchStartTime[client];
                PrintToServer("[AI Bot DEBUG] %s ModeB: moving to defib (dist=%.0f, elapsed=%.0fs, ent=%d)", dbgN, dist, elapsed, searchTarget);
            }
        }
        return true;  // LOCK while moving - prevents Heal/Combat from overriding
    }

    // === Mode A: Bot has defib - go revive dead teammate ===

    // Verify bot still has defib
    int weapon = GetPlayerWeaponSlot(client, 3);
    if (weapon == -1) {
        g_bDefibSearchMode[client] = true;
        g_iDefibSearchTarget[client] = -1;
        g_fDefibSearchStartTime[client] = GetGameTime();
        return false;  // Don't lock - let other tasks run while searching
    }
    char cls[64];
    GetEntityClassname(weapon, cls, sizeof(cls));
    if (!StrEqual(cls, "weapon_defibrillator")) {
        g_bDefibSearchMode[client] = true;
        g_iDefibSearchTarget[client] = -1;
        g_fDefibSearchStartTime[client] = GetGameTime();
        return false;  // Don't lock
    }

    // Get death position
    float defibPos[3];
    if (g_bHasDeathPosition[target]) {
        defibPos = g_fDeathPosition[target];
    } else {
        GetClientAbsOrigin(target, defibPos);
    }

    float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, defibPos);

    if (dist > DEFIB_USE_RANGE) {
        // FAR FROM CORPSE - Don't lock. Let Navigate handle nav-mesh pathfinding.
        g_fDefibWeaponSwitchTime[client] = 0.0;

        // Track when Mode A navigation started
        if (g_fDefibReviveStartTime[client] <= 0.0) {
            g_fDefibReviveStartTime[client] = GetGameTime();
            AI_ResetTeleportState(client);
        }

        // Check for stuck: if far from corpse and elapsed time exceeds threshold, teleport
        float reviveElapsed = GetGameTime() - g_fDefibReviveStartTime[client];

        // Abandon if teleport failed too many times
        if (AI_ShouldAbandonNavigation(client)) {
            if (g_bDebugMode) {
                char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                AI_DebugDecision(client, "ModeA: abandon (teleport failed %d times)", g_BotState[client].teleportAttempts);
            }
            TaskDefib_Reset(client);
            return false;
        }

        if (AI_TryStuckTeleport(client, defibPos, dist, reviveElapsed, DEFIB_STUCK_TELEPORT_DIST, DEFIB_STUCK_TELEPORT_TIME)) {
            if (g_bDebugMode) {
                char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                PrintToServer("[AI Bot DEBUG] %s ModeA: teleported near corpse (was stuck %.0fs, dist=%.0f)", dbgN, reviveElapsed, dist);
            }
            return false;
        }

        g_BotState[client].moveTarget[0] = defibPos[0];
        g_BotState[client].moveTarget[1] = defibPos[1];
        g_BotState[client].moveTarget[2] = defibPos[2];
        g_BotState[client].hasMoveTarget = true;

        AI_SnapViewToPosition(client, defibPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
        AI_ComputeMoveToward(client, defibPos, g_BotState[client].absOrigin,
                              g_BotState[client].eyeAngles, vel);

        if (g_bDebugMode) {
            char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
            char tgtN[64];
            if (IsClientInGame(target)) GetClientName(target, tgtN, sizeof(tgtN));
            else strcopy(tgtN, sizeof(tgtN), "???");
            PrintToChatAll("\x04[AI Bot]\x03 %s\x01 带着除颤器走向 \x03%s\x01 (dist=%.0f)", dbgN, tgtN, dist);
        }
        return false;  // DON'T LOCK
    }

    // ================================================================
    // Within DEFIB_USE_RANGE - REVIVE THE DEAD PLAYER (LOCKS)
    // ================================================================

    // Reset revive start time - bot has reached the corpse
    g_fDefibReviveStartTime[client] = 0.0;

    // Clear IN_USE to prevent picking up items near corpse
    buttons &= ~IN_USE;

    int activeWeapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");

    if (activeWeapon != weapon) {
        // Direct weapon switch
        SetEntPropEnt(client, Prop_Send, "m_hActiveWeapon", weapon);
        FakeClientCommand(client, "use weapon_defibrillator");

        if (g_fDefibWeaponSwitchTime[client] == 0.0) {
            g_fDefibWeaponSwitchTime[client] = GetGameTime();
        }

        float aimPos[3];
        aimPos = defibPos;
        aimPos[2] += 10.0;
        AI_SnapViewToPosition(client, aimPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
        return true;
    }

    // Wait for weapon deploy
    float nextAttack = GetEntPropFloat(weapon, Prop_Send, "m_flNextPrimaryAttack");
    float gameTime = GetGameTime();
    if (nextAttack > gameTime) {
        float aimPos[3];
        aimPos = defibPos;
        aimPos[2] += 10.0;
        AI_SnapViewToPosition(client, aimPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
        return true;
    }

    // Fire defib
    float aimPos[3];
    aimPos = defibPos;
    aimPos[2] += 10.0;
    AI_SnapViewToPosition(client, aimPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
    buttons |= IN_ATTACK;

    if (g_bDebugMode) {
        char dbgName3[64]; GetClientName(client, dbgName3, sizeof(dbgName3));
        char targetName[64];
        if (IsClientInGame(target)) GetClientName(target, targetName, sizeof(targetName));
        else strcopy(targetName, sizeof(targetName), "???");
        PrintToChatAll("\x04[AI Bot]\x03 %s\x01 正在用除颤器救 \x03%s\x01 (dist=%.0f)", dbgName3, targetName, dist);
    }

    return true;  // Lock while using defib
}

/**
 * Reset defibrillator task state.
 */
void TaskDefib_Reset(int client) {
    g_BotState[client].defibTarget = -1;
    g_iDefibSearchTarget[client] = -1;
    g_bDefibSearchMode[client] = false;
    g_fDefibWeaponSwitchTime[client] = 0.0;
    g_fDefibSearchStartTime[client] = 0.0;
    g_bDefibNeedDropMedkit[client] = false;
    g_bDefibSearchIsSpawn[client] = false;
    g_fDefibReviveStartTime[client] = 0.0;
    g_BotState[client].hasMoveTarget = false;
    AI_ResetTeleportState(client);
}

// ============================================================
// Internal helpers
// ============================================================

/**
 * Find the nearest REACHABLE defibrillator within search range.
 * Searches BOTH dropped weapon_defibrillator entities AND
 * weapon_defibrillator_spawn entities (defibs on tables/shelves).
 * @return Entity index, or -1 if none found.
 */
static int TaskDefib_FindNearest(int client) {
    int best = -1;
    float bestDist = DEFIB_SEARCH_RANGE + 1.0;
    bool bestIsSpawn = false;

    // 1) Search for dropped weapon_defibrillator entities on the ground
    int ent = -1;
    int totalFound = 0;
    int skippedOwner = 0;
    int skippedDist = 0;
    int skippedReach = 0;

    while ((ent = FindEntityByClassname(ent, "weapon_defibrillator")) != -1) {
        if (!IsValidEntity(ent)) continue;
        totalFound++;

        // Skip defibs held by LIVING players (in their inventory)
        int owner = GetEntPropEnt(ent, Prop_Data, "m_hOwnerEntity");
        if (owner > 0 && owner <= MaxClients && IsPlayerAlive(owner)) {
            skippedOwner++;
            continue;
        }

        float entPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, entPos);
        if (dist > DEFIB_SEARCH_RANGE) {
            skippedDist++;
            continue;
        }
        if (dist >= bestDist) continue;

        // Skip unreachable defibs (underground / inside geometry)
        if (!TaskDefib_IsReachable(client, ent)) {
            skippedReach++;
            continue;
        }

        bestDist = dist;
        best = ent;
        bestIsSpawn = false;
    }

    // 2) Search for weapon_defibrillator_spawn entities (defibs on tables/shelves)
    int spawnSkip = 0;
    int spawnFound = 0;
    ent = -1;
    while ((ent = FindEntityByClassname(ent, "weapon_defibrillator_spawn")) != -1) {
        if (!IsValidEntity(ent)) continue;

        // Check if spawn is still active (not picked clean)
        // m_nCount may not exist on all entity types, so check safely
        bool hasItems = true;
        int offset = FindDataMapInfo(ent, "m_nCount");
        if (offset > 0) {
            int count = GetEntProp(ent, Prop_Data, "m_nCount");
            if (count <= 0) {
                hasItems = false;
                spawnSkip++;
                continue;
            }
        }

        spawnFound++;
        float entPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, entPos);
        if (dist > DEFIB_SEARCH_RANGE) {
            skippedDist++;
            continue;
        }
        if (dist >= bestDist) continue;

        // Skip unreachable spawns
        if (!TaskDefib_IsReachable(client, ent)) {
            skippedReach++;
            continue;
        }

        bestDist = dist;
        best = ent;
        bestIsSpawn = true;
    }

    // Store spawn flag for this bot
    g_bDefibSearchIsSpawn[client] = bestIsSpawn;

    return best;
}
