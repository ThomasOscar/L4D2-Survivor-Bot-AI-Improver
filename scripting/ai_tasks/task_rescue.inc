/**
 * AI Task: Rescue
 * 
 * Priority Order: 0 (Highest priority!)
 * Rescues pinned (SI-controlled) or incapacitated teammates.
 * Uses a global rescuer assignment table to coordinate multiple bots.
 */

#if defined _task_rescue_included
    #endinput
#endif
#define _task_rescue_included

// Rescue range constants
#define RESCUE_SHOVE_RANGE      80.0
#define RESCUE_SHOOT_RANGE      1000.0
#define RESCUE_REVIVE_RANGE     64.0

// Global rescuer assignment table (exception: cross-bot coordination)
// g_iAssignedRescuer[victim] = rescuer client index, or -1 if unassigned
int g_iAssignedRescuer[MAXPLAYERS + 1];

/**
 * Initialize rescue assignments. Called from map start or plugin start.
 */
void TaskRescue_InitGlobals() {
    for (int i = 0; i <= MaxClients; i++) {
        g_iAssignedRescuer[i] = -1;
    }
}

/**
 * Get the attacker entity that is controlling a victim.
 * Checks all SI grab types: Smoker tongue, Hunter pounce, Jockey ride, Charger pummel.
 * @return Attacker entity index, or -1 if not controlled.
 */
static int TaskRescue_GetAttacker(int victim) {
    if (!AI_IsValidClient(victim) || !IsPlayerAlive(victim)) return -1;
    if (!HasEntProp(victim, Prop_Send, "m_tongueOwner")) return -1;

    int ent;
    ent = GetEntPropEnt(victim, Prop_Send, "m_tongueOwner");
    if (ent > 0 && ent <= MaxClients) return ent;

    ent = GetEntPropEnt(victim, Prop_Send, "m_pounceAttacker");
    if (ent > 0 && ent <= MaxClients) return ent;

    ent = GetEntPropEnt(victim, Prop_Send, "m_jockeyAttacker");
    if (ent > 0 && ent <= MaxClients) return ent;

    ent = GetEntPropEnt(victim, Prop_Send, "m_pummelAttacker");
    if (ent > 0 && ent <= MaxClients) return ent;

    return -1;
}

/**
 * Check if a survivor needs rescue (pinned or incapped).
 */
static bool TaskRescue_NeedsHelp(int victim) {
    if (!AI_IsValidClient(victim) || !IsPlayerAlive(victim)) return false;
    if (GetClientTeam(victim) != 2) return false;

    // Pinned by SI?
    if (AI_IsPlayerPinned(victim)) return true;

    // Incapacitated?
    if (AI_IsPlayerIncapacitated(victim)) return true;

    return false;
}

/**
 * Clean up stale rescue assignments (dead rescuers, rescued victims, etc.)
 */
static void TaskRescue_CleanupAssignments() {
    for (int victim = 1; victim <= MaxClients; victim++) {
        if (g_iAssignedRescuer[victim] == -1) continue;

        int rescuer = g_iAssignedRescuer[victim];

        // Victim no longer needs help
        if (!TaskRescue_NeedsHelp(victim)) {
            g_iAssignedRescuer[victim] = -1;
            continue;
        }

        // Rescuer is invalid/dead/incapped/pinned
        if (!AI_IsValidClient(rescuer) || !IsPlayerAlive(rescuer) ||
            AI_IsPlayerIncapacitated(rescuer) || AI_IsPlayerPinned(rescuer)) {
            g_iAssignedRescuer[victim] = -1;
            continue;
        }
    }
}

/**
 * Check if this bot is already assigned as a rescuer for someone.
 * @return victim client index, or -1 if not assigned.
 */
static int TaskRescue_GetAssignedVictim(int rescuer) {
    for (int v = 1; v <= MaxClients; v++) {
        if (g_iAssignedRescuer[v] == rescuer) return v;
    }
    return -1;
}

/**
 * Check if rescue task should activate for this bot.
 */
bool TaskRescue_ShouldUpdate(int client) {
    if (!g_bRescueEnabled) return false;
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;

    // Cleanup stale assignments
    TaskRescue_CleanupAssignments();

    // Already assigned as rescuer?
    int assignedVictim = TaskRescue_GetAssignedVictim(client);
    if (assignedVictim != -1 && TaskRescue_NeedsHelp(assignedVictim)) {
        return true;
    }

    // Check if any teammate needs help and this bot can be assigned
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!TaskRescue_NeedsHelp(i)) continue;

        // Already has a rescuer assigned?
        if (g_iAssignedRescuer[i] != -1) continue;

        // Find closest available bot for this victim
        float victimPos[3];
        GetClientAbsOrigin(i, victimPos);

        int closestBot = -1;
        float closestDist = 999999.0;

        for (int b = 1; b <= MaxClients; b++) {
            if (!IsClientInGame(b)) continue;
            if (!IsFakeClient(b)) continue;
            if (!IsPlayerAlive(b)) continue;
            if (GetClientTeam(b) != 2) continue;
            if (AI_IsPlayerIncapacitated(b) || AI_IsPlayerPinned(b)) continue;

            // Already rescuing someone else?
            if (TaskRescue_GetAssignedVictim(b) != -1) continue;

            float botPos[3];
            GetClientAbsOrigin(b, botPos);
            float dist = AI_GetVectorDistance(botPos, victimPos);
            if (dist < closestDist) {
                closestDist = dist;
                closestBot = b;
            }
        }

        if (closestBot == client) {
            // Assign this bot as the rescuer
            g_iAssignedRescuer[i] = client;
            return true;
        }
    }

    return false;
}

/**
 * Execute rescue logic: move toward victim, kill attacker or revive.
 * @return Always true (rescue locks the task).
 */
bool TaskRescue_Update(int client, int &buttons, float vel[3]) {
    int victim = TaskRescue_GetAssignedVictim(client);
    if (victim == -1 || !TaskRescue_NeedsHelp(victim)) {
        TaskRescue_Reset(client);
        return false;
    }

    float victimPos[3];
    GetClientAbsOrigin(victim, victimPos);

    // Update BotState
    g_BotState[client].rescueTarget = victim;
    g_BotState[client].isRescuing = true;

    // === Emergency teleport: if too far and stuck for too long ===
    float rescueDist = AI_GetVectorDistance(g_BotState[client].absOrigin, victimPos);
    float rescueElapsed = GetGameTime() - g_BotState[client].taskStartTime;

    if (rescueDist > g_fTeleportRange && rescueElapsed > g_fSaveTeleportTime) {
        float telePos[3];
        telePos[0] = victimPos[0] + GetRandomFloat(-80.0, 80.0);
        telePos[1] = victimPos[1] + GetRandomFloat(-80.0, 80.0);
        telePos[2] = victimPos[2] + 10.0;

        TeleportEntity(client, telePos, NULL_VECTOR, NULL_VECTOR);
        g_BotState[client].taskStartTime = GetGameTime(); // Reset to prevent repeated teleports

        if (g_bDebugMode) {
            PrintToServer("[AI Bot] Rescue teleport: bot %d -> target %d (dist=%.0f)", client, victim, rescueDist);
        }
    }

    // === Case 1: Victim is pinned by SI — kill the attacker ===
    if (AI_IsPlayerPinned(victim)) {
        int attacker = TaskRescue_GetAttacker(victim);
        g_BotState[client].rescueAttacker = attacker;

        if (attacker != -1 && AI_IsValidClient(attacker) && IsPlayerAlive(attacker)) {
            float attackerPos[3];
            GetClientAbsOrigin(attacker, attackerPos);
            float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, attackerPos);

            // Close enough to shove?
            if (dist < RESCUE_SHOVE_RANGE) {
                attackerPos[2] += 40.0;
                AI_SnapViewToPosition(client, attackerPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                AI_PressShoveButton(client, buttons);
                return true;
            }

            // Can see attacker? Shoot!
            if (dist < RESCUE_SHOOT_RANGE &&
                AI_IsVisibleEntity(g_BotState[client].eyePos, attacker, MASK_SHOT, client)) {
                attackerPos[2] += 40.0;
                AI_SnapViewToPosition(client, attackerPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);

                // Auto-reload check
                int weapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
                if (IsValidEntity(weapon) && GetEntProp(weapon, Prop_Send, "m_iClip1") <= 0) {
                    buttons |= IN_RELOAD;
                } else {
                    AI_PressAttackButton(client, buttons);
                }
                return true;
            }

            // Too far or not visible — move toward attacker
            AI_ComputeMoveToward(client, attackerPos, g_BotState[client].absOrigin,
                                  g_BotState[client].eyeAngles, vel);
            return true;
        }

        // Attacker not found but victim is still pinned — move toward victim
        AI_ComputeMoveToward(client, victimPos, g_BotState[client].absOrigin,
                              g_BotState[client].eyeAngles, vel);
        return true;
    }

    // === Case 2: Victim is incapacitated — revive ===
    if (AI_IsPlayerIncapacitated(victim)) {
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, victimPos);

        if (dist < RESCUE_REVIVE_RANGE) {
            // Close enough — look at victim and press USE
            victimPos[2] += 20.0;
            AI_SnapViewToPosition(client, victimPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
            AI_PressUseButton(client, buttons);
            return true;
        }

        // Move toward victim
        AI_ComputeMoveToward(client, victimPos, g_BotState[client].absOrigin,
                              g_BotState[client].eyeAngles, vel);
        return true;
    }

    return true;
}

/**
 * Reset rescue state and release assignment.
 */
void TaskRescue_Reset(int client) {
    g_BotState[client].rescueTarget = -1;
    g_BotState[client].rescueAttacker = -1;
    g_BotState[client].isRescuing = false;

    // Clear any assignments where this bot is the rescuer
    for (int v = 1; v <= MaxClients; v++) {
        if (g_iAssignedRescuer[v] == client) {
            g_iAssignedRescuer[v] = -1;
        }
    }
}
