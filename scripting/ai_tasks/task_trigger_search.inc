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
#define TRIGGER_ACID_RANGE        200.0   // Don't approach trigger if acid is this close
#define TRIGGER_TANK_RANGE        400.0   // Don't approach trigger if Tank is this close
#define TRIGGER_MAX_HOLD_ATTEMPTS 3       // After this many failed hold attempts, teleport skip
#define TRIGGER_HOLD_TIMEOUT      10.0    // Max seconds to hold USE before considering it failed
#define TRIGGER_DIRECT_ACTIVATE_TIME 3.0  // After this many seconds, try AcceptEntityInput directly

// Global used-trigger tracking (reset each map)
#define MAX_ENTITIES 2048
static bool g_bTriggerUsed[MAX_ENTITIES];

// Per-bot trigger search state
static int g_iTriggerTarget[MAXPLAYERS + 1];           // Entity being pursued
static float g_fTriggerScanTime[MAXPLAYERS + 1];       // Last scan timestamp
static float g_fTriggerHoldStart[MAXPLAYERS + 1];      // When we started holding USE
static bool g_bTriggerHolding[MAXPLAYERS + 1];          // Whether we're in hold-USE mode

// Per-entity hold-USE failure tracking (for teleport skip)
static int g_iTriggerHoldFailCount[MAX_ENTITIES];       // How many times hold-USE failed on this entity

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
        g_iTriggerHoldFailCount[i] = 0;
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

    // Don't approach triggers if standing in acid or near a Tank
    if (AI_IsDangerNearPosition(g_BotState[client].absOrigin, TRIGGER_ACID_RANGE, TRIGGER_TANK_RANGE)) {
        return false;
    }

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

            // Skip entities that exceeded max hold-USE failure count (will be teleport-skipped)
            if (g_iTriggerHoldFailCount[ent] >= TRIGGER_MAX_HOLD_ATTEMPTS) continue;

            float entPos[3];
            GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", entPos);

            // Skip if danger near the trigger entity
            if (AI_IsDangerNearPosition(entPos, TRIGGER_ACID_RANGE, TRIGGER_TANK_RANGE)) continue;

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

    // Check if this entity has exceeded max hold-USE failures — teleport skip instead
    if (TriggerSearch_NeedsHoldUse(target) && g_iTriggerHoldFailCount[target] >= TRIGGER_MAX_HOLD_ATTEMPTS) {
        AI_DebugDecision(client, "Trigger teleport-skip: hold-USE failed %d times", g_iTriggerHoldFailCount[target]);
        TriggerSearch_TeleportSkip(client, target);
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

    // Abort if danger appeared near us or the trigger
    float entPos[3];
    GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", entPos);
    if (AI_IsDangerNearPosition(g_BotState[client].absOrigin, TRIGGER_ACID_RANGE, TRIGGER_TANK_RANGE) ||
        AI_IsDangerNearPosition(entPos, TRIGGER_ACID_RANGE, TRIGGER_TANK_RANGE)) {
        if (g_bDebugMode) {
            AI_DebugDecision(client, "Trigger abort: danger nearby (acid/tank)");
        }
        TaskTriggerSearch_Reset(client);
        return false;
    }

    float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, entPos);

    if (dist <= TRIGGER_USE_RANGE) {
        // Close enough — look at entity
        entPos[2] += 20.0;
        AI_SnapViewToPosition(client, entPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);

        if (TriggerSearch_NeedsHoldUse(target)) {
            // === Hold-USE mode: keep IN_USE pressed until progress completes ===
            // Do NOT use FL_FROZEN — it prevents the engine from processing USE input,
            // which means the progress bar never appears and the trigger never activates.
            // Instead, zero vel and clear movement buttons. isHoldingUse flag protects
            // from task preemption and dodge override.

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
                AI_DebugDecision(client, "Holding USE (need %.1fs)", holdTime);
                // Also try direct activation — kickstart the entity
                AcceptEntityInput(target, "Press", client, client);
            }

            float heldDuration = GetGameTime() - g_fTriggerHoldStart[client];
            float requiredDuration = TriggerSearch_GetHoldDuration(target);

            // Phase 1: Hold USE for the required duration (progress bar should advance)
            if (heldDuration < requiredDuration + 0.5) {
                return true;  // LOCK task while holding USE
            }

            // Phase 2: Held long enough — try to complete the trigger
            // First try: mark as used normally (progress bar completed)
            g_bTriggerUsed[target] = true;
            g_fTriggerLastUsed[target] = GetGameTime();
            g_iTriggerAssignee[target] = -1;
            g_bTriggerHolding[client] = false;
            g_BotState[client].isHoldingUse = false;
            g_iTriggerTarget[client] = -1;

            // Also try direct activation as backup
            AcceptEntityInput(target, "Press", client, client);
            AI_DebugDecision(client, "Trigger USE completed (%.1fs)", heldDuration);
            return false;

            // Note: If the trigger didn't actually activate (progress bar didn't work),
            // the TRIGGER_HOLD_TIMEOUT check below handles it when the task is re-entered.
            // The g_iTriggerHoldFailCount tracks persistent failures across attempts.
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

/**
 * Teleport all bots past a trigger/door that they can't open.
 * Finds a position ahead using flow distance and teleports the team there.
 * Only used as a last resort when hold-USE fails repeatedly.
 */
static void TriggerSearch_TeleportSkip(int client, int target) {
    float triggerPos[3];
    GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", triggerPos);

    // Find a position ahead of the trigger using flow distance sampling
    float skipTarget[3];
    bool foundSkip = false;
    float bestFlow = -1.0;

    // Sample 8 directions from the trigger position to find forward
    for (int dir = 0; dir < 8; dir++) {
        float angle = float(dir) * 45.0;
        float dirVec[3];
        dirVec[0] = Cosine(DegToRad(angle));
        dirVec[1] = Sine(DegToRad(angle));
        dirVec[2] = 0.0;

        // Sample at increasing distances (300, 500, 700 units)
        for (int range = 300; range <= 700; range += 200) {
            float samplePos[3];
            samplePos[0] = triggerPos[0] + dirVec[0] * float(range);
            samplePos[1] = triggerPos[1] + dirVec[1] * float(range);
            samplePos[2] = triggerPos[2];

            // Check not blocked by wall
            Handle trace = TR_TraceRayFilterEx(triggerPos, samplePos,
                                                MASK_PLAYERSOLID_BRUSHONLY, RayType_EndPoint,
                                                AI_TraceFilter_NoPlayers, 0);
            if (TR_DidHit(trace)) {
                delete trace;
                continue;
            }
            delete trace;

            // Check flow distance — must be higher than trigger position
            Address navArea = L4D2Direct_GetTerrorNavArea(samplePos);
            if (navArea != Address_Null) {
                float sampleFlow = L4D2Direct_GetTerrorNavAreaFlow(navArea);
                float triggerFlow = 0.0;
                Address triggerArea = L4D2Direct_GetTerrorNavArea(triggerPos);
                if (triggerArea != Address_Null) {
                    triggerFlow = L4D2Direct_GetTerrorNavAreaFlow(triggerArea);
                }
                if (sampleFlow > triggerFlow && sampleFlow > bestFlow) {
                    bestFlow = sampleFlow;
                    skipTarget = samplePos;
                    foundSkip = true;
                }
            }
        }
    }

    if (!foundSkip) {
        // Fallback: teleport 500 units in the direction of the bot's facing
        float fwd[3];
        GetAngleVectors(g_BotState[client].eyeAngles, fwd, NULL_VECTOR, NULL_VECTOR);
        skipTarget[0] = triggerPos[0] + fwd[0] * 500.0;
        skipTarget[1] = triggerPos[1] + fwd[1] * 500.0;
        skipTarget[2] = triggerPos[2];
    }

    // Teleport ALL alive survivor bots past the trigger
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (!IsFakeClient(i)) continue;  // Don't teleport human players

        float tpPos[3];
        tpPos = skipTarget;
        tpPos[0] += GetRandomFloat(-50.0, 50.0);
        tpPos[1] += GetRandomFloat(-50.0, 50.0);
        TeleportEntity(i, tpPos, NULL_VECTOR, NULL_VECTOR);
        AI_DebugDecision(i, "Teleport-skip past trigger (fail=%d)", g_iTriggerHoldFailCount[target]);
    }

    // Mark trigger as used so bots don't try again
    g_bTriggerUsed[target] = true;
    g_fTriggerLastUsed[target] = GetGameTime();
    g_iTriggerAssignee[target] = -1;
    g_iTriggerTarget[client] = -1;
    g_bTriggerHolding[client] = false;
    g_BotState[client].isHoldingUse = false;
}
