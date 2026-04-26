/**
 * AI Task: Heal
 * 
 * Priority Order: 2
 * Handles self-healing and teammate healing using first aid kits.
 * 
 * Key safety feature: FL_FROZEN is used to prevent bot movement during heal,
 * and MUST be cleaned up in TaskReset to prevent permanently frozen bots.
 * Reference: VScript ai-heal.nut
 */

#if defined _task_heal_included
    #endinput
#endif
#define _task_heal_included

// FL_FROZEN safety define — may not be available in all include sets
#if !defined FL_FROZEN
    #define FL_FROZEN (1 << 5)
#endif

// Heal constants
#define HEAL_SELF_THRESHOLD       40
#define HEAL_TEAMMATE_THRESHOLD   30
#define HEAL_TEAMMATE_RANGE       200.0
#define HEAL_USE_DURATION         5.0
#define HEAL_APPROACH_RANGE       64.0
#define HEAL_SAFEROOM_THRESHOLD   100
#define HEAL_SAFEROOM_DOOR_RANGE  600.0
#define HEAL_SAFEROOM_CHANGELEVEL_RANGE 400.0

/**
 * Check if the bot is inside a safe room (near checkpoint door or changelevel trigger).
 */
static bool Heal_IsInSafeRoom(int client) {
    float pos[3];
    GetClientAbsOrigin(client, pos);

    // Check for checkpoint doors
    int ent = -1;
    while ((ent = FindEntityByClassname(ent, "prop_door_rotating_checkpoint")) != -1) {
        if (!IsValidEntity(ent)) continue;
        float doorPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", doorPos);
        if (GetVectorDistance(pos, doorPos) < HEAL_SAFEROOM_DOOR_RANGE) return true;
    }

    // Check for changelevel triggers
    ent = -1;
    while ((ent = FindEntityByClassname(ent, "info_changelevel")) != -1) {
        if (!IsValidEntity(ent)) continue;
        float chgPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", chgPos);
        if (GetVectorDistance(pos, chgPos) < HEAL_SAFEROOM_CHANGELEVEL_RANGE) return true;
    }

    return false;
}

/**
 * Check if heal task should activate for this bot.
 */
bool TaskHeal_ShouldUpdate(int client) {
    if (!g_bHealEnabled) return false;
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;

    // Already healing? Continue.
    if (g_BotState[client].isHealing) return true;

    // Don't heal during active rescue
    if (g_BotState[client].isRescuing) return false;

    // Must have a medkit (slot 3)
    int medkit = AI_GetWeaponSlot(client, 3);
    if (medkit == -1) return false;

    // Check medkit is actually a first_aid_kit
    char clsname[64];
    AI_GetWeaponClassname(medkit, clsname, sizeof(clsname));
    if (!StrEqual(clsname, "weapon_first_aid_kit")) return false;

    int myHP = g_BotState[client].health;
    bool onThirdStrike = (GetEntProp(client, Prop_Send, "m_bIsOnThirdStrike") != 0);

    // === Safe room priority healing: lower threshold to HP < 100 ===
    bool inSafeRoom = Heal_IsInSafeRoom(client);
    if (inSafeRoom && myHP < HEAL_SAFEROOM_THRESHOLD && g_BotState[client].visibleThreats == 0) {
        return true;
    }

    // Self-heal: HP <= 40 or on third strike, and no visible threats
    if ((myHP <= HEAL_SELF_THRESHOLD || onThirdStrike) && g_BotState[client].visibleThreats == 0) {
        // Additional safety: no active combat
        if (!g_BotState[client].isInCombat) {
            return true;
        }
    }

    // Teammate heal: find a nearby teammate with HP <= 30 and no threats
    // In safe room: heal any teammate with HP < 100
    int tmHealThreshold = inSafeRoom ? HEAL_SAFEROOM_THRESHOLD : HEAL_TEAMMATE_THRESHOLD;
    if (g_BotState[client].visibleThreats == 0 && !g_BotState[client].isInCombat) {
        for (int i = 1; i <= MaxClients; i++) {
            if (i == client) continue;
            if (!AI_IsValidClient(i) || !IsPlayerAlive(i)) continue;
            if (GetClientTeam(i) != 2) continue;
            if (AI_IsPlayerIncapacitated(i)) continue;

            int tmHP = GetClientHealth(i);
            if (tmHP >= tmHealThreshold) continue;

            float tmPos[3];
            GetClientAbsOrigin(i, tmPos);
            float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, tmPos);
            if (dist <= HEAL_TEAMMATE_RANGE) {
                return true;
            }
        }
    }

    // If currently frozen for heal but ShouldUpdate says no, we need cleanup
    if (g_BotState[client].isFrozenForHeal) {
        TaskHeal_Reset(client);
    }

    return false;
}

/**
 * Execute heal logic: approach target, freeze, use medkit.
 * @return true while healing (lock task).
 */
bool TaskHeal_Update(int client, int &buttons, float vel[3]) {
    float curTime = GetGameTime();

    // === Abort conditions ===
    if (g_BotState[client].controlledByType > 0 ||
        g_BotState[client].isIncapacitated) {
        TaskHeal_Reset(client);
        return false;
    }

    // === Currently healing — maintain ===
    if (g_BotState[client].isHealing) {
        // Check abort: high threat appeared (Tank)
        if (g_BotState[client].visibleThreats > 0) {
            // Check if a tank is visible — abort heal
            for (int i = 1; i <= MaxClients; i++) {
                if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
                if (GetClientTeam(i) != 3) continue;
                if (GetEntProp(i, Prop_Send, "m_zombieClass") == 8) {
                    // Tank is present — abort
                    TaskHeal_Reset(client);
                    return false;
                }
            }
        }

        // Check if heal target is still valid
        int target = g_BotState[client].healTarget;
        if (target != client && target != -1) {
            if (!AI_IsValidClient(target) || !IsPlayerAlive(target)) {
                TaskHeal_Reset(client);
                return false;
            }
        }

        // Check timeout — heal complete
        if (curTime >= g_BotState[client].healEndTime) {
            TaskHeal_Reset(client);
            return false;
        }

        // Keep holding attack button (IN_ATTACK for self, IN_ATTACK2 for teammate)
        if (g_BotState[client].healTarget == client || g_BotState[client].healTarget == -1) {
            buttons |= IN_ATTACK;
        } else {
            buttons |= IN_ATTACK2;
        }

        // Stop all movement during heal
        AI_ClearMoveVelocity(vel);

        return true;
    }

    // === Not yet healing — select target and start ===

    // Find best heal target
    int healTarget = -1;
    int lowestHP = 999;
    bool inSafeRoom = Heal_IsInSafeRoom(client);
    int tmHealThreshold = inSafeRoom ? HEAL_SAFEROOM_THRESHOLD : HEAL_TEAMMATE_THRESHOLD;

    // Check teammates first
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!AI_IsValidClient(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (AI_IsPlayerIncapacitated(i)) continue;

        int tmHP = GetClientHealth(i);
        if (tmHP >= tmHealThreshold) continue;

        float tmPos[3];
        GetClientAbsOrigin(i, tmPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, tmPos);
        if (dist > HEAL_TEAMMATE_RANGE) continue;

        if (tmHP < lowestHP) {
            lowestHP = tmHP;
            healTarget = i;
        }
    }

    // If no teammate needs healing, heal self
    if (healTarget == -1) {
        int myHP = g_BotState[client].health;
        bool onThirdStrike = (GetEntProp(client, Prop_Send, "m_bIsOnThirdStrike") != 0);
        int selfThreshold = inSafeRoom ? HEAL_SAFEROOM_THRESHOLD : HEAL_SELF_THRESHOLD;
        if (myHP < selfThreshold || onThirdStrike) {
            healTarget = client;
        }
    }

    if (healTarget == -1) {
        return false;
    }

    // === Need to approach teammate? ===
    if (healTarget != client) {
        float targetPos[3];
        GetClientAbsOrigin(healTarget, targetPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, targetPos);

        if (dist > HEAL_APPROACH_RANGE) {
            // Move toward teammate
            AI_ComputeMoveToward(client, targetPos, g_BotState[client].absOrigin,
                                  g_BotState[client].eyeAngles, vel);
            g_BotState[client].healTarget = healTarget;
            return true; // Lock while approaching
        }

        // Close enough — look at teammate
        targetPos[2] += 40.0;
        AI_SnapViewToPosition(client, targetPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
    }

    // === Start healing ===
    // Switch to medkit (slot 3)
    int medkit = AI_GetWeaponSlot(client, 3);
    if (medkit == -1) {
        return false;
    }

    // Check if medkit is active weapon
    int activeWeapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
    if (activeWeapon != medkit) {
        // Force weapon switch by pressing slot key
        // In SourceMod, we use FakeClientCommand to switch
        FakeClientCommand(client, "use weapon_first_aid_kit");
        return true; // Wait for switch
    }

    // Freeze the bot to prevent movement during heal
    int flags = GetEntityFlags(client);
    if (((flags & FL_FROZEN) == 0)) {
        SetEntityFlags(client, flags | FL_FROZEN);
    }

    // Start the heal
    g_BotState[client].isHealing = true;
    g_BotState[client].healTarget = healTarget;
    g_BotState[client].healEndTime = curTime + HEAL_USE_DURATION;
    g_BotState[client].isFrozenForHeal = true;

    // Press attack to use medkit (IN_ATTACK for self, IN_ATTACK2 for teammate)
    if (healTarget == client) {
        buttons |= IN_ATTACK;
    } else {
        buttons |= IN_ATTACK2;
    }

    // Stop all movement
    AI_ClearMoveVelocity(vel);

    return true;
}

/**
 * Reset heal state. CRITICAL: Must remove FL_FROZEN to prevent permanently frozen bots!
 */
void TaskHeal_Reset(int client) {
    // Remove frozen flag FIRST — most important cleanup!
    if (g_BotState[client].isFrozenForHeal) {
        int flags = GetEntityFlags(client);
        if (((flags & FL_FROZEN) != 0)) {
            SetEntityFlags(client, flags & ~FL_FROZEN);
        }
        g_BotState[client].isFrozenForHeal = false;
    }

    g_BotState[client].isHealing = false;
    g_BotState[client].healTarget = -1;
    g_BotState[client].healEndTime = 0.0;
}
