/**
 * AI Task: Search
 * 
 * Priority Order: 4
 * Searches for missing essential items: medkits, pills/adrenaline,
 * throwables, and ammo resupply points.
 */

#if defined _task_search_included
    #endinput
#endif
#define _task_search_included

// Search constants
#define SEARCH_RANGE            500.0
#define SEARCH_PICKUP_RANGE     50.0
#define SEARCH_AMMO_RANGE       300.0
#define SEARCH_SCAN_INTERVAL    1.5

// Per-bot search state
static int g_iSearchTarget[MAXPLAYERS + 1];
static int g_iSearchType[MAXPLAYERS + 1];     // 0=none, 1=medkit, 2=pills, 3=ammo, 4=throwable
static float g_fSearchScanTime[MAXPLAYERS + 1];

/**
 * Check if search task should activate for this bot.
 */
bool TaskSearch_ShouldUpdate(int client) {
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;

    // Don't search during combat, rescue, or heal
    if (g_BotState[client].isInCombat) return false;
    if (g_BotState[client].isRescuing) return false;
    if (g_BotState[client].isHealing) return false;

    // If already tracking a target, continue
    if (g_iSearchTarget[client] > 0 && IsValidEntity(g_iSearchTarget[client])) {
        return true;
    }

    // Throttle scanning
    float curTime = GetGameTime();
    if ((curTime - g_fSearchScanTime[client]) < SEARCH_SCAN_INTERVAL) {
        return false;
    }
    g_fSearchScanTime[client] = curTime;

    // Priority 0 (Emergency): Search for ammo if primary weapon is completely dry
    int primaryWeapon = GetPlayerWeaponSlot(client, 0);
    if (primaryWeapon != -1 && IsValidEntity(primaryWeapon)) {
        if (HasEntProp(primaryWeapon, Prop_Send, "m_iClip1")) {
            int clip = GetEntProp(primaryWeapon, Prop_Send, "m_iClip1");
            int ammoType = GetEntProp(primaryWeapon, Prop_Send, "m_iPrimaryAmmoType");
            int reserve = (ammoType >= 0) ? GetEntProp(client, Prop_Send, "m_iAmmo", _, ammoType) : 0;
            if (clip == 0 && reserve == 0) {
                int urgentAmmo = AI_FindNearestEntityByClassname("weapon_ammo_spawn",
                                     g_BotState[client].absOrigin, SEARCH_RANGE);
                if (urgentAmmo != -1) {
                    g_iSearchTarget[client] = urgentAmmo;
                    g_iSearchType[client] = 3;
                    return true;
                }
            }
        }
    }

    // Check what we need
    bool needMedkit = (GetPlayerWeaponSlot(client, 3) == -1);
    bool needPills = (GetPlayerWeaponSlot(client, 4) == -1);

    // Priority 1: Search for medkit
    if (needMedkit) {
        int found = AI_FindNearestEntityByClassname("weapon_first_aid_kit",
                        g_BotState[client].absOrigin, SEARCH_RANGE);
        if (found != -1 && TaskSearch_IsUnowned(found)) {
            g_iSearchTarget[client] = found;
            g_iSearchType[client] = 1;
            return true;
        }
    }

    // Priority 2: Search for pills or adrenaline
    if (needPills) {
        int foundPills = AI_FindNearestEntityByClassname("weapon_pain_pills",
                             g_BotState[client].absOrigin, SEARCH_RANGE);
        int foundAdren = AI_FindNearestEntityByClassname("weapon_adrenaline",
                             g_BotState[client].absOrigin, SEARCH_RANGE);

        int best = -1;
        float bestDist = SEARCH_RANGE + 1.0;

        if (foundPills != -1 && TaskSearch_IsUnowned(foundPills)) {
            float pPos[3];
            GetEntPropVector(foundPills, Prop_Data, "m_vecAbsOrigin", pPos);
            float d = AI_GetVectorDistance(g_BotState[client].absOrigin, pPos);
            if (d < bestDist) {
                bestDist = d;
                best = foundPills;
            }
        }
        if (foundAdren != -1 && TaskSearch_IsUnowned(foundAdren)) {
            float aPos[3];
            GetEntPropVector(foundAdren, Prop_Data, "m_vecAbsOrigin", aPos);
            float d = AI_GetVectorDistance(g_BotState[client].absOrigin, aPos);
            if (d < bestDist) {
                best = foundAdren;
            }
        }

        if (best != -1) {
            g_iSearchTarget[client] = best;
            g_iSearchType[client] = 2;
            return true;
        }
    }

    // Priority 3: Search for throwable
    bool needThrowable = (GetPlayerWeaponSlot(client, 2) == -1);
    if (needThrowable) {
        int foundThrowable = TaskSearch_FindNearestThrowable(client);
        if (foundThrowable != -1) {
            g_iSearchTarget[client] = foundThrowable;
            g_iSearchType[client] = 4;
            return true;
        }
    }

    // Priority 4: Search for ammo pile (always useful)
    int ammo = AI_FindNearestEntityByClassname("weapon_ammo_spawn",
                   g_BotState[client].absOrigin, SEARCH_AMMO_RANGE);
    if (ammo != -1) {
        g_iSearchTarget[client] = ammo;
        g_iSearchType[client] = 3;
        return true;
    }

    g_iSearchTarget[client] = -1;
    g_iSearchType[client] = 0;
    return false;
}

/**
 * Execute search logic: move toward item and pick it up.
 * @return false (never locks — low priority, interruptible).
 */
bool TaskSearch_Update(int client, int &buttons, float vel[3]) {
    int target = g_iSearchTarget[client];
    if (target <= 0 || !IsValidEntity(target)) {
        TaskSearch_Reset(client);
        return false;
    }

    // Check if the item was picked up by someone else
    if (g_iSearchType[client] != 3 && !TaskSearch_IsUnowned(target)) {
        TaskSearch_Reset(client);
        return false;
    }

    // Check if we already have the item we're looking for
    if (g_iSearchType[client] == 1 && GetPlayerWeaponSlot(client, 3) != -1) {
        TaskSearch_Reset(client);
        return false;
    }
    if (g_iSearchType[client] == 2 && GetPlayerWeaponSlot(client, 4) != -1) {
        TaskSearch_Reset(client);
        return false;
    }
    if (g_iSearchType[client] == 4 && GetPlayerWeaponSlot(client, 2) != -1) {
        TaskSearch_Reset(client);
        return false;
    }

    float targetPos[3];
    GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", targetPos);
    float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, targetPos);

    if (dist <= SEARCH_PICKUP_RANGE) {
        // Close enough — look at item and pick up
        targetPos[2] += 10.0;
        AI_SnapViewToPosition(client, targetPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
        AI_PressUseButton(client, buttons);

        // Don't clear target immediately — pickup may need multiple frames.
        // Next ShouldUpdate will detect if we already have the item (lines above)
        // and auto-reset via the "already have item" checks.
        return false;
    }

    // Move toward item
    AI_ComputeMoveToward(client, targetPos, g_BotState[client].absOrigin,
                          g_BotState[client].eyeAngles, vel);
    return false;  // Never lock
}

/**
 * Reset search state.
 */
void TaskSearch_Reset(int client) {
    g_iSearchTarget[client] = -1;
    g_iSearchType[client] = 0;
}

// ============================================================
// Internal helpers
// ============================================================

/**
 * Find the nearest unowned throwable (pipe bomb, molotov, bile jar).
 */
static int TaskSearch_FindNearestThrowable(int client) {
    static const char throwableClasses[][] = {
        "weapon_pipe_bomb",
        "weapon_molotov",
        "weapon_vomitjar"
    };

    int best = -1;
    float bestDist = SEARCH_RANGE + 1.0;

    for (int i = 0; i < sizeof(throwableClasses); i++) {
        int ent = -1;
        while ((ent = FindEntityByClassname(ent, throwableClasses[i])) != -1) {
            if (!IsValidEntity(ent)) continue;
            if (!TaskSearch_IsUnowned(ent)) continue;

            float ePos[3];
            GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", ePos);
            float d = AI_GetVectorDistance(g_BotState[client].absOrigin, ePos);
            if (d < bestDist) {
                bestDist = d;
                best = ent;
            }
        }
    }

    return best;
}

/**
 * Check if an entity is unowned (not held by any player).
 */
static bool TaskSearch_IsUnowned(int entity) {
    if (!IsValidEntity(entity)) return false;
    if (!HasEntProp(entity, Prop_Data, "m_hOwnerEntity")) return true;

    int owner = GetEntPropEnt(entity, Prop_Data, "m_hOwnerEntity");
    return (owner <= 0 || owner > MaxClients);
}
