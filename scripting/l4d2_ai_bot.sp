/**
 * L4D2 AI Bot - Advanced
 * 
 * Modular AI Bot system with a priority-based task queue architecture.
 * Inspired by VScript Advanced Bot AI's task traversal design.
 * 
 * Architecture:
 *   OnPlayerRunCmd (per-frame, per-bot)
 *     -> Cache state into BotState
 *     -> AITaskQueue_Execute() iterates tasks by priority
 *     -> Highest-priority applicable task executes and optionally locks
 *   
 *   Events (async)
 *     -> Update BotState on SI pins, heals, deaths, etc.
 *
 * All bot runtime state lives in g_BotState[MAXPLAYERS+1].
 * No other global arrays for bot state. Task registry is the only exception.
 */

#pragma newdecls required
#pragma semicolon 1

#include <sourcemod>
#include <sdktools>
#include <sdkhooks>

#undef REQUIRE_EXTENSIONS
#undef REQUIRE_PLUGIN
#include <left4dhooks>
#include <socket>
#define REQUIRE_EXTENSIONS
#define REQUIRE_PLUGIN

// Include custom AI core modules
#include "ai_core/bot_state.inc"
#include "ai_core/task_base.inc"
#include "ai_core/config.inc"
#include "ai_core/task_queue.inc"
#include "ai_core/skill_system.inc"
#include "ai_core/events.inc"

// Utility modules
#include "ai_utils/aim_utils.inc"
#include "ai_utils/move_utils.inc"
#include "ai_utils/entity_utils.inc"
#include "ai_utils/math_utils.inc"

// Task modules
#include "ai_tasks/task_combat.inc"
#include "ai_tasks/task_rescue.inc"
#include "ai_tasks/task_heal.inc"
#include "ai_tasks/task_throwable.inc"
#include "ai_tasks/task_navigate.inc"
#include "ai_tasks/task_weapon.inc"
#include "ai_tasks/task_prop_carry.inc"
#include "ai_tasks/task_search.inc"
#include "ai_tasks/task_damage_protect.inc"
#include "ai_tasks/task_item_pass.inc"
#include "ai_tasks/task_trigger_search.inc"
#include "ai_tasks/task_defib.inc"
#include "ai_tasks/task_upgrade.inc"
#include "ai_tasks/task_supply.inc"

// LLM strategic advisor (Task #109)
#include "ai_llm/llm_advisor.inc"

// AI UI
#include "ai_ui/command_handler.inc"
#include "ai_ui/menu_system.inc"
#include "ai_ui/pinpoint_system.inc"

public Plugin myinfo = {
    name        = "L4D2 AI Bot - Advanced",
    author      = "L4D2-LLM Project",
    description = "Modular AI Bot with task queue system",
    version     = "2.0.0",
    url         = "https://github.com/l4d2-llm-project"
};

// ============================================================
// Plugin Lifecycle
// ============================================================

public void OnPluginStart() {
    // Initialize configuration (ConVars)
    AIConfig_Init();

    // Initialize skill system (must be after config)
    SkillSystem_Init();

    // Initialize task queue system
    AITaskQueue_Init();

    // Register game events for state tracking
    AIEvents_Init();

    // Initialize LLM advisor
    LLMAdvisor_Init();

    // Initialize pinpoint command system
    Pinpoint_Init();

    // Register console command aliases for bot menu
    RegConsoleCmd("sm_botmenu", Cmd_BotMenu, "Open AI Bot configuration menu");

    LogMessage("[AI Bot] Plugin started. Version 2.0.0");
}

public void OnMapStart() {
    // Reset all bot states on new map
    ResetAllBotStates();

    // Reset trigger search used-entity list
    TriggerSearch_ResetGlobals();

    // Initialize prop carry pour target detection
    PropCarry_OnMapStart();

    // Reconnect LLM on map change
    LLMAdvisor_OnMapStart();

    LogMessage("[AI Bot] Map started, all bot states reset.");
}

public void OnPluginEnd() {
    LLMAdvisor_Shutdown();
}

// ============================================================
// Chat Command Hook
// ============================================================

public Action OnClientSayCommand(int client, const char[] command, const char[] sArgs) {
    if (!IsValidClient(client)) return Plugin_Continue;

    // Only handle commands starting with !
    if (sArgs[0] != '!') return Plugin_Continue;

    if (AICommand_HandleChat(client, sArgs)) {
        return Plugin_Handled;  // Suppress command text in chat
    }
    return Plugin_Continue;
}

static bool IsValidClient(int client) {
    return (client > 0 && client <= MaxClients && IsClientInGame(client));
}

public Action Cmd_BotMenu(int client, int args) {
    if (client == 0) return Plugin_Handled;
    AIMenu_ShowMain(client);
    return Plugin_Handled;
}

// ============================================================
// Core Frame Loop — OnPlayerRunCmd
// This is THE driver for all bot AI logic.
// ============================================================

public Action OnPlayerRunCmd(int client, int &buttons, int &impulse,
                              float vel[3], float angles[3], int &weapon,
                              int &subtype, int &cmdnum, int &tickcount, int &seed,
                              int mouse[2]) {
    // Only process if system is enabled
    if (!g_bBotEnabled) return Plugin_Continue;

    // Only process alive survivor bots
    if (!IsActiveSurvivorBot(client)) return Plugin_Continue;

    // Step 1: Cache basic data into BotState
    BotState_CacheBasicData(client);

    // Step 1.1: Cache safe position for fall protection + danger zone dodge check
    DamageProtect_CacheSafePos(client);

    // Step 1.2: If actively dodging a danger zone, apply dodge movement but continue task queue
    // (so high-priority tasks like rescue can still execute during dodge)
    bool wasDodging = false;
    if (g_BotState[client].isDodging && g_BotState[client].hasMoveTarget) {
        AI_ComputeMoveToward(client, g_BotState[client].moveTarget,
                              g_BotState[client].absOrigin,
                              g_BotState[client].eyeAngles, vel);
        wasDodging = true;
    }

    // Step 1.5: Expire stale LLM suggestions
    LLMAdvisor_ExpireStale(client);

    // Step 1.6: Pinpoint command override (highest priority player command)
    if (Pinpoint_HandleCommand(client, buttons, vel, angles))
    {
        return Plugin_Changed;
    }

    // Step 2: Execute task queue (modifies buttons and vel as needed)
    bool taskLocked = AITaskQueue_Execute(client, buttons, vel);

    // Step 2.5: Safety net — if Defib task is locked in revive mode (Mode A),
    // suppress IN_USE. The built-in L4D2 bot AI presses IN_USE to pick up items
    // near corpses, causing infinite medkit<->defib swap loops.
    // BUT: in search mode (Mode B), we WANT IN_USE for picking up the defib!
    if (taskLocked && g_BotState[client].currentTaskId == view_as<int>(TASK_DEFIB)
        && !TaskDefib_IsSearchMode(client)) {
        buttons &= ~IN_USE;
    }

    // Step 3: Return Plugin_Changed so engine uses our modified buttons
    return Plugin_Changed;
}

// ============================================================
// Client Connect — SDKHook Registration
// ============================================================

public void OnClientPutInServer(int client) {
    // Hook damage on all clients so we can intercept when a bot attacks them
    // or when they take damage. The callback filters for bot attackers.
    SDKHook(client, SDKHook_OnTakeDamageAlive, Combat_OnTakeDamageAlive);

    // Hook damage protection for environmental hazards (fall/fire/acid)
    SDKHook(client, SDKHook_OnTakeDamageAlive, DamageProtect_OnTakeDamage);

    // Hook USE on survivor bots so human players can request throwable exchange
    if (client > 0 && client <= MaxClients) {
        SDKHook(client, SDKHook_UsePost, OnPlayerUsePost);
    }
}

/**
 * SDKHook_UsePost callback: detect human player pressing USE on a bot.
 * Routes to ItemPass_OnPlayerUseEntity for throwable exchange.
 */
public void OnPlayerUsePost(int entity, int activator, int caller, UseType type, float value) {
    // activator/caller is the player who pressed USE, entity is the target
    int player = (caller > 0 && caller <= MaxClients) ? caller : activator;
    ItemPass_OnPlayerUseEntity(player, entity);
}

// ============================================================
// Entity Created — Hook Witch for damage modification
// ============================================================

public void OnEntityCreated(int entity, const char[] classname) {
    if (StrEqual(classname, "witch")) {
        SDKHook(entity, SDKHook_OnTakeDamageAlive, Combat_OnTakeDamageAlive);
    }

    // Molotov trajectory correction: track bot-thrown molotovs
    if (StrEqual(classname, "molotov_projectile")) {
        // Use RequestFrame to allow the entity to fully initialize
        RequestFrame(OnMolotovCreatedFrame, EntIndexToEntRef(entity));
    }
}

// ============================================================
// Molotov Trajectory Correction (VScript parity)
// Dynamically adjusts bot-thrown molotov flight path toward Tank.
// ============================================================

void OnMolotovCreatedFrame(int entRef) {
    int entity = EntRefToEntIndex(entRef);
    if (entity == INVALID_ENT_REFERENCE || !IsValidEntity(entity)) return;

    // Get the thrower — try m_hThrower first, fallback to m_hOwnerEntity
    int thrower = -1;
    if (HasEntProp(entity, Prop_Data, "m_hThrower")) {
        thrower = GetEntPropEnt(entity, Prop_Data, "m_hThrower");
    }
    if (thrower == -1 && HasEntProp(entity, Prop_Send, "m_hOwnerEntity")) {
        thrower = GetEntPropEnt(entity, Prop_Send, "m_hOwnerEntity");
    }

    // Only correct trajectory for survivor bots
    if (thrower < 1 || thrower > MaxClients) return;
    if (!IsClientInGame(thrower)) return;
    if (!IsFakeClient(thrower)) return;
    if (GetClientTeam(thrower) != 2) return;

    // Only activate when a Tank exists
    if (Molotov_FindNearestTank(entity) == -1) return;

    // Start trajectory correction timer (every 0.1s)
    CreateTimer(0.1, Timer_ModifyMolotovTrajectory, entRef, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
}

Action Timer_ModifyMolotovTrajectory(Handle timer, int entRef) {
    int entity = EntRefToEntIndex(entRef);
    if (entity == INVALID_ENT_REFERENCE || !IsValidEntity(entity)) return Plugin_Stop;

    // Verify entity is still a molotov_projectile
    char cls[32];
    GetEntityClassname(entity, cls, sizeof(cls));
    if (!StrEqual(cls, "molotov_projectile")) return Plugin_Stop;

    // Find nearest Tank
    int tank = Molotov_FindNearestTank(entity);
    if (tank == -1) return Plugin_Stop;

    // Get projectile position and Tank position
    float projPos[3], tankPos[3], dir[3];
    GetEntPropVector(entity, Prop_Data, "m_vecOrigin", projPos);
    GetClientAbsOrigin(tank, tankPos);
    tankPos[2] += 40.0; // Aim at body center

    // Compute desired direction
    SubtractVectors(tankPos, projPos, dir);
    NormalizeVector(dir, dir);
    float speed = 800.0;
    ScaleVector(dir, speed);

    // Blend: 70% corrected + 30% original momentum for natural arc
    float curVel[3];
    GetEntPropVector(entity, Prop_Data, "m_vecVelocity", curVel);
    for (int i = 0; i < 3; i++) {
        dir[i] = dir[i] * 0.7 + curVel[i] * 0.3;
    }

    TeleportEntity(entity, NULL_VECTOR, NULL_VECTOR, dir);
    return Plugin_Continue;
}

/**
 * Find the nearest alive Tank to a given entity.
 * @return Client index of nearest Tank, or -1 if none.
 */
static int Molotov_FindNearestTank(int entity) {
    float entPos[3];
    GetEntPropVector(entity, Prop_Data, "m_vecOrigin", entPos);

    int best = -1;
    float bestDist = 999999.0;
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 3) continue;
        if (GetEntProp(i, Prop_Send, "m_zombieClass") != 8) continue;

        float tankPos[3];
        GetClientAbsOrigin(i, tankPos);
        float dist = GetVectorDistance(entPos, tankPos);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

// ============================================================
// Client Disconnect — Cleanup
// ============================================================

public void OnClientDisconnect(int client) {
    if (client > 0 && client <= MaxClients) {
        SDKUnhook(client, SDKHook_OnTakeDamageAlive, Combat_OnTakeDamageAlive);
        SDKUnhook(client, SDKHook_OnTakeDamageAlive, DamageProtect_OnTakeDamage);
        SDKUnhook(client, SDKHook_UsePost, OnPlayerUsePost);
        g_BotState[client].Reset();
    }
}
