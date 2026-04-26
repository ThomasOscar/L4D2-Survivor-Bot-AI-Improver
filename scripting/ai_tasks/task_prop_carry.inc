/**
 * AI Task: Prop Carry
 * 
 * Priority Order: 4
 * Picks up gas cans, propane tanks, and oxygen tanks.
 * Throws them at Tank or large zombie groups.
 * 
 * Based on proven Phase52_PropCarryThrow logic from the old plugin.
 */

#if defined _task_prop_carry_included
    #endinput
#endif
#define _task_prop_carry_included

// Prop carry constants
#define PROP_SCAN_RANGE         400.0
#define PROP_PICKUP_RANGE       90.0
#define PROP_THROW_RANGE        600.0
#define PROP_THROW_MIN_RANGE    100.0
#define PROP_THROW_COOLDOWN     5.0
#define PROP_SCAN_INTERVAL      1.0
#define PROP_HORDE_SCAN_RANGE   400.0
#define PROP_HORDE_MIN_COUNT    8
#define PROP_POUR_RANGE         100.0  // Distance to pour gas into generator

// Per-bot prop carry state
static int g_iPropTarget[MAXPLAYERS + 1];
static float g_fPropThrowTime[MAXPLAYERS + 1];
static float g_fPropScanTime[MAXPLAYERS + 1];
static bool g_bPropCarrying[MAXPLAYERS + 1];

// Global pour/generator target state
static float g_fPourTargetPos[3];     // Generator/pour target position
static bool g_bHasPourTarget;          // Whether current map has a pour target
static int g_iPourTargetEntity;        // Pour target entity index

/**
 * Initialize pour target detection on map start.
 * Call this from OnMapStart in the main plugin.
 */
void PropCarry_OnMapStart() {
    g_bHasPourTarget = false;
    g_iPourTargetEntity = -1;
    g_fPourTargetPos[0] = 0.0;
    g_fPourTargetPos[1] = 0.0;
    g_fPourTargetPos[2] = 0.0;

    char model[128];

    // Search for generator-type prop_dynamic entities
    int ent = -1;
    while ((ent = FindEntityByClassname(ent, "prop_dynamic")) != -1) {
        if (!IsValidEntity(ent)) continue;
        GetEntPropString(ent, Prop_Data, "m_ModelName", model, sizeof(model));
        if (StrContains(model, "generator") != -1 || StrContains(model, "dvd_player") != -1) {
            GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", g_fPourTargetPos);
            g_bHasPourTarget = true;
            g_iPourTargetEntity = ent;
            LogMessage("[AI Bot] PropCarry: Found generator at %.0f, %.0f, %.0f (ent %d)",
                       g_fPourTargetPos[0], g_fPourTargetPos[1], g_fPourTargetPos[2], ent);
            break;
        }
    }

    // Also search for point_prop_use_target (gascan pour accept point)
    if (!g_bHasPourTarget) {
        ent = -1;
        while ((ent = FindEntityByClassname(ent, "point_prop_use_target")) != -1) {
            if (!IsValidEntity(ent)) continue;
            GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", g_fPourTargetPos);
            g_bHasPourTarget = true;
            g_iPourTargetEntity = ent;
            LogMessage("[AI Bot] PropCarry: Found pour target at %.0f, %.0f, %.0f (ent %d)",
                       g_fPourTargetPos[0], g_fPourTargetPos[1], g_fPourTargetPos[2], ent);
            break;
        }
    }
}

/**
 * Check if prop carry task should activate for this bot.
 */
bool TaskPropCarry_ShouldUpdate(int client) {
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;

    // If carrying, always update (to check throw opportunities)
    if (TaskPropCarry_IsCarrying(client)) {
        g_bPropCarrying[client] = true;
        return true;
    }
    g_bPropCarrying[client] = false;

    // Don't scan while rescuing or healing
    if (g_BotState[client].isRescuing) return false;
    if (g_BotState[client].isHealing) return false;

    // Don't pick up during heavy combat
    if (g_BotState[client].isInCombat && g_BotState[client].targetType >= 2) return false;

    // Throttle scanning
    float curTime = GetGameTime();
    if ((curTime - g_fPropScanTime[client]) < PROP_SCAN_INTERVAL) {
        return (g_iPropTarget[client] > 0 && IsValidEntity(g_iPropTarget[client]));
    }
    g_fPropScanTime[client] = curTime;

    // Scan for nearby props
    int prop = TaskPropCarry_FindNearestProp(client);
    if (prop != -1) {
        g_iPropTarget[client] = prop;
        return true;
    }

    g_iPropTarget[client] = -1;
    return false;
}

/**
 * Execute prop carry logic: pickup or throw.
 * @return true while picking up or throwing (lock task).
 */
bool TaskPropCarry_Update(int client, int &buttons, float vel[3]) {
    float curTime = GetGameTime();

    // === Carrying mode: look for throw or pour opportunities ===
    if (g_bPropCarrying[client]) {
        // Check throw cooldown
        if (curTime < g_fPropThrowTime[client]) {
            return false;
        }

        float botPos[3];
        GetClientAbsOrigin(client, botPos);

        // Priority 0: Pour gascan into generator (when enabled and carrying gascan)
        if (g_bHasPourTarget && g_bFindGasEnabled && TaskPropCarry_IsCarryingGascan(client)) {
            if (g_iPourTargetEntity > 0 && IsValidEntity(g_iPourTargetEntity)) {
                float pourDist = AI_GetVectorDistance(botPos, g_fPourTargetPos);
                if (pourDist <= PROP_POUR_RANGE) {
                    // Close enough — look at generator and use/attack to pour
                    float lookPos[3];
                    lookPos[0] = g_fPourTargetPos[0];
                    lookPos[1] = g_fPourTargetPos[1];
                    lookPos[2] = g_fPourTargetPos[2] + 20.0;
                    AI_SnapViewToPosition(client, lookPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                    AI_PressUseButton(client, buttons);
                    return true;
                } else {
                    // Move toward generator
                    AI_ComputeMoveToward(client, g_fPourTargetPos, botPos,
                                          g_BotState[client].eyeAngles, vel);
                    return true;  // Lock while delivering
                }
            }
        }

        // Priority 1: Throw at Tank
        for (int i = 1; i <= MaxClients; i++) {
            if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
            if (GetClientTeam(i) != 3) continue;
            if (GetEntProp(i, Prop_Send, "m_zombieClass") != 8) continue;

            float tankPos[3];
            GetClientAbsOrigin(i, tankPos);
            float dist = AI_GetVectorDistance(botPos, tankPos);

            if (dist <= PROP_THROW_RANGE && dist >= PROP_THROW_MIN_RANGE) {
                if (AI_IsVisibleEntity(g_BotState[client].eyePos, i, MASK_SHOT, client)) {
                    tankPos[2] += 40.0;
                    AI_SnapViewToPosition(client, tankPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                    buttons |= IN_ATTACK;
                    g_fPropThrowTime[client] = curTime + PROP_THROW_COOLDOWN;
                    return true;
                }
            }
        }

        // Priority 2: Throw at large zombie group
        float centerPos[3];
        int hordeCount = TaskPropCarry_FindHordeCenter(client, centerPos);
        if (hordeCount >= 5) {
            AI_SnapViewToPosition(client, centerPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
            buttons |= IN_ATTACK;
            g_fPropThrowTime[client] = curTime + PROP_THROW_COOLDOWN;
            return true;
        }

        // If carrying gascan and generator exists, keep moving toward it
        if (g_bHasPourTarget && g_bFindGasEnabled && TaskPropCarry_IsCarryingGascan(client)) {
            AI_ComputeMoveToward(client, g_fPourTargetPos, botPos,
                                  g_BotState[client].eyeAngles, vel);
            return false;  // Don't lock, allow combat to override
        }

        return false;  // Carrying but no good throw target
    }

    // === Pickup mode ===
    int target = g_iPropTarget[client];
    if (target <= 0 || !IsValidEntity(target)) {
        TaskPropCarry_Reset(client);
        return false;
    }

    // Check if someone else took it
    int owner = GetEntPropEnt(target, Prop_Data, "m_hOwnerEntity");
    if (owner > 0 && owner <= MaxClients) {
        TaskPropCarry_Reset(client);
        return false;
    }

    float propPos[3];
    GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", propPos);
    float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, propPos);

    if (dist <= PROP_PICKUP_RANGE) {
        // Close enough — pick up
        propPos[2] += 10.0;
        AI_SnapViewToPosition(client, propPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
        AI_PressUseButton(client, buttons);
        return true;  // Lock while picking up
    }

    // Move toward prop
    AI_ComputeMoveToward(client, propPos, g_BotState[client].absOrigin,
                          g_BotState[client].eyeAngles, vel);
    return false;  // Don't lock while walking toward
}

/**
 * Reset prop carry state.
 */
void TaskPropCarry_Reset(int client) {
    g_iPropTarget[client] = -1;
    g_bPropCarrying[client] = false;
}

// ============================================================
// Internal helpers
// ============================================================

/**
 * Check if the bot is currently carrying a prop (gascan/propane/oxygen).
 */
static bool TaskPropCarry_IsCarrying(int client) {
    int activeWeapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
    if (!IsValidEntity(activeWeapon)) return false;

    char classname[64];
    GetEntityClassname(activeWeapon, classname, sizeof(classname));

    if (StrEqual(classname, "weapon_gascan")) return true;
    if (StrEqual(classname, "weapon_propanetank")) return true;
    if (StrEqual(classname, "weapon_oxygentank")) return true;

    return false;
}

/**
 * Check if bot is specifically carrying a gascan (for pour logic).
 */
static bool TaskPropCarry_IsCarryingGascan(int client) {
    int activeWeapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
    if (!IsValidEntity(activeWeapon)) return false;

    char classname[64];
    GetEntityClassname(activeWeapon, classname, sizeof(classname));
    return StrEqual(classname, "weapon_gascan");
}

/**
 * Find the nearest unowned prop within scan range.
 */
static int TaskPropCarry_FindNearestProp(int client) {
    int best = -1;
    float bestDist = PROP_SCAN_RANGE + 1.0;

    static const char propClassnames[][] = {
        "weapon_gascan",
        "weapon_propanetank",
        "weapon_oxygentank"
    };

    for (int p = 0; p < 3; p++) {
        int ent = -1;
        while ((ent = FindEntityByClassname(ent, propClassnames[p])) != -1) {
            if (!IsValidEntity(ent)) continue;

            // Skip if someone is carrying it
            int owner = GetEntPropEnt(ent, Prop_Data, "m_hOwnerEntity");
            if (owner > 0 && owner <= MaxClients) continue;

            float entPos[3];
            GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);
            float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, entPos);
            if (dist > PROP_SCAN_RANGE) continue;
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
 * Find the center position of a nearby common infected horde.
 * @return Number of infected found in the group.
 */
static int TaskPropCarry_FindHordeCenter(int client, float result[3]) {
    float cx = 0.0, cy = 0.0, cz = 0.0;
    int count = 0;

    int ent = -1;
    while ((ent = FindEntityByClassname(ent, "infected")) != -1) {
        if (!IsValidEntity(ent)) continue;

        float infPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", infPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, infPos);
        if (dist > PROP_HORDE_SCAN_RANGE) continue;

        cx += infPos[0];
        cy += infPos[1];
        cz += infPos[2];
        count++;
        if (count >= 15) break;  // Limit iterations
    }

    if (count >= 5) {
        result[0] = cx / float(count);
        result[1] = cy / float(count);
        result[2] = (cz / float(count)) + 20.0;
    }

    return count;
}
