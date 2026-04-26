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

// Tank rock and Charger detection
#define TANK_ROCK_DETECT_RANGE   500.0   // Detection range for incoming Tank rock
#define CHARGER_DETECT_RANGE     400.0   // Detection range for charging Charger

// Self-heal thresholds
#define SELF_HEAL_HP_PILLS       40      // Use pills/adrenaline below this HP
#define SELF_HEAL_HP_ADRENALINE  30      // Use adrenaline below this HP (faster)

// General SI damage reduction by skill level (index = level 0..4)
// Lv0: 0%, Lv1: 10%, Lv2: 20%, Lv3: 30%, Lv4: 40%
static float g_flSIDmgReduction[SKILL_LEVEL_COUNT] = {
    0.0, 0.10, 0.20, 0.30, 0.40
};

// Tank damage reduction by skill level (index = level 0..4)
// Lv0: 0%, Lv1: 5%, Lv2: 10%, Lv3: 20%, Lv4: 30%
static float g_flTankDmgReduction[SKILL_LEVEL_COUNT] = {
    0.0, 0.05, 0.10, 0.20, 0.30
};

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

    // === 0.5. Instant-kill protection (ConVar controlled) ===
    // When enabled, cap damage so the bot is never one-shot killed or incapped
    if (g_bProtectEnabled) {
        int hp = GetClientHealth(victim);
        bool isIncap = (GetEntProp(victim, Prop_Send, "m_isIncapacitated") != 0);

        if (!isIncap) {
            // Bot is standing — prevent one-shot incap
            if (damage >= float(hp) && hp > 0) {
                damage = float(hp) - 1.0;
                if (damage < 1.0) damage = 1.0;
                return Plugin_Changed;
            }
        } else {
            // Bot is incapped — prevent instant death
            if (damage >= float(hp) && hp > 0) {
                damage = float(hp) - 1.0;
                if (damage < 1.0) damage = 1.0;
                return Plugin_Changed;
            }
        }
    }

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

    // === 4. Tank Damage Reduction ===
    if (attacker > 0 && attacker <= MaxClients && IsClientInGame(attacker)) {
        if (GetClientTeam(attacker) == 3 && GetEntProp(attacker, Prop_Send, "m_zombieClass") == 8) {
            float reduction = g_flTankDmgReduction[level];
            if (reduction > 0.0) {
                damage *= (1.0 - reduction);
                return Plugin_Changed;
            }
        }
    }

    // === 5. General SI Damage Reduction ===
    if (attacker > 0 && attacker <= MaxClients && IsClientInGame(attacker)) {
        if (GetClientTeam(attacker) == 3) {
            // Skip Tank (already handled above)
            if (GetEntProp(attacker, Prop_Send, "m_zombieClass") != 8) {
                float reduction = g_flSIDmgReduction[level];
                if (reduction > 0.0) {
                    damage *= (1.0 - reduction);
                    return Plugin_Changed;
                }
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

    // === Self-heal: use pills/adrenaline when low HP ===
    int level = g_iBotSkillLevel;
    if (level < SKILL_LEVEL_MIN) level = SKILL_LEVEL_MIN;
    if (level > SKILL_LEVEL_MAX) level = SKILL_LEVEL_MAX;
    // Only self-heal at skill level 2+ (smarter bots know to use items)
    if (level >= 2) {
        int hp = GetClientHealth(client);
        int slot4 = GetPlayerWeaponSlot(client, 4);  // Pills/Adrenaline slot
        if (slot4 != -1 && hp <= SELF_HEAL_HP_PILLS) {
            char cls[64];
            GetEntityClassname(slot4, cls, sizeof(cls));
            if (StrEqual(cls, "weapon_pain_pills") || StrEqual(cls, "weapon_adrenaline")) {
                // Use the item
                FakeClientCommand(client, "use %s", cls);
                if (g_bDebugMode) {
                    char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                    PrintToServer("[AI Bot] %s self-heal: using %s (HP=%d)", dbgN, cls, hp);
                }
                return;
            }
        }
    }

    // === Tank rock dodge ===
    int level2 = level;
    if (level2 >= 2) {  // Skill 2+ can dodge rocks
        int rockEnt = DamageProtect_FindIncomingTankRock(client);
        if (rockEnt != -1) {
            float rockPos[3];
            GetEntPropVector(rockEnt, Prop_Data, "m_vecAbsOrigin", rockPos);
            DamageProtect_StartDodge(client, rockPos, curTime, 300.0, 1.5);
            return;
        }
    }

    // === Charger dodge ===
    if (level2 >= 1) {  // Skill 1+ can dodge chargers
        int chargerEnt = DamageProtect_FindChargingCharger(client);
        if (chargerEnt != -1) {
            float chargerPos[3];
            GetClientAbsOrigin(chargerEnt, chargerPos);
            DamageProtect_StartDodge(client, chargerPos, curTime, 250.0, 1.2);
            return;
        }
    }

    // Detect nearest danger (acid/fire)
    float dangerPos[3];
    int dangerEnt = DamageProtect_FindNearbyDanger(client, dangerPos);
    if (dangerEnt == -1) return;

    DamageProtect_StartDodge(client, dangerPos, curTime, DANGER_DODGE_DISTANCE, DANGER_DODGE_DURATION);
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
    int safetyCount = 0;
    while ((ent = FindEntityByClassname(ent, "insect_swarm")) != -1) {
        if (++safetyCount > 256) break;
        if (!IsValidEntity(ent)) { ent++; continue; }
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
    safetyCount = 0;
    while ((ent = FindEntityByClassname(ent, "inferno")) != -1) {
        if (++safetyCount > 256) break;
        if (!IsValidEntity(ent)) { ent++; continue; }
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

/**
 * Start a dodge maneuver away from a position.
 */
static void DamageProtect_StartDodge(int client, const float dangerPos[3], float curTime,
                                      float dodgeDist, float dodgeDuration) {
    float pos[3], escapeDir[3], escapeTarget[3];
    pos[0] = g_BotState[client].absOrigin[0];
    pos[1] = g_BotState[client].absOrigin[1];
    pos[2] = g_BotState[client].absOrigin[2];

    SubtractVectors(pos, dangerPos, escapeDir);
    escapeDir[2] = 0.0;

    float len = GetVectorLength(escapeDir);
    if (len < 1.0) {
        escapeDir[0] = 1.0;
        escapeDir[1] = 0.0;
    }
    NormalizeVector(escapeDir, escapeDir);
    ScaleVector(escapeDir, dodgeDist);
    AddVectors(pos, escapeDir, escapeTarget);

    g_BotState[client].moveTarget[0] = escapeTarget[0];
    g_BotState[client].moveTarget[1] = escapeTarget[1];
    g_BotState[client].moveTarget[2] = escapeTarget[2];
    g_BotState[client].hasMoveTarget = true;
    g_BotState[client].isDodging = true;
    g_BotState[client].dodgeExpireTime = curTime + dodgeDuration;
}

/**
 * Find an incoming Tank rock (tank_rock entity) heading toward the bot.
 */
static int DamageProtect_FindIncomingTankRock(int client) {
    float pos[3];
    pos = g_BotState[client].absOrigin;

    int ent = -1;
    int safetyCount = 0;
    while ((ent = FindEntityByClassname(ent, "tank_rock")) != -1) {
        if (++safetyCount > 256) break;
        if (!IsValidEntity(ent)) { ent++; continue; }

        float rockPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", rockPos);
        float dist = AI_GetVectorDistance(pos, rockPos);

        if (dist > TANK_ROCK_DETECT_RANGE) continue;

        // Check if rock is moving toward the bot (velocity check)
        float rockVel[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsVelocity", rockVel);
        float rockSpeed = GetVectorLength(rockVel);
        if (rockSpeed < 50.0) continue;  // Rock isn't really moving

        // Direction from rock to bot
        float toBot[3];
        SubtractVectors(pos, rockPos, toBot);
        toBot[2] = 0.0;
        float toBotLen = GetVectorLength(toBot);
        if (toBotLen < 1.0) continue;

        // Normalize
        toBot[0] /= toBotLen;
        toBot[1] /= toBotLen;

        // Normalize rock velocity (horizontal)
        float normVel[3];
        normVel[0] = rockVel[0];
        normVel[1] = rockVel[1];
        normVel[2] = 0.0;
        float velLen = GetVectorLength(normVel);
        if (velLen < 1.0) continue;
        normVel[0] /= velLen;
        normVel[1] /= velLen;

        // Dot product: if rock is heading toward bot, value > 0
        float dot = normVel[0] * toBot[0] + normVel[1] * toBot[1];
        if (dot > 0.5) {
            return ent;  // Rock is heading toward this bot
        }
    }

    return -1;
}

/**
 * Find a Charger that is currently charging toward the bot.
 */
static int DamageProtect_FindChargingCharger(int client) {
    float pos[3];
    pos = g_BotState[client].absOrigin;

    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 3) continue;
        if (GetEntProp(i, Prop_Send, "m_zombieClass") != 6) continue;  // ZC_CHARGER = 6

        // Check if Charger is in charging state
        MoveType mt = GetEntityMoveType(i);
        if (mt != MOVETYPE_ISOMETRIC) continue;  // Charger charges with isometric move type

        float chargerPos[3];
        GetClientAbsOrigin(i, chargerPos);
        float dist = AI_GetVectorDistance(pos, chargerPos);
        if (dist > CHARGER_DETECT_RANGE) continue;

        // Check if Charger is heading toward the bot
        float chargerVel[3];
        GetEntPropVector(i, Prop_Data, "m_vecAbsVelocity", chargerVel);
        float speed = GetVectorLength(chargerVel);
        if (speed < 100.0) continue;  // Not really charging

        float toBot[3];
        SubtractVectors(pos, chargerPos, toBot);
        toBot[2] = 0.0;
        float toBotLen = GetVectorLength(toBot);
        if (toBotLen < 1.0) continue;
        toBot[0] /= toBotLen;
        toBot[1] /= toBotLen;

        float normVel[3];
        normVel[0] = chargerVel[0];
        normVel[1] = chargerVel[1];
        normVel[2] = 0.0;
        float velLen = GetVectorLength(normVel);
        if (velLen < 1.0) continue;
        normVel[0] /= velLen;
        normVel[1] /= velLen;

        float dot = normVel[0] * toBot[0] + normVel[1] * toBot[1];
        if (dot > 0.3) {
            return i;  // Charger is heading toward this bot
        }
    }

    return -1;
}
