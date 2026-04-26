/**
 * AI Task: Trigger Search
 * 
 * Priority Order: 5 (lowest — only runs when nothing else is active)
 * Searches for interactive map entities (buttons, doors, timed buttons)
 * and activates them to help the team progress through the map.
 *
 * Tracked classnames:
 *   - func_button         (elevator buttons, gate controls)
 *   - func_button_timed   (timed buttons, finales)
 *   - prop_door_rotating  (closable/openable doors)
 *
 * Maintains a per-round used-trigger list to avoid re-pressing already
 * activated entities. Reset on OnMapStart.
 */

#if defined _task_trigger_search_included
    #endinput
#endif
#define _task_trigger_search_included

// Trigger search constants
#define TRIGGER_SEARCH_RANGE      800.0   // Max search radius (units)
#define TRIGGER_USE_RANGE         96.0    // Must be this close to activate
#define TRIGGER_SCAN_INTERVAL     3.0     // Seconds between scans

// Global used-trigger tracking (reset each map)
#define MAX_ENTITIES 2048
static bool g_bTriggerUsed[MAX_ENTITIES];

// Per-bot trigger search state
static int g_iTriggerTarget[MAXPLAYERS + 1];           // Entity being pursued
static float g_fTriggerScanTime[MAXPLAYERS + 1];       // Last scan timestamp

// Classnames to search for, checked in order
static char g_sTriggerClassnames[][] = {
    "func_button",
    "func_button_timed",
    "prop_door_rotating",
    "prop_door_rotating_checkpoint"
};

// Re-interaction cooldown: allow re-pressing after this many seconds
#define TRIGGER_REINTERACT_COOLDOWN 10.0
static float g_fTriggerLastUsed[MAX_ENTITIES];   // Time when entity was last used

/**
 * Reset all used-trigger flags. Called from OnMapStart.
 */
void TriggerSearch_ResetGlobals() {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        g_bTriggerUsed[i] = false;
        g_fTriggerLastUsed[i] = 0.0;
    }
    for (int i = 0; i <= MaxClients; i++) {
        g_iTriggerTarget[i] = -1;
        g_fTriggerScanTime[i] = 0.0;
    }
}

/**
 * Check if trigger search task should activate for this bot.
 */
bool TaskTriggerSearch_ShouldUpdate(int client) {
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;
    if (g_BotState[client].isInCombat) return false;
    if (g_BotState[client].isRescuing) return false;
    if (g_BotState[client].isHealing) return false;

    // If already tracking a trigger, continue
    int current = g_iTriggerTarget[client];
    if (current > 0 && current < MAX_ENTITIES && IsValidEntity(current) && !g_bTriggerUsed[current]) {
        return true;
    }

    // Throttle scanning
    float curTime = GetGameTime();
    if ((curTime - g_fTriggerScanTime[client]) < TRIGGER_SCAN_INTERVAL) {
        return false;
    }
    g_fTriggerScanTime[client] = curTime;

    // Search for an interactive entity
    // Allow re-interaction with entities after cooldown (doors can close, buttons may need re-pressing)
    int best = -1;
    float bestDist = TRIGGER_SEARCH_RANGE + 1.0;

    for (int c = 0; c < sizeof(g_sTriggerClassnames); c++) {
        int ent = -1;
        int safetyCount = 0;
        while ((ent = FindEntityByClassname(ent, g_sTriggerClassnames[c])) != -1) {
            if (++safetyCount > 256) break;
            if (!IsValidEntity(ent)) { ent++; continue; }
            if (ent <= 0 || ent >= MAX_ENTITIES) continue;

            // Skip recently used entities (cooldown)
            if (g_bTriggerUsed[ent]) {
                if (g_fTriggerLastUsed[ent] > 0.0 &&
                    (curTime - g_fTriggerLastUsed[ent]) < TRIGGER_REINTERACT_COOLDOWN) {
                    continue;  // Too soon to re-interact
                }
                // Cooldown expired — allow re-interaction
                g_bTriggerUsed[ent] = false;
            }

            // Check if the entity is locked / disabled
            if (TriggerSearch_IsLocked(ent)) continue;

            // For doors: skip if already open (rotation > 0 means it's been opened)
            char cls[64];
            GetEntityClassname(ent, cls, sizeof(cls));
            if (StrContains(cls, "prop_door_rotating") != -1) {
                if (HasEntProp(ent, Prop_Send, "m_eDoorState")) {
                    int doorState = GetEntProp(ent, Prop_Send, "m_eDoorState");
                    if (doorState == 2 || doorState == 3) continue;  // Door is opening/open, skip
                }
            }

            float entPos[3];
            GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);
            float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, entPos);
            if (dist > TRIGGER_SEARCH_RANGE) continue;

            if (dist < bestDist) {
                bestDist = dist;
                best = ent;
            }
        }
    }

    if (best != -1) {
        g_iTriggerTarget[client] = best;
        return true;
    }

    g_iTriggerTarget[client] = -1;
    return false;
}

/**
 * Execute trigger search logic: move to entity and activate it.
 * @return false (never locks — lowest priority, fully interruptible).
 */
bool TaskTriggerSearch_Update(int client, int &buttons, float vel[3]) {
    int target = g_iTriggerTarget[client];

    // Validate target
    if (target <= 0 || target >= MAX_ENTITIES || !IsValidEntity(target)) {
        TaskTriggerSearch_Reset(client);
        return false;
    }

    // Already used this round? (with cooldown-based re-interaction)
    if (g_bTriggerUsed[target]) {
        float curTime = GetGameTime();
        if (g_fTriggerLastUsed[target] > 0.0 &&
            (curTime - g_fTriggerLastUsed[target]) < TRIGGER_REINTERACT_COOLDOWN) {
            TaskTriggerSearch_Reset(client);
            return false;
        }
        // Cooldown expired — allow re-interaction
        g_bTriggerUsed[target] = false;
    }

    float entPos[3];
    GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", entPos);
    float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, entPos);

    if (dist <= TRIGGER_USE_RANGE) {
        // Close enough — look at entity and activate
        entPos[2] += 20.0;
        AI_SnapViewToPosition(client, entPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);

        // Activate via AcceptEntityInput
        AcceptEntityInput(target, "Use", client, client);

        // Also press USE button as backup
        AI_PressUseButton(client, buttons);

        // Mark as used (with timestamp for re-interaction cooldown)
        g_bTriggerUsed[target] = true;
        g_fTriggerLastUsed[target] = GetGameTime();
        g_iTriggerTarget[client] = -1;

        return false;
    }

    // Move toward entity
    AI_ComputeMoveToward(client, entPos, g_BotState[client].absOrigin,
                          g_BotState[client].eyeAngles, vel);

    // Stuck teleport for trigger approach
    float triggerElapsed = GetGameTime() - g_BotState[client].taskStartTime;
    if (AI_ShouldAbandonNavigation(client)) {
        TaskTriggerSearch_Reset(client);
        return false;
    }
    AI_TryStuckTeleport(client, entPos, dist, triggerElapsed, 300.0, 10.0);

    return false; // Never lock — interruptible
}

/**
 * Reset trigger search state for a bot.
 */
void TaskTriggerSearch_Reset(int client) {
    g_iTriggerTarget[client] = -1;
    AI_ResetTeleportState(client);
}

// ============================================================
// Internal helpers
// ============================================================

/**
 * Check if an interactive entity is locked/disabled.
 * func_button has m_bLocked, prop_door_rotating has m_bLocked.
 * If the prop doesn't have the property, assume unlocked.
 */
static bool TriggerSearch_IsLocked(int entity) {
    if (HasEntProp(entity, Prop_Data, "m_bLocked")) {
        return (GetEntProp(entity, Prop_Data, "m_bLocked") != 0);
    }
    return false;
}
