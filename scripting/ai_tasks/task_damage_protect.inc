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
#define HUNTER_DODGE_RANGE       400.0   // Detection range for pouncing Hunter
#define SMOKER_DODGE_RANGE       600.0   // Detection range for Smoker targeting

// Lv6 Cheat: enhanced detection ranges and self-heal
#define LV6_TANK_ROCK_RANGE      800.0   // Lv6: detect rocks much earlier
#define LV6_CHARGER_RANGE        600.0   // Lv6: detect Chargers earlier
#define LV6_HUNTER_RANGE         600.0   // Lv6: detect Hunters earlier
#define LV6_SMOKER_RANGE         900.0   // Lv6: detect Smokers much earlier
#define LV6_SELF_HEAL_COOLDOWN   1.5     // Lv6: faster self-heal (1.5s vs 3.0s)
#define LV6_SELF_HEAL_HP_PILLS   60      // Lv6: use pills earlier (60 vs 40 HP)

// Self-heal thresholds
#define SELF_HEAL_HP_PILLS       40      // Use pills/adrenaline below this HP
#define SELF_HEAL_HP_ADRENALINE  30      // Use adrenaline below this HP (faster)
#define SELF_HEAL_COOLDOWN       3.0     // Seconds between self-heal attempts

// General SI damage reduction by skill level (index = level 0..5)
// Lv0: 0%, Lv1: 10%, Lv2: 20%, Lv3: 30%, Lv4: 40%, Lv5: 80%
static float g_flSIDmgReduction[SKILL_LEVEL_COUNT] = {
    0.0, 0.10, 0.20, 0.30, 0.40, 0.80
};

// Tank damage reduction by skill level (index = level 0..5)
// Lv0: 0%, Lv1: 5%, Lv2: 10%, Lv3: 20%, Lv4: 30%, Lv5: 70%
static float g_flTankDmgReduction[SKILL_LEVEL_COUNT] = {
    0.0, 0.05, 0.10, 0.20, 0.30, 0.70
};

// Fire damage reduction by skill level (index = level 0..5)
// Lv0: 0%, Lv1: 0%, Lv2: 30%, Lv3: 45%, Lv4: 60%, Lv5: 95%
static float g_flFireDmgReduction[SKILL_LEVEL_COUNT] = {
    0.0, 0.0, 0.30, 0.45, 0.60, 0.95
};

// Acid damage reduction by skill level (index = level 0..5)
// Lv0: 0%, Lv1: 0%, Lv2: 20%, Lv3: 35%, Lv4: 50%, Lv5: 90%
static float g_flAcidDmgReduction[SKILL_LEVEL_COUNT] = {
    0.0, 0.0, 0.20, 0.35, 0.50, 0.90
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
// Per-bot last self-heal attempt time (prevent spam)
static float g_flLastSelfHeal[MAXPLAYERS + 1];

// Shared SI scan cache — avoid 4 MaxClients loops per bot per 0.3s
static int g_iSharedPouncingHunter = -1;
static int g_iSharedSmokerTarget[MAXPLAYERS + 1] = {0, ...};  // 0=none, client=being targeted
static int g_iSharedChargingCharger = -1;
static float g_fSharedSIScanTime = 0.0;
#define SI_SCAN_SHARED_INTERVAL 0.3

// Shared danger scan cache — avoid 12 FindEntityByClassname loops per 0.3s (3 per bot x 4 bots)
#define MAX_SHARED_DANGERS 16
static int g_iSharedDangerEnts[MAX_SHARED_DANGERS];       // Entity indices
static float g_fSharedDangerPos[MAX_SHARED_DANGERS][3];   // Danger positions
static int g_iSharedDangerCount = 0;                       // How many dangers cached
static int g_iSharedRockEnt = -1;                          // Incoming tank rock entity
static float g_fSharedRockPos[3];                          // Rock position
static float g_fSharedDangerScanTime = 0.0;                // Last scan time
#define DANGER_SCAN_SHARED_INTERVAL 0.3

/**
 * Shared SI scan — runs once per interval for ALL bots.
 * Results are cached and reused by individual bots.
 */
static void DamageProtect_SharedSIScan(float curTime) {
    if ((curTime - g_fSharedSIScanTime) < SI_SCAN_SHARED_INTERVAL) return;
    g_fSharedSIScanTime = curTime;

    // Reset
    g_iSharedPouncingHunter = -1;
    g_iSharedChargingCharger = -1;
    for (int i = 0; i <= MaxClients; i++) g_iSharedSmokerTarget[i] = 0;

    // Scan all SI once
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 3) continue;

        int zClass = GetEntProp(i, Prop_Send, "m_zombieClass");

        // Hunter pounce detection
        if (zClass == 3) {
            int flags = GetEntityFlags(i);
            if ((flags & FL_ONGROUND) == 0) {
                // Check if heading toward any survivor
                float hunterVel[3];
                GetEntPropVector(i, Prop_Data, "m_vecAbsVelocity", hunterVel);
                if (GetVectorLength(hunterVel) > 100.0) {
                    g_iSharedPouncingHunter = i;
                }
            }
        }
        // Smoker targeting detection
        else if (zClass == 1) {
            // Check who Smoker is pulling
            for (int vi = 1; vi <= MaxClients; vi++) {
                if (!IsClientInGame(vi) || !IsPlayerAlive(vi) || GetClientTeam(vi) != 2) continue;
                if (GetEntPropEnt(vi, Prop_Send, "m_tongueOwner") == i) {
                    g_iSharedSmokerTarget[vi] = i;
                }
            }
            // Check ability target
            int abilityEnt = GetEntPropEnt(i, Prop_Send, "m_customAbility");
            if (abilityEnt != -1 && IsValidEntity(abilityEnt)) {
                if (HasEntProp(abilityEnt, Prop_Send, "m_tongueVictim")) {
                    int victim = GetEntPropEnt(abilityEnt, Prop_Send, "m_tongueVictim");
                    if (victim > 0 && victim <= MaxClients) {
                        g_iSharedSmokerTarget[victim] = i;
                    }
                }
            }
        }
        // Charger detection
        else if (zClass == 6) {
            bool isCharging = false;
            int abilityEnt = GetEntPropEnt(i, Prop_Send, "m_customAbility");
            if (abilityEnt != -1 && IsValidEntity(abilityEnt)) {
                if (HasEntProp(abilityEnt, Prop_Send, "m_isCharging")) {
                    isCharging = (GetEntProp(abilityEnt, Prop_Send, "m_isCharging") != 0);
                }
            }
            if (!isCharging && GetEntityMoveType(i) == MOVETYPE_ISOMETRIC) {
                float vel[3];
                GetEntPropVector(i, Prop_Data, "m_vecAbsVelocity", vel);
                if (GetVectorLength(vel) > 200.0) isCharging = true;
            }
            if (isCharging) g_iSharedChargingCharger = i;
        }
    }
}

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
    // Lv6 Cheat mode: always enable instant-kill protection
    if (g_bProtectEnabled || g_bCheatMode) {
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

    // Run shared SI scan (once per 0.3s for all bots)
    DamageProtect_SharedSIScan(curTime);
    // Run shared danger scan (once per 0.5s for all bots)
    DamageProtect_SharedDangerScan(curTime);

    // === Self-heal: use pills/adrenaline when low HP ===
    int level = g_iBotSkillLevel;
    if (level < SKILL_LEVEL_MIN) level = SKILL_LEVEL_MIN;
    if (level > SKILL_LEVEL_MAX) level = SKILL_LEVEL_MAX;
    // Lv6 Cheat: faster cooldown, use pills at higher HP
    float healCooldown = g_bCheatMode ? LV6_SELF_HEAL_COOLDOWN : SELF_HEAL_COOLDOWN;
    int healThreshold = g_bCheatMode ? LV6_SELF_HEAL_HP_PILLS : SELF_HEAL_HP_PILLS;
    // Only self-heal at skill level 2+ (smarter bots know to use items)
    if (level >= 2 && (curTime - g_flLastSelfHeal[client]) >= healCooldown) {
        int hp = GetClientHealth(client);
        int slot4 = GetPlayerWeaponSlot(client, 4);  // Pills/Adrenaline slot
        if (slot4 != -1 && hp <= healThreshold) {
            char cls[64];
            GetEntityClassname(slot4, cls, sizeof(cls));
            if (StrEqual(cls, "weapon_pain_pills") || StrEqual(cls, "weapon_adrenaline")) {
                g_flLastSelfHeal[client] = curTime;
                FakeClientCommand(client, "use %s", cls);
                if (g_bDebugMode) {
                    char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                    PrintToServer("[AI Bot] %s self-heal: using %s (HP=%d)", dbgN, cls, hp);
                }
                return;
            }
        }
    }

    // === Hunter pounce dodge (using shared scan result) ===
    if (level >= 2 && g_iSharedPouncingHunter != -1) {
        // Validate hunter is still valid (may have died/despawned between scans)
        if (!IsClientInGame(g_iSharedPouncingHunter) || !IsPlayerAlive(g_iSharedPouncingHunter)) {
            g_iSharedPouncingHunter = -1;  // Invalidate stale cache
        } else {
        // Check if pouncing hunter is heading toward this bot
        float hunterRange = g_bCheatMode ? LV6_HUNTER_RANGE : HUNTER_DODGE_RANGE;
        float hunterPos[3], botPos[3];
        GetClientAbsOrigin(g_iSharedPouncingHunter, hunterPos);
        botPos = g_BotState[client].absOrigin;
        float dist = AI_GetVectorDistance(botPos, hunterPos);
        if (dist <= hunterRange) {
            // Verify hunter is heading toward this bot
            float hunterVel[3];
            GetEntPropVector(g_iSharedPouncingHunter, Prop_Data, "m_vecAbsVelocity", hunterVel);
            float toBot[3];
            SubtractVectors(botPos, hunterPos, toBot);
            toBot[2] = 0.0;
            float toBotLen = GetVectorLength(toBot);
            if (toBotLen > 1.0) {
                NormalizeVector(toBot, toBot);
                float normVel[3]; normVel[0] = hunterVel[0]; normVel[1] = hunterVel[1]; normVel[2] = 0.0;
                float velLen = GetVectorLength(normVel);
                if (velLen > 1.0) {
                    normVel[0] /= velLen; normVel[1] /= velLen;
                    if (normVel[0]*toBot[0] + normVel[1]*toBot[1] > 0.3) {
                        DamageProtect_StartPerpendicularDodge(client, hunterPos, curTime, 200.0, 0.8);
                        return;
                    }
                }
            }
        }
        }  // end else (hunter is valid)
    }

    // === Smoker dodge (using shared scan result) ===
    if (level >= 1 && g_iSharedSmokerTarget[client] != 0) {
        int smokerEnt = g_iSharedSmokerTarget[client];
        // Validate smoker is still alive (may have died between scans)
        if (smokerEnt > 0 && smokerEnt <= MaxClients && IsClientInGame(smokerEnt) && IsPlayerAlive(smokerEnt)) {
            float smokerPos[3];
            GetClientAbsOrigin(smokerEnt, smokerPos);
            DamageProtect_StartPerpendicularDodge(client, smokerPos, curTime, 150.0, 0.6);
            return;
        }
    }

    // === Tank rock dodge ===
    if (level >= 2) {  // Skill 2+ can dodge rocks
        // Lv6 Cheat: larger rock detection range
        float rockDetectRange = g_bCheatMode ? LV6_TANK_ROCK_RANGE : TANK_ROCK_DETECT_RANGE;
        int rockEnt = DamageProtect_FindIncomingTankRock(client, rockDetectRange);
        if (rockEnt != -1) {
            float rockPos[3];
            GetEntPropVector(rockEnt, Prop_Data, "m_vecAbsOrigin", rockPos);
            DamageProtect_StartDodge(client, rockPos, curTime, 300.0, 1.5);
            return;
        }
    }

    // === Charger dodge (using shared scan result) ===
    if (level >= 1 && g_iSharedChargingCharger != -1) {
        if (!IsClientInGame(g_iSharedChargingCharger) || !IsPlayerAlive(g_iSharedChargingCharger)) {
            g_iSharedChargingCharger = -1;  // Invalidate stale cache
        } else {
            float chargerRange = g_bCheatMode ? LV6_CHARGER_RANGE : CHARGER_DETECT_RANGE;
            float chargerPos[3], botPos[3];
            GetClientAbsOrigin(g_iSharedChargingCharger, chargerPos);
            botPos = g_BotState[client].absOrigin;
            float dist = AI_GetVectorDistance(botPos, chargerPos);
            if (dist <= chargerRange) {
                // Verify charger is heading toward this bot
                float chargerVel[3];
                GetEntPropVector(g_iSharedChargingCharger, Prop_Data, "m_vecAbsVelocity", chargerVel);
                float toBot[3];
                SubtractVectors(botPos, chargerPos, toBot);
                toBot[2] = 0.0;
                float toBotLen = GetVectorLength(toBot);
                if (toBotLen > 1.0) {
                    NormalizeVector(toBot, toBot);
                    float normVel[3]; normVel[0] = chargerVel[0]; normVel[1] = chargerVel[1]; normVel[2] = 0.0;
                    float velLen = GetVectorLength(normVel);
                    if (velLen > 1.0) {
                        normVel[0] /= velLen; normVel[1] /= velLen;
                        if (normVel[0]*toBot[0] + normVel[1]*toBot[1] > 0.3) {
                            DamageProtect_StartPerpendicularDodge(client, chargerPos, curTime, 250.0, 1.2);
                            return;
                        }
                    }
                }
            }
        }
    }

    // Detect nearest danger (acid/fire)
    float dangerPos[3];
    int dangerEnt = DamageProtect_FindNearbyDanger(client, dangerPos);
    if (dangerEnt == -1) return;

    DamageProtect_StartDodge(client, dangerPos, curTime, DANGER_DODGE_DISTANCE, DANGER_DODGE_DURATION);
}

/**
 * Shared danger scan: scan all acid pools, fires, and tank rocks ONCE per 0.5s
 * for ALL bots, instead of each bot scanning independently.
 * Called from DamageProtect_SharedSIScan when the SI scan fires.
 */
static void DamageProtect_SharedDangerScan(float curTime) {
    if ((curTime - g_fSharedDangerScanTime) < DANGER_SCAN_SHARED_INTERVAL) return;
    g_fSharedDangerScanTime = curTime;

    // Scan acid pools and fires
    g_iSharedDangerCount = 0;
    int safetyCount;

    // Acid pools (insect_swarm)
    int ent = -1;
    safetyCount = 0;
    while ((ent = FindEntityByClassname(ent, "insect_swarm")) != -1) {
        if (++safetyCount > 64) break;
        if (!IsValidEntity(ent)) { ent++; continue; }
        if (g_iSharedDangerCount >= MAX_SHARED_DANGERS) break;
        g_iSharedDangerEnts[g_iSharedDangerCount] = ent;
        GetEntPropVector(ent, Prop_Send, "m_vecOrigin", g_fSharedDangerPos[g_iSharedDangerCount]);
        g_iSharedDangerCount++;
    }

    // Fire (inferno)
    ent = -1;
    safetyCount = 0;
    while ((ent = FindEntityByClassname(ent, "inferno")) != -1) {
        if (++safetyCount > 64) break;
        if (!IsValidEntity(ent)) { ent++; continue; }
        if (g_iSharedDangerCount >= MAX_SHARED_DANGERS) break;
        g_iSharedDangerEnts[g_iSharedDangerCount] = ent;
        GetEntPropVector(ent, Prop_Send, "m_vecOrigin", g_fSharedDangerPos[g_iSharedDangerCount]);
        g_iSharedDangerCount++;
    }

    // Tank rocks
    g_iSharedRockEnt = -1;
    ent = -1;
    safetyCount = 0;
    while ((ent = FindEntityByClassname(ent, "tank_rock")) != -1) {
        if (++safetyCount > 16) break;
        if (!IsValidEntity(ent)) { ent++; continue; }
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", g_fSharedRockPos);
        // Just cache the first rock found (usually only one at a time)
        g_iSharedRockEnt = ent;
        break;
    }
}

/**
 * Find the nearest danger entity using shared cache.
 * Avoids per-bot FindEntityByClassname loops — just checks distances to cached positions.
 */
static int DamageProtect_FindNearbyDanger(int client, float dangerPos[3]) {
    float pos[3];
    pos = g_BotState[client].absOrigin;

    float closest = DANGER_DETECT_RANGE;
    int found = -1;

    for (int i = 0; i < g_iSharedDangerCount; i++) {
        // Validate entity still exists (acid/fire may have expired between scans)
        if (g_iSharedDangerEnts[i] <= 0 || !IsValidEntity(g_iSharedDangerEnts[i])) continue;
        float dist = GetVectorDistance(pos, g_fSharedDangerPos[i]);
        if (dist < closest) {
            closest = dist;
            found = g_iSharedDangerEnts[i];
            dangerPos = g_fSharedDangerPos[i];
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
static int DamageProtect_FindIncomingTankRock(int client, float detectRange = TANK_ROCK_DETECT_RANGE) {
    // Use shared rock cache first (avoid FindEntityByClassname per bot)
    if (g_iSharedRockEnt != -1 && IsValidEntity(g_iSharedRockEnt)) {
        float pos[3];
        pos = g_BotState[client].absOrigin;
        float dist = AI_GetVectorDistance(pos, g_fSharedRockPos);
        if (dist <= detectRange) {
            // Check if rock is heading toward this bot
            float rockVel[3];
            GetEntPropVector(g_iSharedRockEnt, Prop_Data, "m_vecAbsVelocity", rockVel);
            float rockSpeed = GetVectorLength(rockVel);
            if (rockSpeed >= 50.0) {
                float toBot[3];
                SubtractVectors(pos, g_fSharedRockPos, toBot);
                toBot[2] = 0.0;
                float toBotLen = GetVectorLength(toBot);
                if (toBotLen >= 1.0) {
                    toBot[0] /= toBotLen;
                    toBot[1] /= toBotLen;
                    float normVel[3];
                    normVel[0] = rockVel[0]; normVel[1] = rockVel[1]; normVel[2] = 0.0;
                    float velLen = GetVectorLength(normVel);
                    if (velLen >= 1.0) {
                        normVel[0] /= velLen; normVel[1] /= velLen;
                        float dot = normVel[0] * toBot[0] + normVel[1] * toBot[1];
                        if (dot > 0.5) return g_iSharedRockEnt;
                    }
                }
            }
        }
    }
    return -1;
}

/**
 * Start a perpendicular dodge maneuver — dodge sideways relative to the danger position.
 * This is better than direct away-dodge for linear attacks (Charger, Hunter, Smoker tongue).
 */
static void DamageProtect_StartPerpendicularDodge(int client, const float dangerPos[3], float curTime,
                                                    float dodgeDist, float dodgeDuration) {
    float pos[3];
    pos = g_BotState[client].absOrigin;

    // Direction from danger to bot
    float toBot[3];
    SubtractVectors(pos, dangerPos, toBot);
    toBot[2] = 0.0;
    float len = GetVectorLength(toBot);
    if (len < 1.0) {
        toBot[0] = 1.0; toBot[1] = 0.0;
    } else {
        NormalizeVector(toBot, toBot);
    }

    // Perpendicular direction (rotate 90 degrees)
    float perpDir[3];
    if (GetRandomFloat(0.0, 1.0) < 0.5) {
        perpDir[0] = -toBot[1];
        perpDir[1] = toBot[0];
    } else {
        perpDir[0] = toBot[1];
        perpDir[1] = -toBot[0];
    }
    perpDir[2] = 0.0;

    // Combine perpendicular + slight backward movement
    float escapeDir[3];
    escapeDir[0] = perpDir[0] * 0.7 + toBot[0] * 0.3;
    escapeDir[1] = perpDir[1] * 0.7 + toBot[1] * 0.3;
    escapeDir[2] = 0.0;
    NormalizeVector(escapeDir, escapeDir);
    ScaleVector(escapeDir, dodgeDist);

    float escapeTarget[3];
    AddVectors(pos, escapeDir, escapeTarget);

    g_BotState[client].moveTarget[0] = escapeTarget[0];
    g_BotState[client].moveTarget[1] = escapeTarget[1];
    g_BotState[client].moveTarget[2] = escapeTarget[2];
    g_BotState[client].hasMoveTarget = true;
    g_BotState[client].isDodging = true;
    g_BotState[client].dodgeExpireTime = curTime + dodgeDuration;
}