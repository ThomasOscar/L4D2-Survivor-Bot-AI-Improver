/**
 * AI Task: Ammo Upgrade Pack Use
 * 
 * Priority Order: 4 (same as Search — low priority)
 * Tick Interval: 2.0s
 *
 * Bot detects nearby unowned upgrade packs (incendiary/explosive),
 * picks one up, then deploys it at the nearest ammo pile for team benefit.
 */

#if defined _task_upgrade_included
    #endinput
#endif
#define _task_upgrade_included

// Upgrade pack constants
#define UPGRADE_SEARCH_RANGE    400.0   // Max distance to detect upgrade packs
#define UPGRADE_PICKUP_RANGE    50.0    // Distance to press USE for pickup
#define UPGRADE_DEPLOY_RANGE    100.0   // Distance to deploy at ammo pile
#define UPGRADE_AMMO_SEARCH     600.0   // Max range to find ammo pile for deploy

// Upgrade task phases
#define UPGRADE_PHASE_SEEK      0   // Looking for upgrade pack on ground
#define UPGRADE_PHASE_PICKUP    1   // Moving to pick up
#define UPGRADE_PHASE_DEPLOY    2   // Holding pack, looking for ammo pile to deploy

// Per-bot upgrade state
static int g_iUpgradePhase[MAXPLAYERS + 1];
static int g_iUpgradeDeployTarget[MAXPLAYERS + 1];  // Ammo pile entity for deploy

/**
 * Check if upgrade task should activate for this bot.
 */
bool TaskUpgrade_ShouldUpdate(int client) {
    if (!g_bUpgradeEnabled) return false;
    if (g_BotState[client].isIncapacitated) return false;
    if (g_BotState[client].controlledByType > 0) return false;

    // Phase: already holding an upgrade pack → continue to deploy phase
    if (g_iUpgradePhase[client] == UPGRADE_PHASE_DEPLOY) {
        // Verify bot still holds an upgrade pack in slot 3
        int weapon = GetPlayerWeaponSlot(client, 3);
        if (weapon != -1) {
            char cls[64];
            GetEntityClassname(weapon, cls, sizeof(cls));
            if (StrEqual(cls, "weapon_upgradepack_incendiary") ||
                StrEqual(cls, "weapon_upgradepack_explosive")) {
                return true;
            }
        }
        // Lost the pack — reset
        g_iUpgradePhase[client] = UPGRADE_PHASE_SEEK;
    }

    // Phase: seek — find a nearby unowned upgrade pack on the ground
    float pos[3];
    GetClientAbsOrigin(client, pos);

    float closest = UPGRADE_SEARCH_RANGE;
    int found = -1;

    // Search incendiary upgrade packs
    int ent = -1;
    while ((ent = FindEntityByClassname(ent, "weapon_upgradepack_incendiary")) != -1) {
        if (GetEntPropEnt(ent, Prop_Send, "m_hOwnerEntity") != -1) continue;
        float entPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);
        float dist = GetVectorDistance(pos, entPos);
        if (dist < closest) {
            closest = dist;
            found = ent;
        }
    }

    // Search explosive upgrade packs
    ent = -1;
    while ((ent = FindEntityByClassname(ent, "weapon_upgradepack_explosive")) != -1) {
        if (GetEntPropEnt(ent, Prop_Send, "m_hOwnerEntity") != -1) continue;
        float entPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);
        float dist = GetVectorDistance(pos, entPos);
        if (dist < closest) {
            closest = dist;
            found = ent;
        }
    }

    if (found != -1) {
        g_BotState[client].upgradeTarget = found;
        g_iUpgradePhase[client] = UPGRADE_PHASE_PICKUP;
        return true;
    }

    return false;
}

/**
 * Execute upgrade pack task.
 * Phase 1: Move to pack and pick up with USE.
 * Phase 2: Find nearest ammo pile, move to it, and deploy (IN_ATTACK).
 * @return true to lock this task.
 */
bool TaskUpgrade_Update(int client, int &buttons, float vel[3]) {
    // === Phase: Pickup ===
    if (g_iUpgradePhase[client] == UPGRADE_PHASE_PICKUP) {
        int target = g_BotState[client].upgradeTarget;
        if (target == -1 || !IsValidEntity(target)) return false;

        // Verify still unowned
        if (GetEntPropEnt(target, Prop_Send, "m_hOwnerEntity") != -1) return false;

        float entPos[3];
        GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", entPos);
        float dist = GetVectorDistance(g_BotState[client].absOrigin, entPos);

        if (dist <= UPGRADE_PICKUP_RANGE) {
            // Close enough — look at it and press USE
            entPos[2] += 10.0;
            AI_SnapViewToPosition(client, entPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
            AI_PressUseButton(client, buttons);

            // Transition to deploy phase next tick
            g_iUpgradePhase[client] = UPGRADE_PHASE_DEPLOY;
            return true;
        }

        // Move toward upgrade pack
        AI_ComputeMoveToward(client, entPos, g_BotState[client].absOrigin,
                              g_BotState[client].eyeAngles, vel);
        return false;  // Don't lock while approaching
    }

    // === Phase: Deploy ===
    if (g_iUpgradePhase[client] == UPGRADE_PHASE_DEPLOY) {
        // Verify bot still holds an upgrade pack
        int weapon = GetPlayerWeaponSlot(client, 3);
        if (weapon == -1) {
            g_iUpgradePhase[client] = UPGRADE_PHASE_SEEK;
            return false;
        }

        char cls[64];
        GetEntityClassname(weapon, cls, sizeof(cls));
        if (!StrEqual(cls, "weapon_upgradepack_incendiary") &&
            !StrEqual(cls, "weapon_upgradepack_explosive")) {
            g_iUpgradePhase[client] = UPGRADE_PHASE_SEEK;
            return false;
        }

        // Find nearest ammo pile
        int ammoPile = TaskUpgrade_FindAmmoPile(client);
        if (ammoPile == -1) {
            // No ammo pile nearby — deploy in place (still useful for teammates)
            TaskUpgrade_DeployInPlace(client, weapon, buttons);
            return true;
        }

        g_iUpgradeDeployTarget[client] = ammoPile;

        float ammoPos[3];
        GetEntPropVector(ammoPile, Prop_Data, "m_vecAbsOrigin", ammoPos);
        float dist = GetVectorDistance(g_BotState[client].absOrigin, ammoPos);

        if (dist <= UPGRADE_DEPLOY_RANGE) {
            // Close enough — switch to upgrade pack and deploy
            TaskUpgrade_DeployInPlace(client, weapon, buttons);
            return true;
        }

        // Move toward ammo pile
        AI_ComputeMoveToward(client, ammoPos, g_BotState[client].absOrigin,
                              g_BotState[client].eyeAngles, vel);
        return true;  // Lock while moving to deploy spot
    }

    return false;
}

/**
 * Helper: switch to upgrade pack weapon and press attack to deploy.
 */
static void TaskUpgrade_DeployInPlace(int client, int weapon, int &buttons) {
    char active[64];
    GetClientWeapon(client, active, sizeof(active));
    if (!StrEqual(active, "weapon_upgradepack_incendiary") &&
        !StrEqual(active, "weapon_upgradepack_explosive")) {
        // Switch to upgrade pack
        SetEntPropEnt(client, Prop_Send, "m_hActiveWeapon", weapon);
        return;
    }
    // Deploy by attacking
    buttons |= IN_ATTACK;
}

/**
 * Find the nearest weapon_ammo_spawn entity within UPGRADE_AMMO_SEARCH range.
 * @return Entity index, or -1 if none found.
 */
static int TaskUpgrade_FindAmmoPile(int client) {
    float pos[3];
    GetClientAbsOrigin(client, pos);

    int best = -1;
    float bestDist = UPGRADE_AMMO_SEARCH;

    int ent = -1;
    while ((ent = FindEntityByClassname(ent, "weapon_ammo_spawn")) != -1) {
        float entPos[3];
        GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);
        float dist = GetVectorDistance(pos, entPos);
        if (dist < bestDist) {
            bestDist = dist;
            best = ent;
        }
    }

    return best;
}

/**
 * Reset upgrade task state.
 */
void TaskUpgrade_Reset(int client) {
    g_BotState[client].upgradeTarget = -1;
    g_iUpgradePhase[client] = UPGRADE_PHASE_SEEK;
    g_iUpgradeDeployTarget[client] = -1;
}
