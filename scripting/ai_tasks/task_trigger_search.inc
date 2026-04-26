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

// FL_FROZEN safety define — may not be available in all include sets
#if !defined FL_FROZEN
    #define FL_FROZEN (1 << 5)
#endif

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
static float g_fTriggerHoldStart[MAXPLAYERS + 1];      // When we started holding USE
static bool g_bTriggerHolding[MAXPLAYERS + 1];          // Whether we're in hold-USE mode

/**
 * Check if an entity requires HOLDING USE (progress bar) to activate.
 * func_button_timed and prop_door_rotating_checkpoint need sustained USE.
 */
static bool TriggerSearch_NeedsHoldUse(int entity) {
    char cls[64];
    if (!GetEntityClassname(entity, cls, sizeof(cls))) return false;
    // Timed buttons (panic events, finales) require holding USE
    if (StrEqual(cls, "func_button_timed")) return true;
    // Checkpoint/saferoom doors require holding USE
    if (StrEqual(cls, "prop_door_rotating_checkpoint")) return true;
    return false;
}

/**
 * Get the hold duration for a timed button (in seconds).
 */
static float TriggerSearch_GetHoldDuration(int entity) {
    if (HasEntProp(entity, Prop_Data, "m_flUseTime")) {
        return GetEntPropFloat(entity, Prop_Data, "m_flUseTime");
    }
    return 5.0;  // Default hold time if we can't read it
}
static char g_sTriggerClassnames[][] = {
    "func_button",
    "func_button_timed",
    "prop_door_rotating",
    "prop_door_rotating_checkpoint"
};

// Re-interaction cooldown: allow re-pressing after this many seconds
#define TRIGGER_REINTERACT_COOLDOWN 10.0
static float g_fTriggerLastUsed[MAX_ENTITIES];   // Time when entity was last used

// Trigger assignment: only ONE bot per trigger entity (like defib task)
static int g_iTriggerAssignee[MAX_ENTITIES];      // Which bot is assigned to this entity (-1 = none)

/**
 * Reset all used-trigger flags. Called from OnMapStart.
 */
void TriggerSearch_ResetGlobals() {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        g_bTriggerUsed[i] = false;
        g_fTriggerLastUsed[i] = 0.0;
        g_iTriggerAssignee[i] = -1;
    }
    for (int i = 0; i <= MaxClients; i++) {
        g_iTriggerTarget[i] = -1;
        g_fTriggerScanTime[i] = 0.0;
        g_bTriggerHolding[i] = false;
        g_fTriggerHoldStart[i] = 0.0;
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

    // If already tracking a trigger, continue (even if holding USE)
    int current = g_iTriggerTarget[client];
    if (current > 0 && current < MAX_ENTITIES && IsValidEntity(current) && !g_bTriggerUsed[current]) {
        // Verify our assignment is still valid
        if (g_iTriggerAssignee[current] == client) return true;
        // Someone else took over — release
        g_iTriggerTarget[client] = -1;
        g_bTriggerHolding[client] = false;
    }

    // Clean up stale assignments for this bot
    if (current > 0 && current < MAX_ENTITIES && g_iTriggerAssignee[current] == client) {
        g_iTriggerAssignee[current] = -1;
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

            // Skip entities already assigned to another bot
            if (g_iTriggerAssignee[ent] != -1 && g_iTriggerAssignee[ent] != client) {
                // Check if the assigned bot is still valid and working on it
                int assignee = g_iTriggerAssignee[ent];
                if (assignee > 0 && assignee <= MaxClients && IsClientInGame(assignee) &&
                    IsPlayerAlive(assignee) && g_iTriggerTarget[assignee] == ent) {
                    continue;  // Another bot is handling this
                }
                // Assignment is stale — clear it
                g_iTriggerAssignee[ent] = -1;
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
        // Assign this bot to the trigger (only ONE bot per trigger!)
        g_iTriggerAssignee[best] = client;
        g_iTriggerTarget[client] = best;
        return true;
    }

    g_iTriggerTarget[client] = -1;
    return false;
}

/**
 * Execute trigger search logic: move to entity and activate it.
 * For timed entities (func_button_timed, checkpoint doors), holds IN_USE
 * until the progress bar completes.
 * @return true if holding USE (locks task), false otherwise.
 */
bool TaskTriggerSearch_Update(int client, int &buttons, float vel[3]) {
    int target = g_iTriggerTarget[client];

    // Validate target
    if (target <= 0 || target >= MAX_ENTITIES || !IsValidEntity(target)) {
        TaskTriggerSearch_Reset(client);
        return false;
    }

    // Already used this round? (with cooldown-based re-interaction)
    if (g_bTriggerUsed[target] && !g_bTriggerHolding[client]) {
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
        // Close enough — look at entity
        entPos[2] += 20.0;
        AI_SnapViewToPosition(client, entPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);

        if (TriggerSearch_NeedsHoldUse(target)) {
            // === Hold-USE mode: keep IN_USE pressed until progress completes ===
            // CRITICAL: Freeze the bot at engine level (FL_FROZEN) like heal does.
            // Just clearing vel/buttons is NOT enough — other tasks/engine AI override them.
            int flags = GetEntityFlags(client);
            if ((flags & FL_FROZEN) == 0) {
                SetEntityFlags(client, flags | FL_FROZEN);
            }

            vel[0] = 0.0;
            vel[1] = 0.0;
            vel[2] = 0.0;
            buttons |= IN_USE;
            buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_DUCK);

            if (!g_bTriggerHolding[client]) {
                // First frame of hold — start tracking
                g_bTriggerHolding[client] = true;
                g_BotState[client].isHoldingUse = true;
                g_fTriggerHoldStart[client] = GetGameTime();
                float holdTime = TriggerSearch_GetHoldDuration(target);
                AI_DebugDecision(client, "Holding USE on entity (need %.1fs)", holdTime);
            }

            // Check if we've held long enough
            float heldDuration = GetGameTime() - g_fTriggerHoldStart[client];
            float requiredDuration = TriggerSearch_GetHoldDuration(target);
            if (heldDuration >= requiredDuration + 0.5) {
                // Held long enough — unfreeze, mark as used and release
                int f = GetEntityFlags(client);
                if ((f & FL_FROZEN) != 0) {
                    SetEntityFlags(client, f & ~FL_FROZEN);
                }
                g_bTriggerUsed[target] = true;
                g_fTriggerLastUsed[target] = GetGameTime();
                g_iTriggerAssignee[target] = -1;  // Clear assignment
                g_bTriggerHolding[client] = false;
                g_BotState[client].isHoldingUse = false;
                g_iTriggerTarget[client] = -1;
                return false;  // Release task
            }

            return true;  // LOCK task while holding USE
        } else {
            // === Instant-use mode: press USE once ===
            AcceptEntityInput(target, "Use", client, client);
            AI_PressUseButton(client, buttons);

            g_bTriggerUsed[target] = true;
            g_fTriggerLastUsed[target] = GetGameTime();
            g_iTriggerTarget[client] = -1;

            return false;
        }
    }

    // Not in range yet — move toward entity
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
    // CRITICAL: Remove FL_FROZEN if we were holding USE, same as heal task
    if (g_bTriggerHolding[client]) {
        int flags = GetEntityFlags(client);
        if ((flags & FL_FROZEN) != 0) {
            SetEntityFlags(client, flags & ~FL_FROZEN);
        }
    }
    // Clear our assignment on the trigger entity
    int target = g_iTriggerTarget[client];
    if (target > 0 && target < MAX_ENTITIES && g_iTriggerAssignee[target] == client) {
        g_iTriggerAssignee[target] = -1;
    }
    g_iTriggerTarget[client] = -1;
    g_bTriggerHolding[client] = false;
    g_BotState[client].isHoldingUse = false;
    g_fTriggerHoldStart[client] = 0.0;
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
