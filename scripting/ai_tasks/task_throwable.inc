/**
 * AI Task: Throwable
 * 
 * Priority Order: 1
 * Searches for throwable items (pipe bombs, molotovs, bile jars),
 * picks them up, and uses them at optimal moments.
 */

#if defined _task_throwable_included
    #endinput
#endif
#define _task_throwable_included

// Throwable constants
#define THROW_SEARCH_RANGE      600.0
#define THROW_PICKUP_RANGE      50.0
#define THROW_TANK_RANGE        600.0
#define THROW_USE_COMMON_MIN    8
#define THROW_USE_SI_MIN        3
#define THROW_SCAN_INTERVAL     0.5

// Per-bot throwable search state
static int g_iThrowSearchTarget[MAXPLAYERS + 1];
static float g_fThrowScanTime[MAXPLAYERS + 1];

/**
 * Check if throwable task should activate for this bot.
 */
bool TaskThrowable_ShouldUpdate(int client) {
    if (!g_bThrowableEnabled) return false;
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;

    int throwSlot = GetPlayerWeaponSlot(client, 2);

    // Mode A: No throwable — search for nearby ones
    if (throwSlot == -1) {
        // Only skip search during active Tank combat (still search during SI combat)
        if (g_BotState[client].isInCombat && g_BotState[client].targetType == 3) return false;

        float curTime = GetGameTime();
        if ((curTime - g_fThrowScanTime[client]) < THROW_SCAN_INTERVAL) {
            // If we already have a search target, keep going
            return (g_iThrowSearchTarget[client] > 0 && IsValidEntity(g_iThrowSearchTarget[client]));
        }
        g_fThrowScanTime[client] = curTime;

        // Scan for nearby throwables
        int best = TaskThrowable_FindNearest(client);
        if (best != -1) {
            g_iThrowSearchTarget[client] = best;
            return true;
        }
        return false;
    }

    // Mode B: Has throwable — check for use conditions
    return TaskThrowable_ShouldUse(client);
}

/**
 * Execute throwable logic: pickup or use.
 * @return true while picking up (lock task), false otherwise.
 */
bool TaskThrowable_Update(int client, int &buttons, float vel[3]) {
    int throwSlot = GetPlayerWeaponSlot(client, 2);

    // === Pickup Mode ===
    if (throwSlot == -1) {
        int target = g_iThrowSearchTarget[client];
        if (target <= 0 || !IsValidEntity(target)) {
            TaskThrowable_Reset(client);
            return false;
        }

        float targetPos[3];
        GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", targetPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, targetPos);

        if (dist < THROW_PICKUP_RANGE) {
            // Close enough — pick up
            targetPos[2] += 10.0;
            AI_SnapViewToPosition(client, targetPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
            AI_PressUseButton(client, buttons);
            return true;  // Lock while picking up
        }

        // Move toward throwable
        AI_ComputeMoveToward(client, targetPos, g_BotState[client].absOrigin,
                              g_BotState[client].eyeAngles, vel);

        // Stuck teleport for throwable pickup
        float throwElapsed = GetGameTime() - g_BotState[client].taskStartTime;
        if (AI_ShouldAbandonNavigation(client)) {
            TaskThrowable_Reset(client);
            return false;
        }
        AI_TryStuckTeleport(client, targetPos, dist, throwElapsed, 300.0, 10.0);

        return true;  // Lock while approaching to prevent combat interruption
    }

    // === Use Mode ===
    if (!TaskThrowable_ShouldUse(client)) {
        return false;
    }

    // Find the best throw target position
    float throwTarget[3];
    bool foundTarget = TaskThrowable_FindThrowTarget(client, throwTarget);
    if (!foundTarget) {
        return false;
    }

    // Switch to throwable slot
    int activeWeapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
    if (activeWeapon != throwSlot) {
        FakeClientCommand(client, "slot3");
        return true;
    }

    // Aim at target and throw
    AI_SnapViewToPosition(client, throwTarget, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
    buttons |= IN_ATTACK;
    return true;
}

/**
 * Reset throwable state.
 */
void TaskThrowable_Reset(int client) {
    g_iThrowSearchTarget[client] = -1;
    AI_ResetTeleportState(client);
}

// ============================================================
// Internal helpers
// ============================================================

/**
 * Find nearest throwable entity within search range.
 */
static int TaskThrowable_FindNearest(int client) {
    int best = -1;
    float bestDist = THROW_SEARCH_RANGE + 1.0;

    // Search all three throwable types
    static const char throwClassnames[][] = {
        "weapon_pipe_bomb",
        "weapon_molotov",
        "weapon_vomitjar"
    };

    for (int t = 0; t < 3; t++) {
        int ent = -1;
        int safetyCount = 0;
        while ((ent = FindEntityByClassname(ent, throwClassnames[t])) != -1) {
            if (++safetyCount > 256) break;
            if (!IsValidEntity(ent)) { ent++; continue; }

            // Skip owned throwables
            int owner = GetEntPropEnt(ent, Prop_Data, "m_hOwnerEntity");
            if (owner > 0 && owner <= MaxClients) continue;

            float entPos[3];
            GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);
            float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, entPos);
            if (dist > THROW_SEARCH_RANGE) continue;
            if (dist >= bestDist) continue;

            if (AI_IsVisiblePosition(g_BotState[client].eyePos, entPos, MASK_SHOT, client)) {
                bestDist = dist;
                best = ent;
            }
        }
    }

    return best;
}

/**
 * Check if conditions are right to use a held throwable.
 */
static bool TaskThrowable_ShouldUse(int client) {
    // Condition a: Large common horde
    if (g_BotState[client].visibleCommons >= THROW_USE_COMMON_MIN) {
        return true;
    }

    // Condition b: Tank nearby
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 3) continue;
        if (GetEntProp(i, Prop_Send, "m_zombieClass") != 8) continue;

        float tankPos[3];
        GetClientAbsOrigin(i, tankPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, tankPos);
        if (dist <= THROW_TANK_RANGE) {
            return true;
        }
    }

    // Condition c: 3+ special infected clustered
    if (g_BotState[client].visibleThreats >= THROW_USE_SI_MIN) {
        return true;
    }

    return false;
}

/**
 * Find the best position to throw at.
 * Priority: Tank > SI cluster center > common horde center.
 */
static bool TaskThrowable_FindThrowTarget(int client, float result[3]) {
    // Priority 1: Tank
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 3) continue;
        if (GetEntProp(i, Prop_Send, "m_zombieClass") != 8) continue;

        float tankPos[3];
        GetClientAbsOrigin(i, tankPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, tankPos);
        if (dist <= THROW_TANK_RANGE) {
            result[0] = tankPos[0];
            result[1] = tankPos[1];
            result[2] = tankPos[2] + 40.0;
            return true;
        }
    }

    // Priority 2: Common horde center
    float cx = 0.0, cy = 0.0, cz = 0.0;
    int count = 0;
    int common = -1;
    int safetyCount = 0;
    while ((common = FindEntityByClassname(common, "infected")) != -1) {
        if (++safetyCount > 256) break;
        if (!IsValidEntity(common)) { common++; continue; }

        float cPos[3];
        GetEntPropVector(common, Prop_Data, "m_vecAbsOrigin", cPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, cPos);
        if (dist > 500.0) continue;

        cx += cPos[0];
        cy += cPos[1];
        cz += cPos[2];
        count++;
        if (count >= 15) break;
    }

    if (count >= 5) {
        result[0] = cx / float(count);
        result[1] = cy / float(count);
        result[2] = (cz / float(count)) + 20.0;
        return true;
    }

    // Priority 3: Closest visible SI
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 3) continue;
        if (GetEntProp(i, Prop_Send, "m_isGhost") != 0) continue;

        float siPos[3];
        GetClientAbsOrigin(i, siPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, siPos);
        if (dist > THROW_TANK_RANGE) continue;

        result[0] = siPos[0];
        result[1] = siPos[1];
        result[2] = siPos[2] + 30.0;
        return true;
    }

    return false;
}
