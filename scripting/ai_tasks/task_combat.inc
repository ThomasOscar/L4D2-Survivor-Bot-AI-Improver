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

// Witch crown constants
#define WITCH_CROWN_RANGE_SHOTGUN   150.0   // Max range for shotgun crown
#define WITCH_CROWN_RANGE_MELEE     80.0    // Max range for melee crown
#define WITCH_CROWN_RANGE_APPROACH  500.0   // Start approaching witch for crown

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
    int safetyCount;
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
    safetyCount = 0;
    while ((witch = FindEntityByClassname(witch, "witch")) != -1) {
        if (++safetyCount > 256) break;
        if (!IsValidEntity(witch)) { witch++; continue; }

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
    safetyCount = 0;
    while ((common = FindEntityByClassname(common, "infected")) != -1) {
        if (++safetyCount > 256) break;
        if (!IsValidEntity(common)) { common++; continue; }

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
    int safetyCount;

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

    // === Pass 2: Scan witches ===
    // Targeting priority: tank(3) > SI(2) > witch(4) > common(1)
    if (bestType != 3 && bestType != 2) {
        int witch = -1;
        safetyCount = 0;
        while ((witch = FindEntityByClassname(witch, "witch")) != -1) {
            if (++safetyCount > 256) break;
            if (!IsValidEntity(witch)) { witch++; continue; }

            float witchPos[3];
            GetEntPropVector(witch, Prop_Data, "m_vecAbsOrigin", witchPos);
            float dist = AI_GetVectorDistance(g_BotState[client].eyePos, witchPos);
            if (dist > COMBAT_MAX_RANGE) continue;

            int seq = GetEntProp(witch, Prop_Send, "m_nSequence");
            bool isAngry = (seq >= WITCH_ANIM_RAGE_START);

            // Angry witches: always target
            // Calm witches: target only in aggressive leader mode if blocking path
            if (!isAngry) {
                if (!TaskNavigate_IsAggressiveLeaderMode()) continue;
                // Check if witch is reasonably close (potential blocker)
                if (dist > WITCH_CROWN_RANGE_APPROACH) continue;
            }

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
        safetyCount = 0;
        while ((common = FindEntityByClassname(common, "infected")) != -1) {
            if (++safetyCount > 256) break;
            if (!IsValidEntity(common)) { common++; continue; }

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

    // === Cover Priority: protect busy teammates ===
    // If a teammate is doing a critical task (rescue/defib/hold-USE),
    // prefer targeting enemies near that teammate to provide cover fire.
    int busyClient = -1;
    float busyPos[3];
    if (AI_IsTeammateBusyWithCriticalTask(busyClient, busyPos) && busyClient != client) {
        int coverTarget = -1;
        int coverType = 0;
        float coverDistToTeammate = 600.0;

        for (int ci = 1; ci <= MaxClients; ci++) {
            if (ci == client) continue;
            if (!IsClientInGame(ci) || !IsPlayerAlive(ci) || GetClientTeam(ci) != 3) continue;
            if (GetEntProp(ci, Prop_Send, "m_isGhost") != 0) continue;
            float ctPos[3];
            GetClientAbsOrigin(ci, ctPos);
            float distToBusy = AI_GetVectorDistance(busyPos, ctPos);
            if (distToBusy > 600.0) continue;
            if (!AI_IsVisibleEntity(g_BotState[client].eyePos, ci, MASK_SHOT, client)) continue;

            int czClass = GetEntProp(ci, Prop_Send, "m_zombieClass");
            int cType = (czClass == 8) ? 3 : 2;

            if (cType > coverType || (cType == coverType && distToBusy < coverDistToTeammate)) {
                coverTarget = ci;
                coverType = cType;
                coverDistToTeammate = distToBusy;
            }
        }

        // Use cover target if same priority tier or higher than normal best
        if (coverTarget != -1 && coverType >= bestType) {
            bestTarget = coverTarget;
            bestType = coverType;
            float ctPos[3];
            GetClientAbsOrigin(coverTarget, ctPos);
            bestDist = AI_GetVectorDistance(g_BotState[client].eyePos, ctPos);
            threatCount++;  // Count this as a visible threat
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

    // === Witch Crown technique (high-skill maneuver) ===
    // When targeting a witch at close range, use specialized attack patterns:
    //   - Melee weapon: approach and slash rapidly (knife/melee crown)
    //   - Shotgun: aim at head, fire point-blank (shotgun crown)
    //   - Other: approach as close as possible, then fire
    if (bestType == 4 && bestDist < WITCH_CROWN_RANGE_APPROACH) {
        int level = g_iBotSkillLevel;
        if (level < SKILL_LEVEL_MIN) level = SKILL_LEVEL_MIN;
        if (level > SKILL_LEVEL_MAX) level = SKILL_LEVEL_MAX;

        // Only attempt crown at skill level 2+
        if (level >= 2) {
            float witchPos[3];
            GetEntPropVector(bestTarget, Prop_Data, "m_vecAbsOrigin", witchPos);

            // Check if we have a melee weapon
            bool hasMelee = false;
            int secondary = GetPlayerWeaponSlot(client, 1);
            if (secondary != -1 && IsValidEntity(secondary)) {
                char secCls[64];
                GetEntityClassname(secondary, secCls, sizeof(secCls));
                if (StrEqual(secCls, "weapon_melee")) {
                    hasMelee = true;
                }
            }

            // Check if we have a shotgun
            bool hasShotgun = false;
            int primary = GetPlayerWeaponSlot(client, 0);
            if (primary != -1 && IsValidEntity(primary)) {
                char priCls[64];
                GetEntityClassname(primary, priCls, sizeof(priCls));
                if (StrContains(priCls, "shotgun") != -1) {
                    hasShotgun = true;
                }
            }

            if (hasMelee && bestDist <= WITCH_CROWN_RANGE_MELEE) {
                // Melee crown: switch to melee, aim at witch head, slash
                AI_DebugDecision(client, "Witch melee crown %.0fu", bestDist);
                int activeWep = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
                if (activeWep != secondary) {
                    ClientCommand(client, "slot2");
                }
                // Aim at witch head level
                witchPos[2] += 50.0;
                AI_SnapViewToPosition(client, witchPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                // Slash rapidly
                AI_PressAttackButton(client, buttons);
                AI_PressAttackButton(client, buttons);

                g_BotState[client].targetEntity = bestTarget;
                g_BotState[client].targetType = bestType;
                g_BotState[client].isInCombat = true;
                g_BotState[client].visibleThreats = threatCount;
                g_BotState[client].visibleCommons = commonCount;
                return true;  // Lock for witch crown
            } else if (hasShotgun && bestDist <= WITCH_CROWN_RANGE_SHOTGUN) {
                // Shotgun crown: switch to shotgun, aim at head, fire
                AI_DebugDecision(client, "Witch shotgun crown %.0fu", bestDist);
                int activeWep = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
                if (activeWep != primary) {
                    ClientCommand(client, "slot1");
                }
                // Aim at witch head for max damage
                witchPos[2] += 50.0;
                AI_SnapViewToPosition(client, witchPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);

                if (reactionReady) {
                    int weapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
                    if (IsValidEntity(weapon)) {
                        int clip = GetEntProp(weapon, Prop_Send, "m_iClip1");
                        if (clip > 0) {
                            AI_PressAttackButton(client, buttons);
                            // For pump shotguns: shove immediately after firing (cancel animation)
                            char wepCls[64];
                            GetEntityClassname(weapon, wepCls, sizeof(wepCls));
                            if (StrEqual(wepCls, "weapon_pumpshotgun") || StrEqual(wepCls, "weapon_shotgun_chrome")) {
                                AI_PressShoveButton(client, buttons);
                            }
                        } else {
                            buttons |= IN_RELOAD;
                        }
                    }
                }

                g_BotState[client].targetEntity = bestTarget;
                g_BotState[client].targetType = bestType;
                g_BotState[client].isInCombat = true;
                g_BotState[client].visibleThreats = threatCount;
                g_BotState[client].visibleCommons = commonCount;
                return true;  // Lock for witch crown
            } else if (bestDist > WITCH_CROWN_RANGE_MELEE || bestDist > WITCH_CROWN_RANGE_SHOTGUN) {
                // Not close enough for crown yet — approach the witch
                AI_ComputeMoveToward(client, witchPos, g_BotState[client].absOrigin,
                                      g_BotState[client].eyeAngles, vel);
                // Look at witch while approaching
                witchPos[2] += 40.0;
                AI_SnapViewToPosition(client, witchPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);

                g_BotState[client].targetEntity = bestTarget;
                g_BotState[client].targetType = bestType;
                g_BotState[client].isInCombat = true;
                g_BotState[client].visibleThreats = threatCount;
                g_BotState[client].visibleCommons = commonCount;
                return true;  // Lock while approaching witch
            }
        }
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

    // Debug: set combat decision reason
    if (g_bDebugMode) {
        char targetName[32];
        switch (bestType) {
            case 1: FormatEx(targetName, sizeof(targetName), "Common");
            case 2: {
                if (bestTarget > 0 && bestTarget <= MaxClients) {
                    char siName[16];
                    int zClass = GetEntProp(bestTarget, Prop_Send, "m_zombieClass");
                    switch (zClass) {
                        case 1: strcopy(siName, sizeof(siName), "Smoker");
                        case 2: strcopy(siName, sizeof(siName), "Boomer");
                        case 3: strcopy(siName, sizeof(siName), "Hunter");
                        case 4: strcopy(siName, sizeof(siName), "Spitter");
                        case 5: strcopy(siName, sizeof(siName), "Jockey");
                        case 6: strcopy(siName, sizeof(siName), "Charger");
                        default: strcopy(siName, sizeof(siName), "SI");
                    }
                    FormatEx(targetName, sizeof(targetName), "%s", siName);
                } else strcopy(targetName, sizeof(targetName), "SI");
            }
            case 3: strcopy(targetName, sizeof(targetName), "Tank");
            case 4: strcopy(targetName, sizeof(targetName), "Witch");
            default: strcopy(targetName, sizeof(targetName), "?");
        }
        FormatEx(g_BotState[client].taskSwitchReason, sizeof(BotState::taskSwitchReason),
            "%s %du", targetName, RoundToNearest(bestDist));
    }
    g_BotState[client].isInCombat = true;
    g_BotState[client].visibleThreats = threatCount;
    g_BotState[client].visibleCommons = commonCount;

    // Lock task for Tank, SI, or Witch in aggressive leader mode
    bool lockForWitch = (bestType == 4 && TaskNavigate_IsAggressiveLeaderMode());
    return (bestType == 2 || bestType == 3 || lockForWitch);
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
