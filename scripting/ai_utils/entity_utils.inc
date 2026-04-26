/**
 * AI Entity Utilities
 * 
 * Client validation, team checks, weapon queries, and entity helpers.
 * No global variable dependencies — all data passed via parameters.
 */

#if defined _entity_utils_included
    #endinput
#endif
#define _entity_utils_included

#include <sourcemod>
#include <sdktools>

// ============================================================================
// Client Validation
// ============================================================================

/**
 * Check if a client index is valid and in-game.
 *
 * @param client    Client index.
 * @return          True if the client is valid and in-game.
 */
stock bool AI_IsValidClient(int client)
{
    return (1 <= client <= MaxClients && IsClientInGame(client));
}

/**
 * Check if a client is a fake client (bot).
 *
 * @param client    Client index.
 * @return          True if the client is a valid in-game bot.
 */
stock bool AI_IsPlayerBot(int client)
{
    return (AI_IsValidClient(client) && IsFakeClient(client));
}

/**
 * Check if a client is on the Survivor team (team 2) and alive.
 *
 * @param client    Client index.
 * @return          True if valid, in-game, survivor team, and alive.
 */
stock bool AI_IsPlayerSurvivor(int client)
{
    return (AI_IsValidClient(client) && GetClientTeam(client) == 2 && IsPlayerAlive(client));
}

/**
 * Check if a client is on the Infected team (team 3).
 *
 * @param client    Client index.
 * @return          True if valid, in-game, and infected team.
 */
stock bool AI_IsPlayerInfected(int client)
{
    return (AI_IsValidClient(client) && GetClientTeam(client) == 3);
}

// ============================================================================
// Player State Checks
// ============================================================================

/**
 * Check if a survivor is incapacitated (downed).
 *
 * @param client    Client index (must be valid and alive).
 * @return          True if incapacitated.
 */
stock bool AI_IsPlayerIncapacitated(int client)
{
    if (!AI_IsValidClient(client) || !IsPlayerAlive(client))
        return false;
    return (GetEntProp(client, Prop_Send, "m_isIncapacitated") != 0);
}

/**
 * Check if a survivor is on their third strike (black & white).
 *
 * @param client    Client index (must be valid and alive).
 * @return          True if on third strike.
 */
stock bool AI_IsPlayerOnThirdStrike(int client)
{
    if (!AI_IsValidClient(client) || !IsPlayerAlive(client))
        return false;
    return (GetEntProp(client, Prop_Send, "m_bIsOnThirdStrike") != 0);
}

/**
 * Check if a survivor is pinned by any special infected.
 * Checks: smoker tongue, hunter pounce, jockey ride, charger carry/pummel.
 *
 * @param client    Client index (must be valid and alive).
 * @return          True if pinned by any special infected.
 */
stock bool AI_IsPlayerPinned(int client)
{
    if (!AI_IsValidClient(client) || !IsPlayerAlive(client))
        return false;

    if (!HasEntProp(client, Prop_Send, "m_pounceAttacker"))
        return false;

    if (GetEntPropEnt(client, Prop_Send, "m_tongueOwner") != -1) return true;
    if (GetEntPropEnt(client, Prop_Send, "m_pounceAttacker") != -1) return true;
    if (GetEntPropEnt(client, Prop_Send, "m_jockeyAttacker") != -1) return true;
    if (GetEntPropEnt(client, Prop_Send, "m_carryAttacker") != -1) return true;
    if (GetEntPropEnt(client, Prop_Send, "m_pummelAttacker") != -1) return true;
    return false;
}

/**
 * Get the type of pin control on a survivor.
 *
 * @param client    Client index (must be valid and alive).
 * @return          0 = free, 1 = smoker, 2 = hunter, 3 = jockey,
 *                  4 = charger_carry, 5 = charger_pummel.
 */
stock int AI_GetPlayerControlType(int client)
{
    if (!AI_IsValidClient(client) || !IsPlayerAlive(client))
        return 0;

    if (!HasEntProp(client, Prop_Send, "m_pounceAttacker"))
        return 0;

    if (GetEntPropEnt(client, Prop_Send, "m_tongueOwner") != -1) return 1;     // smoker
    if (GetEntPropEnt(client, Prop_Send, "m_pounceAttacker") != -1) return 2;   // hunter
    if (GetEntPropEnt(client, Prop_Send, "m_jockeyAttacker") != -1) return 3;   // jockey
    if (GetEntPropEnt(client, Prop_Send, "m_carryAttacker") != -1) return 4;    // charger carry
    if (GetEntPropEnt(client, Prop_Send, "m_pummelAttacker") != -1) return 5;   // charger pummel
    return 0;
}

/**
 * Check if an entity is on fire.
 *
 * @param entity    Entity index.
 * @return          True if entity has FL_ONFIRE flag.
 */
stock bool AI_IsEntityOnFire(int entity)
{
    return ((GetEntityFlags(entity) & FL_ONFIRE) != 0);
}

// ============================================================================
// Weapon Helpers
// ============================================================================

/**
 * Get the weapon entity in a specific inventory slot.
 *
 * @param client    Client index.
 * @param slot      Weapon slot (0 = primary, 1 = secondary, etc.).
 * @return          Weapon entity index, or -1 if empty/invalid.
 */
stock int AI_GetWeaponSlot(int client, int slot)
{
    if (!AI_IsValidClient(client))
        return -1;
    return GetPlayerWeaponSlot(client, slot);
}

/**
 * Get the classname of a weapon entity.
 *
 * @param weapon    Weapon entity index.
 * @param buffer    Output buffer for the classname.
 * @param maxlen    Maximum length of the buffer.
 * @return          True if successful.
 */
stock bool AI_GetWeaponClassname(int weapon, char[] buffer, int maxlen)
{
    if (!IsValidEntity(weapon))
    {
        buffer[0] = '\0';
        return false;
    }
    GetEntityClassname(weapon, buffer, maxlen);
    return true;
}

// ============================================================================
// Entity Helpers
// ============================================================================

/**
 * Validate an entity index (handles both edict and non-edict range).
 *
 * @param entity    Entity index to validate.
 * @return          True if the entity is valid.
 */
stock bool AI_IsValidEntity(int entity)
{
    if (entity <= 0)
        return false;
    if (entity <= 2048) // MAXENTITIES
        return IsValidEdict(entity);
    return IsValidEntity(entity);
}

/**
 * Get the absolute origin of any entity.
 *
 * @param entity    Entity index.
 * @param result    Output vector for the entity's position.
 * @return          True if position was retrieved successfully.
 */
stock bool AI_GetEntityAbsOrigin(int entity, float result[3])
{
    if (!AI_IsValidEntity(entity))
        return false;
    GetEntPropVector(entity, Prop_Data, "m_vecAbsOrigin", result);
    return true;
}

/**
 * Get the centeroid (bounding box center) of an entity.
 *
 * @param entity    Entity index.
 * @param result    Output vector for the center point.
 * @return          True if successful.
 */
stock bool AI_GetEntityCenteroid(int entity, float result[3])
{
    if (!AI_GetEntityAbsOrigin(entity, result))
        return false;

    char netClass[64];
    if (!GetEntityNetClass(entity, netClass, sizeof(netClass)))
        return false;

    int offset = FindSendPropInfo(netClass, "m_vecMins");
    if (offset == -1)
        return false;

    float mins[3], maxs[3];
    GetEntDataVector(entity, offset, mins);
    GetEntPropVector(entity, Prop_Send, "m_vecMaxs", maxs);

    result[0] += (mins[0] + maxs[0]) * 0.5;
    result[1] += (mins[1] + maxs[1]) * 0.5;
    result[2] += (mins[2] + maxs[2]) * 0.5;

    return true;
}

/**
 * Get the health of an entity.
 *
 * @param entity    Entity index.
 * @return          Current health, or 0 if invalid.
 */
stock int AI_GetEntityHealth(int entity)
{
    if (!AI_IsValidEntity(entity))
        return 0;
    return GetEntProp(entity, Prop_Data, "m_iHealth");
}

/**
 * Get the max health of an entity.
 *
 * @param entity    Entity index.
 * @return          Max health, or 0 if invalid.
 */
stock int AI_GetEntityMaxHealth(int entity)
{
    if (!AI_IsValidEntity(entity))
        return 0;
    return GetEntProp(entity, Prop_Data, "m_iMaxHealth");
}

/**
 * Find the nearest entity of a given classname within a maximum distance.
 *
 * @param classname     Entity classname to search for (e.g. "weapon_first_aid_kit").
 * @param origin        Reference position to measure distance from.
 * @param maxDist       Maximum distance (actual distance, not squared).
 * @return              Entity index of the nearest match, or -1 if none found.
 */
stock int AI_FindNearestEntityByClassname(const char[] classname, const float origin[3], float maxDist)
{
    int best = -1;
    float bestDist = maxDist * maxDist; // Use squared for performance
    float entPos[3];

    int ent = -1;
    int safetyCount = 0;
    while ((ent = FindEntityByClassname(ent, classname)) != -1)
    {
        if (++safetyCount > 256) break;
        if (!AI_IsValidEntity(ent))
        {
            ent++;
            continue;
        }

        AI_GetEntityAbsOrigin(ent, entPos);
        float dist = GetVectorDistance(origin, entPos, true);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = ent;
        }
    }

    return best;
}
