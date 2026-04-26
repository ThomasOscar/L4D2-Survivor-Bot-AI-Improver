/**
 * L4D2 AI Bot - BotState Definition
 * 
 * Central state object for each bot. All runtime state is stored here.
 * No other global arrays should be created for bot state management.
 */

#if defined _ai_bot_state_included
 #endinput
#endif
#define _ai_bot_state_included

enum struct BotState {
    // === Basic State ===
    bool isActive;              // Whether this slot is an active bot
    bool isAlive;               // Whether the bot is alive
    bool isIncapacitated;       // Whether the bot is incapacitated
    int health;                 // Current health
    float eyePos[3];            // Eye position
    float absOrigin[3];         // Feet position
    float eyeAngles[3];         // View angles

    // === Combat State ===
    int targetEntity;           // Current attack target entity
    int targetType;             // Target type (0=none, 1=common, 2=SI, 3=tank, 4=witch)
    bool isInCombat;            // Whether in combat
    int visibleThreats;         // Number of visible special infected threats
    int visibleCommons;         // Number of visible common infected

    // === Pinned State (controlled by SI) ===
    int controlledByType;       // 0=free, 1=smoker, 2=hunter, 3=jockey, 4=charger_carry, 5=charger_pummel
    int controllerEntity;       // Entity index of the controlling SI

    // === Rescue State ===
    int rescueTarget;           // Client being rescued (pinned/incapped teammate)
    int rescueAttacker;         // SI entity to kill for rescue
    bool isRescuing;            // Whether currently rescuing

    // === Heal State ===
    bool isHealing;             // Whether currently healing
    int healTarget;             // Heal target (self or teammate)
    float healEndTime;          // Expected heal end time
    bool isFrozenForHeal;       // Whether frozen for healing animation
    bool isHoldingUse;          // Whether holding USE on a timed trigger (progress bar)

    // === Inventory State ===
    int primaryWeapon;          // Primary weapon entity (slot 0)
    int secondaryWeapon;        // Secondary weapon entity (slot 1)
    int throwableWeapon;        // Throwable entity (slot 2)
    int medkit;                 // Medkit entity (slot 3)
    int pills;                  // Pills/adrenaline entity (slot 4)
    bool hasThrowable;          // Whether holding a throwable

    // === Task Queue State ===
    int currentTaskId;          // Current executing task ID (AITaskId)
    int currentTaskOrder;       // Current task priority order
    bool taskLocked;            // Whether task is locked (cannot be preempted by lower priority)
    float taskStartTime;        // When the current task started
    char taskSwitchReason[128]; // Reason for last task switch (debug)

    // === Combat Skill State ===
    float combatFirstSightTime; // When the current combat target was first spotted (for reaction delay)
    int lastCombatTarget;       // Last combat target entity (to detect target changes)

    // === Damage Protection State ===
    float lastSafePos[3];           // Last known safe position (for fall protection teleport)
    float lastSafePosTime;          // When lastSafePos was recorded
    bool isDodging;                 // Whether currently dodging a danger zone
    float dodgeExpireTime;          // When dodge movement expires

    // === Defib State ===
    int defibTarget;                // Defibrillator revive target client

    // === Upgrade Pack State ===
    int upgradeTarget;              // Upgrade pack target entity

    // === Item Pass State ===
    float itemPassCooldown;         // Cooldown timestamp for item passing
    int itemPassTarget;             // Target client for item passing
    int itemPassPhase;              // 0=idle, 1=approaching, 2=dropping

    // === Supply Management State ===
    int supplyTarget;               // Target entity to pick up
    int supplySlotType;             // 0=none, 1=primary, 2=secondary, 3=medkit, 4=pills, 5=throwable, 6=ammo
    float supplyGiveCooldown[MAXPLAYERS + 1]; // Per-player cooldown for giving items

    // === Navigation State ===
    float moveTarget[3];        // Movement target position
    bool hasMoveTarget;         // Whether there is a move target
    float lastMoveTime;         // Last time movement occurred
    bool isStuck;               // Whether the bot is stuck
    float stuckTime;            // When the bot got stuck
    int teleportAttempts;       // Number of teleport attempts for current navigation
    float lastTeleportTime;     // When the last teleport occurred

    // === LLM Advisory State ===
    char llmAction[32];         // LLM suggested action
    char llmTarget[64];         // LLM suggested target
    float llmMoveTarget[3];     // LLM suggested move position
    bool hasLLMSuggestion;      // Whether there is an LLM suggestion
    float llmSuggestionTime;    // Timestamp of the LLM suggestion

    // === Pinpoint Command State ===
    bool hasPingCommand;        // Whether there is an active ping command
    float pingCommandExpire;    // When the ping command expires
    int pingTargetEntity;       // Ping attack target entity
    float pingMoveTarget[3];    // Ping move target position

    // === Methods ===
    void Reset() {
        this.isActive = false;
        this.isAlive = false;
        this.isIncapacitated = false;
        this.health = 0;
        this.targetEntity = -1;
        this.targetType = 0;
        this.isInCombat = false;
        this.visibleThreats = 0;
        this.visibleCommons = 0;
        this.controlledByType = 0;
        this.controllerEntity = -1;
        this.rescueTarget = -1;
        this.rescueAttacker = -1;
        this.isRescuing = false;
        this.isHealing = false;
        this.healTarget = -1;
        this.healEndTime = 0.0;
        this.isFrozenForHeal = false;
        this.isHoldingUse = false;
        this.primaryWeapon = -1;
        this.secondaryWeapon = -1;
        this.throwableWeapon = -1;
        this.medkit = -1;
        this.pills = -1;
        this.hasThrowable = false;
        this.currentTaskId = -1;
        this.currentTaskOrder = -1;
        this.taskLocked = false;
        this.taskStartTime = 0.0;
        this.taskSwitchReason[0] = '\0';
        this.combatFirstSightTime = 0.0;
        this.lastCombatTarget = -1;
        this.lastSafePosTime = 0.0;
        this.isDodging = false;
        this.dodgeExpireTime = 0.0;
        this.defibTarget = -1;
        this.upgradeTarget = -1;
        this.itemPassCooldown = 0.0;
        this.itemPassTarget = -1;
        this.itemPassPhase = 0;
        this.supplyTarget = -1;
        this.supplySlotType = 0;
        for (int i = 0; i <= MAXPLAYERS; i++) {
            this.supplyGiveCooldown[i] = 0.0;
        }
        for (int i = 0; i < 3; i++) {
            this.moveTarget[i] = 0.0;
            this.lastSafePos[i] = 0.0;
            this.llmMoveTarget[i] = 0.0;
        }
        this.hasMoveTarget = false;
        this.lastMoveTime = 0.0;
        this.isStuck = false;
        this.stuckTime = 0.0;
        this.teleportAttempts = 0;
        this.lastTeleportTime = 0.0;
        this.hasLLMSuggestion = false;
        this.llmSuggestionTime = 0.0;
        this.llmAction[0] = '\0';
        this.llmTarget[0] = '\0';
        this.hasPingCommand = false;
        this.pingCommandExpire = 0.0;
        this.pingTargetEntity = -1;
        this.pingMoveTarget[0] = 0.0;
        this.pingMoveTarget[1] = 0.0;
        this.pingMoveTarget[2] = 0.0;
    }
}

// Global bot state array
BotState g_BotState[MAXPLAYERS + 1];

// Pre-cache: store last known alive/incapped position for all survivors
// Updated every frame from OnPlayerRunCmd for bots, and via events for humans
// Used by defib task because GetClientAbsOrigin returns spectator position for dead players
float g_fLastGroundPosition[MAXPLAYERS + 1][3];

/**
 * Reset all bot states. Called on map start.
 */
void ResetAllBotStates() {
    for (int i = 0; i <= MaxClients; i++) {
        // CRITICAL: Clear FL_FROZEN from entity flags before resetting state.
        // If plugin reloads while a bot is frozen for healing, the entity flag persists
        // but the BotState (isFrozenForHeal) is reset, so TaskHeal_Reset would never clear it.
        if (i > 0 && IsClientInGame(i) && g_BotState[i].isFrozenForHeal) {
            int flags = GetEntityFlags(i);
            if ((flags & FL_FROZEN) != 0) {
                SetEntityFlags(i, flags & ~FL_FROZEN);
            }
        }
        g_BotState[i].Reset();
    }
}

/**
 * Check if a client index is a valid, connected, alive survivor bot.
 */
bool IsActiveSurvivorBot(int client) {
    if (client < 1 || client > MaxClients) return false;
    if (!IsClientInGame(client)) return false;
    if (!IsFakeClient(client)) return false;
    if (!IsPlayerAlive(client)) return false;
    if (GetClientTeam(client) != 2) return false;  // Team 2 = Survivors
    return true;
}

// Shared human position cache timer — runs once every 0.5s for ALL bots
static float g_fHumanCacheTime = 0.0;
#define HUMAN_CACHE_INTERVAL 0.5

/**
 * Cache basic bot data into BotState. Called each frame from OnPlayerRunCmd.
 */
void BotState_CacheBasicData(int client) {
    GetClientEyePosition(client, g_BotState[client].eyePos);
    GetClientAbsOrigin(client, g_BotState[client].absOrigin);
    GetClientEyeAngles(client, g_BotState[client].eyeAngles);
    g_BotState[client].health = GetClientHealth(client);
    g_BotState[client].isAlive = IsPlayerAlive(client);
    g_BotState[client].isIncapacitated = GetEntProp(client, Prop_Send, "m_isIncapacitated") != 0;
    g_BotState[client].isActive = true;

    // Cache this bot's ground position as "last known position" for defib
    g_fLastGroundPosition[client] = g_BotState[client].absOrigin;

    // Cache human positions — shared across all bots, runs only once per HUMAN_CACHE_INTERVAL
    float curTime = GetGameTime();
    if ((curTime - g_fHumanCacheTime) >= HUMAN_CACHE_INTERVAL) {
        g_fHumanCacheTime = curTime;
        for (int i = 1; i <= MaxClients; i++) {
            if (!IsClientInGame(i)) continue;
            if (GetClientTeam(i) != 2) continue;
            if (!IsPlayerAlive(i)) continue;
            if (!IsFakeClient(i)) {
                GetClientAbsOrigin(i, g_fLastGroundPosition[i]);
            }
        }
    }
}
