/**
 * AI Task: Combat
 * 
 * Priority Order: 1
 * Handles target acquisition and engagement for all threat types.
 * 
 * Target priority (high to low):
 *   1. Tank (zombie type 8)
 *   2. Active special infected (non-ghost, team 3)
 *   3. Angry Witch
 *   4. Common infected
 *
 * Skill-based enhancements:
 *   - Dynamic shove range: 80.0 + (skill_level * 10.0)
 *   - Shove success rate: SkillRollSuccess(shoveSuccessRate)
 *   - Aim deviation: random offset based on aimAccuracy (degrees)
 *   - Reaction delay: delay before firing after first spotting a target
 *   - Witch bonus damage: Lv3+ gets extra damage vs angry Witch
 */

#if defined _task_combat_included
    #endinput
#endif
#define _task_combat_included

// Witch animation sequences for rage detection
#define WITCH_ANIM_RAGE_START   30
#define WITCH_ANIM_RAGE_LOOP    31

// Combat range constants
#define COMBAT_SHOVE_RANGE_BASE 80.0
#define COMBAT_SHOVE_RANGE_PER_LEVEL 10.0
#define COMBAT_MAX_RANGE        2000.0
#define COMBAT_COMMON_RANGE     800.0

// Witch bonus damage multiplier for Lv3+ with shotgun
#define WITCH_SHOTGUN_BONUS_LV3 1.5
#define WITCH_SHOTGUN_BONUS_LV4 2.0

/**
 * Calculate the dynamic shove range for the current skill level.
 * Lv0=80, Lv1=90, Lv2=100, Lv3=110, Lv4=120
 */
static float Combat_GetShoveRange() {
    int level = g_iBotSkillLevel;
    if (level < SKILL_LEVEL_MIN) level = SKILL_LEVEL_MIN;
    if (level > SKILL_LEVEL_MAX) level = SKILL_LEVEL_MAX;
    return COMBAT_SHOVE_RANGE_BASE + (float(level) * COMBAT_SHOVE_RANGE_PER_LEVEL);
}

/**
 * Check if combat task should activate for this bot.
 */
bool TaskCombat_ShouldUpdate(int client) {
    // Bot must be free and standing
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;

    // Quick scan: are there any visible threats?
    // We do the full scan in Update, but do a lightweight check here
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i)) continue;
        if (!IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 3) continue;

        // Skip ghosts
        if (GetEntProp(i, Prop_Send, "m_isGhost") != 0) continue;

        float targetPos[3];
        GetClientAbsOrigin(i, targetPos);
        float dist = AI_GetVectorDistance(g_BotState[client].eyePos, targetPos);
        if (dist > COMBAT_MAX_RANGE) continue;

        if (AI_IsVisibleEntity(g_BotState[client].eyePos, i, MASK_SHOT, client)) {
            return true;
        }
    }

    // Check for angry witches
    int witch = -1;
    while ((witch = FindEntityByClassname(witch, "witch")) != -1) {
        if (!IsValidEntity(witch)) continue;

        float witchPos[3];
        GetEntPropVector(witch, Prop_Data, "m_vecAbsOrigin", witchPos);
        float dist = AI_GetVectorDistance(g_BotState[client].eyePos, witchPos);
        if (dist > COMBAT_MAX_RANGE) continue;

        // Check if witch is angry (rage animation)
        int seq = GetEntProp(witch, Prop_Send, "m_nSequence");
        if (seq >= WITCH_ANIM_RAGE_START) {
            if (AI_IsVisiblePosition(g_BotState[client].eyePos, witchPos, MASK_SHOT, client)) {
                return true;
            }
        }
    }

    // Check for common infected
    int common = -1;
    while ((common = FindEntityByClassname(common, "infected")) != -1) {
        if (!IsValidEntity(common)) continue;

        float commonPos[3];
        GetEntPropVector(common, Prop_Data, "m_vecAbsOrigin", commonPos);
        float dist = AI_GetVectorDistance(g_BotState[client].eyePos, commonPos);
        if (dist > COMBAT_COMMON_RANGE) continue;

        if (AI_IsVisiblePosition(g_BotState[client].eyePos, commonPos, MASK_SHOT, client)) {
            return true;
        }
    }

    return false;
}

/**
 * Execute combat logic: target selection, aiming, firing.
 * Incorporates skill-based: reaction delay, aim deviation, shove roll, dynamic shove range.
 * @return true if engaging Tank or SI (lock task), false for commons.
 */
bool TaskCombat_Update(int client, int &buttons, float vel[3]) {
    // vel is available for future use (e.g., strafing while shooting)
    #pragma unused vel

    int bestTarget = -1;
    int bestType = 0;       // 0=none, 1=common, 2=SI, 3=tank, 4=witch
    float bestDist = COMBAT_MAX_RANGE + 1.0;
    int threatCount = 0;
    int commonCount = 0;

    // === Pass 1: Scan special infected (team 3) ===
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i)) continue;
        if (!IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 3) continue;
        if (GetEntProp(i, Prop_Send, "m_isGhost") != 0) continue;

        float targetPos[3];
        GetClientAbsOrigin(i, targetPos);
        float dist = AI_GetVectorDistance(g_BotState[client].eyePos, targetPos);
        if (dist > COMBAT_MAX_RANGE) continue;

        if (!AI_IsVisibleEntity(g_BotState[client].eyePos, i, MASK_SHOT, client)) continue;

        threatCount++;

        // Determine if this is a Tank (zombie type 8)
        int zombieClass = GetEntProp(i, Prop_Send, "m_zombieClass");
        bool isTank = (zombieClass == 8); // ZC_TANK = 8 in L4D2
        int thisType = isTank ? 3 : 2;

        // Priority: Tank > SI. Within same priority, pick closest.
        if (thisType > bestType || (thisType == bestType && dist < bestDist)) {
            bestTarget = i;
            bestType = thisType;
            bestDist = dist;
        }
    }

    // === Pass 2: Scan angry witches (only if no Tank or SI found) ===
    // Targeting priority: tank(3) > SI(2) > witch(4) > common(1)
    if (bestType != 3 && bestType != 2) {
        int witch = -1;
        while ((witch = FindEntityByClassname(witch, "witch")) != -1) {
            if (!IsValidEntity(witch)) continue;

            float witchPos[3];
            GetEntPropVector(witch, Prop_Data, "m_vecAbsOrigin", witchPos);
            float dist = AI_GetVectorDistance(g_BotState[client].eyePos, witchPos);
            if (dist > COMBAT_MAX_RANGE) continue;

            int seq = GetEntProp(witch, Prop_Send, "m_nSequence");
            if (seq < WITCH_ANIM_RAGE_START) continue;

            if (!AI_IsVisiblePosition(g_BotState[client].eyePos, witchPos, MASK_SHOT, client)) continue;

            if (dist < bestDist || bestTarget == -1) {
                bestTarget = witch;
                bestType = 4; // witch
                bestDist = dist;
            }
        }
    }

    // === Pass 3: Scan common infected (only if nothing better found) ===
    if (bestTarget == -1) {
        int common = -1;
        while ((common = FindEntityByClassname(common, "infected")) != -1) {
            if (!IsValidEntity(common)) continue;

            float commonPos[3];
            GetEntPropVector(common, Prop_Data, "m_vecAbsOrigin", commonPos);
            float dist = AI_GetVectorDistance(g_BotState[client].eyePos, commonPos);
            if (dist > COMBAT_COMMON_RANGE) continue;

            if (!AI_IsVisiblePosition(g_BotState[client].eyePos, commonPos, MASK_SHOT, client)) continue;

            commonCount++;

            if (dist < bestDist) {
                bestTarget = common;
                bestType = 1; // common
                bestDist = dist;
            }
        }
    }

    // No target found
    if (bestTarget == -1) {
        TaskCombat_Reset(client);
        return false;
    }

    // === Reaction Delay: track first sight time ===
    float curTime = GetGameTime();
    if (g_BotState[client].lastCombatTarget != bestTarget) {
        // New target spotted — record first sight time
        g_BotState[client].combatFirstSightTime = curTime;
        g_BotState[client].lastCombatTarget = bestTarget;
    }

    float reactionDelay = GetSkillFloat("reaction");
    float timeSinceSight = curTime - g_BotState[client].combatFirstSightTime;
    bool reactionReady = (timeSinceSight >= reactionDelay);

    // === Determine target position ===
    float targetPos[3];
    if (bestTarget <= MaxClients) {
        GetClientAbsOrigin(bestTarget, targetPos);
        // Aim at center mass (slightly above origin)
        targetPos[2] += 40.0;
    } else {
        GetEntPropVector(bestTarget, Prop_Data, "m_vecAbsOrigin", targetPos);
        targetPos[2] += 30.0;
    }

    // === Aim at target with skill-based deviation ===
    float aimDeviation = GetSkillFloat("aim");
    if (aimDeviation > 0.0) {
        // Add random angular offset to simulate imperfect aim
        float offsetX = GetRandomFloat(-aimDeviation, aimDeviation);
        float offsetY = GetRandomFloat(-aimDeviation, aimDeviation);
        targetPos[0] += offsetX;
        targetPos[1] += offsetY;
    }

    AI_SnapViewToPosition(client, targetPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);

    // === Fire only if reaction delay has passed ===
    if (reactionReady) {
        // Auto-reload check
        int weapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
        if (IsValidEntity(weapon)) {
            int clip = GetEntProp(weapon, Prop_Send, "m_iClip1");
            if (clip <= 0) {
                buttons |= IN_RELOAD;
            } else {
                // Fire!
                AI_PressAttackButton(client, buttons);
            }
        } else {
            AI_PressAttackButton(client, buttons);
        }
    }
    // If reaction not ready, bot aims but does not fire (simulating delay)

    // === Shove with skill-based success rate and dynamic range ===
    float shoveRange = Combat_GetShoveRange();
    if (bestDist < shoveRange && (bestType == 2 || bestType == 3 || bestType == 1)) {
        // Roll for shove success based on skill level
        float shoveRate = GetSkillFloat("shove");
        if (SkillRollSuccess(shoveRate)) {
            AI_PressShoveButton(client, buttons);
        }
    }

    // Update BotState
    g_BotState[client].targetEntity = bestTarget;
    g_BotState[client].targetType = bestType;
    g_BotState[client].isInCombat = true;
    g_BotState[client].visibleThreats = threatCount;
    g_BotState[client].visibleCommons = commonCount;

    // Lock task for Tank or SI, not for commons
    return (bestType == 2 || bestType == 3);
}

/**
 * Reset combat state.
 */
void TaskCombat_Reset(int client) {
    g_BotState[client].targetEntity = -1;
    g_BotState[client].targetType = 0;
    g_BotState[client].isInCombat = false;
    g_BotState[client].visibleThreats = 0;
    g_BotState[client].visibleCommons = 0;
    g_BotState[client].combatFirstSightTime = 0.0;
    g_BotState[client].lastCombatTarget = -1;
}

// ============================================================
// SDKHook Damage Callback — called from main entry point
// Handles: damage multiplier (shooting compensation) + Witch bonus
// ============================================================

/**
 * SDKHook_OnTakeDamageAlive callback.
 * Registered per-entity in OnClientPutInServer (for clients)
 * and via witch entity hooks.
 *
 * When attacker is a Survivor Bot:
 *   - Apply damageMultiplier from skill config (Lv3: +10%, Lv4: +20%)
 * When victim is an angry Witch and attacker is Lv3+ with shotgun:
 *   - Apply additional Witch shotgun bonus
 */
Action Combat_OnTakeDamageAlive(int victim, int &attacker, int &inflictor, float &damage, int &damagetype) {
    // Validate attacker is a valid survivor bot
    if (attacker < 1 || attacker > MaxClients) return Plugin_Continue;
    if (!IsClientInGame(attacker)) return Plugin_Continue;
    if (!IsFakeClient(attacker)) return Plugin_Continue;
    if (GetClientTeam(attacker) != 2) return Plugin_Continue;

    bool modified = false;

    // === 1. General damage multiplier (shooting compensation) ===
    float dmgMult = GetSkillFloat("damage");
    if (dmgMult > 0.0) {
        damage *= (1.0 + dmgMult);
        modified = true;
    }

    // === 2. Witch bonus damage ===
    // Check if victim is a witch entity
    if (victim > MaxClients && IsValidEntity(victim)) {
        char classname[32];
        GetEntityClassname(victim, classname, sizeof(classname));

        if (StrEqual(classname, "witch")) {
            // Check if witch is angry
            int seq = GetEntProp(victim, Prop_Send, "m_nSequence");
            if (seq >= WITCH_ANIM_RAGE_START) {
                int level = g_iBotSkillLevel;
                if (level < SKILL_LEVEL_MIN) level = SKILL_LEVEL_MIN;
                if (level > SKILL_LEVEL_MAX) level = SKILL_LEVEL_MAX;

                // Lv3+ with shotgun gets bonus
                if (level >= 3) {
                    // Check if attacker is using a shotgun
                    int weapon = GetEntPropEnt(attacker, Prop_Send, "m_hActiveWeapon");
                    if (IsValidEntity(weapon)) {
                        char weaponClass[64];
                        GetEntityClassname(weapon, weaponClass, sizeof(weaponClass));

                        if (StrContains(weaponClass, "shotgun") != -1) {
                            float witchBonus = (level >= 4) ? WITCH_SHOTGUN_BONUS_LV4 : WITCH_SHOTGUN_BONUS_LV3;
                            damage *= witchBonus;
                            modified = true;
                        }
                    }
                }
            }
        }
    }

    // === 3. Apply enemy-type damage multiplier (ConVar controlled) ===
    if (victim > MaxClients && IsValidEntity(victim))
    {
        char victimClass[32];
        GetEntityClassname(victim, victimClass, sizeof(victimClass));
        if (StrEqual(victimClass, "witch"))
        {
            damage *= g_fWitchDamage;
            modified = true;
        }
    }
    else if (victim > 0 && victim <= MaxClients && IsClientInGame(victim) && GetClientTeam(victim) == 3)
    {
        int zClass = GetEntProp(victim, Prop_Send, "m_zombieClass");
        if (zClass == 8) // ZC_TANK = 8 in L4D2
        {
            damage *= g_fTankDamage;
        }
        else // Hunter/Smoker/Boomer/Spitter/Jockey/Charger
        {
            damage *= g_fSpecialDamage;
        }
        modified = true;
    }

    if (modified) {
        return Plugin_Changed;
    }

    return Plugin_Continue;
}
