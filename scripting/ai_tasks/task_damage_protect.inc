/**
 * AI Task: Damage Protection
 * 
 * Protects Survivor Bots from environmental hazards:
 *   - Fall damage:  Teleport back to last safe position when fatal fall detected
 *   - Fire damage:  Reduce burn damage based on skill level
 *   - Acid damage:  Reduce Spitter acid damage based on skill level
 *   - Danger dodge: Actively move away from acid pools and fire before taking damage
 *
 * This module works via SDKHook (OnTakeDamageAlive), not through the task queue
 * ShouldUpdate/Update cycle. It is registered as a task ID for bookkeeping only.
 *
 * Safe position caching runs every 5 seconds inside BotState_CacheBasicData
 * (called from the main frame loop, hooked in the main plugin file).
 * Danger zone dodge checks run every 0.3s during safe pos caching.
 */

#if defined _task_damage_protect_included
    #endinput
#endif
#define _task_damage_protect_included

// Damage type bitmasks (Source Engine)
#if !defined DMG_BURN
#define DMG_BURN   8
#endif
#if !defined DMG_FALL
#define DMG_FALL   32
#endif

// Safe position cache interval (seconds)
#define SAFE_POS_CACHE_INTERVAL  5.0

// Fall damage threshold — above this amount triggers teleport protection
#define FALL_DAMAGE_THRESHOLD    100.0

// Danger zone dodge parameters
#define DANGER_DETECT_RANGE      200.0   // Detection radius for hazards
#define DANGER_DODGE_DISTANCE    200.0   // How far to flee from hazard
#define DANGER_DODGE_DURATION    1.0     // Dodge movement duration (seconds)
#define DANGER_CHECK_INTERVAL    0.3     // Check interval to avoid per-frame cost

// Fire damage reduction by skill level (index = level 0..4)
// Lv0: 0%, Lv1: 0%, Lv2: 30%, Lv3: 45%, Lv4: 60%
static float g_flFireDmgReduction[SKILL_LEVEL_COUNT] = {
    0.0, 0.0, 0.30, 0.45, 0.60
};

// Acid damage reduction by skill level (index = level 0..4)
// Lv0: 0%, Lv1: 0%, Lv2: 20%, Lv3: 35%, Lv4: 50%
static float g_flAcidDmgReduction[SKILL_LEVEL_COUNT] = {
    0.0, 0.0, 0.20, 0.35, 0.50
};

// ============================================================
// Safe Position Caching
// Called from BotState_CacheBasicData or a dedicated timer.
// ============================================================

/**
 * Cache the bot's current position as a safe position if on ground
 * and enough time has elapsed since the last cache.
 *
 * @param client    Bot client index (must be a valid survivor bot).
 */
// Per-bot last danger check time (throttle detection)
static float g_flLastDangerCheck[MAXPLAYERS + 1];

void DamageProtect_CacheSafePos(int client) {
    float curTime = GetGameTime();

    // --- Danger zone active dodge check (runs more frequently) ---
    DamageProtect_CheckDangerZone(client, curTime);

    if ((curTime - g_BotState[client].lastSafePosTime) < SAFE_POS_CACHE_INTERVAL) {
        return;
    }

    // Only cache when bot is on the ground (FL_ONGROUND)
    int flags = GetEntityFlags(client);
    if (((flags & FL_ONGROUND) == 0)) {
        return;
    }

    // Store current position as safe
    g_BotState[client].lastSafePos[0] = g_BotState[client].absOrigin[0];
    g_BotState[client].lastSafePos[1] = g_BotState[client].absOrigin[1];
    g_BotState[client].lastSafePos[2] = g_BotState[client].absOrigin[2];
    g_BotState[client].lastSafePosTime = curTime;
}

// ============================================================
// SDKHook Callback — Damage Protection
// Registered in OnClientPutInServer alongside Combat_OnTakeDamageAlive.
// ============================================================

/**
 * SDKHook_OnTakeDamageAlive callback for damage protection.
 * Handles fall / fire / acid damage mitigation for Survivor Bots.
 *
 * @return Plugin_Changed if damage was modified, Plugin_Handled if blocked,
 *         Plugin_Continue otherwise.
 */
Action DamageProtect_OnTakeDamage(int victim, int &attacker, int &inflictor,
                                   float &damage, int &damagetype) {
    // === 0. Friendly fire immunity (ConVar controlled) ===
    if (g_bImmunityEnabled)
    {
        if (attacker > 0 && attacker <= MaxClients && IsClientInGame(attacker))
        {
            if (GetClientTeam(attacker) == 2 && GetClientTeam(victim) == 2)
            {
                // 友方 Survivor 对 Bot 的伤害清零
                damage = 0.0;
                return Plugin_Changed;
            }
        }
    }

    // Only protect survivor bots
    if (victim < 1 || victim > MaxClients) return Plugin_Continue;
    if (!IsClientInGame(victim)) return Plugin_Continue;
    if (!IsFakeClient(victim)) return Plugin_Continue;
    if (GetClientTeam(victim) != 2) return Plugin_Continue;
    if (!IsPlayerAlive(victim)) return Plugin_Continue;

    // Get current skill level (clamped)
    int level = g_iBotSkillLevel;
    if (level < SKILL_LEVEL_MIN) level = SKILL_LEVEL_MIN;
    if (level > SKILL_LEVEL_MAX) level = SKILL_LEVEL_MAX;

    // === 1. Fall Damage Protection ===
    if (((damagetype & DMG_FALL) != 0) && damage > FALL_DAMAGE_THRESHOLD) {
        // Teleport back to last known safe position
        if (g_BotState[victim].lastSafePosTime > 0.0) {
            TeleportEntity(victim, g_BotState[victim].lastSafePos, NULL_VECTOR, NULL_VECTOR);
            LogMessage("[AI DmgProtect] Bot %d teleported back to safe pos (fall dmg %.0f)", victim, damage);
        }
        // Block the fall damage entirely after teleport
        damage = 0.0;
        return Plugin_Changed;
    }

    // === 2. Fire Damage Reduction ===
    if (((damagetype & DMG_BURN) != 0)) {
        float reduction = g_flFireDmgReduction[level];
        if (reduction > 0.0) {
            damage *= (1.0 - reduction);
            return Plugin_Changed;
        }
    }

    // === 3. Acid (Spitter) Damage Reduction ===
    // Spitter acid entities use classname "insect_swarm"
    if (inflictor > MaxClients && IsValidEntity(inflictor)) {
        char inflictorClass[64];
        GetEntityClassname(inflictor, inflictorClass, sizeof(inflictorClass));
        if (StrEqual(inflictorClass, "insect_swarm")) {
            float reduction = g_flAcidDmgReduction[level];
            if (reduction > 0.0) {
                damage *= (1.0 - reduction);
                return Plugin_Changed;
            }
        }
    }

    return Plugin_Continue;
}

// ============================================================
// Danger Zone Active Dodge
// Detects nearby acid pools (insect_swarm) and fire (inferno)
// and drives the bot away before taking lethal damage.
// ============================================================

/**
 * Periodic danger zone check. Called every frame from the main loop
 * but internally throttled to DANGER_CHECK_INTERVAL.
 */
static void DamageProtect_CheckDangerZone(int client, float curTime) {
    // If currently dodging, check if dodge has expired
    if (g_BotState[client].isDodging) {
        if (curTime >= g_BotState[client].dodgeExpireTime) {
            g_BotState[client].isDodging = false;
            g_BotState[client].hasMoveTarget = false;
        }
        return; // Don't re-check while actively dodging
    }

    // Throttle detection checks
    if ((curTime - g_flLastDangerCheck[client]) < DANGER_CHECK_INTERVAL) {
        return;
    }
    g_flLastDangerCheck[client] = curTime;

    // Don't dodge while incapacitated or pinned
    if (g_BotState[client].isIncapacitated) return;
    if (g_BotState[client].controlledByType > 0) return;

    // Detect nearest danger
    float dangerPos[3];
    int dangerEnt = DamageProtect_FindNearbyDanger(client, dangerPos);
    if (dangerEnt == -1) return;

    // Compute escape direction: away from danger, horizontal only
    float pos[3], escapeDir[3], escapeTarget[3];
    pos[0] = g_BotState[client].absOrigin[0];
    pos[1] = g_BotState[client].absOrigin[1];
    pos[2] = g_BotState[client].absOrigin[2];

    SubtractVectors(pos, dangerPos, escapeDir);
    escapeDir[2] = 0.0; // Stay on horizontal plane

    float len = GetVectorLength(escapeDir);
    if (len < 1.0) {
        // Directly on top of danger — pick arbitrary direction
        escapeDir[0] = 1.0;
        escapeDir[1] = 0.0;
    }
    NormalizeVector(escapeDir, escapeDir);
    ScaleVector(escapeDir, DANGER_DODGE_DISTANCE);
    AddVectors(pos, escapeDir, escapeTarget);

    // Set move target to escape position
    g_BotState[client].moveTarget[0] = escapeTarget[0];
    g_BotState[client].moveTarget[1] = escapeTarget[1];
    g_BotState[client].moveTarget[2] = escapeTarget[2];
    g_BotState[client].hasMoveTarget = true;
    g_BotState[client].isDodging = true;
    g_BotState[client].dodgeExpireTime = curTime + DANGER_DODGE_DURATION;
}

/**
 * Find the nearest danger entity (acid pool or fire) within detection range.
 *
 * @param client      Bot client index.
 * @param dangerPos   Output: position of the nearest danger entity.
 * @return            Entity index of nearest danger, or -1 if none.
 */
static int DamageProtect_FindNearbyDanger(int client, float dangerPos[3]) {
    float pos[3];
    pos[0] = g_BotState[client].absOrigin[0];
    pos[1] = g_BotState[client].absOrigin[1];
    pos[2] = g_BotState[client].absOrigin[2];

    float closest = DANGER_DETECT_RANGE;
    int found = -1;

    // Check acid pools (insect_swarm)
    int ent = -1;
    while ((ent = FindEntityByClassname(ent, "insect_swarm")) != -1) {
        if (!IsValidEntity(ent)) continue;
        float entPos[3];
        GetEntPropVector(ent, Prop_Send, "m_vecOrigin", entPos);
        float dist = GetVectorDistance(pos, entPos);
        if (dist < closest) {
            closest = dist;
            found = ent;
            dangerPos[0] = entPos[0];
            dangerPos[1] = entPos[1];
            dangerPos[2] = entPos[2];
        }
    }

    // Check fire (inferno)
    ent = -1;
    while ((ent = FindEntityByClassname(ent, "inferno")) != -1) {
        if (!IsValidEntity(ent)) continue;
        float entPos[3];
        GetEntPropVector(ent, Prop_Send, "m_vecOrigin", entPos);
        float dist = GetVectorDistance(pos, entPos);
        if (dist < closest) {
            closest = dist;
            found = ent;
            dangerPos[0] = entPos[0];
            dangerPos[1] = entPos[1];
            dangerPos[2] = entPos[2];
        }
    }

    return found;
}
