/**
 * AI Task: Item Pass
 * 
 * Priority Order: 3 (same level as Navigation)
 * Detects when a nearby teammate is critically low on health and has no medkit,
 * then approaches and drops a spare medkit for them.
 *
 * Conditions:
 *   - Bot holds a medkit (slot 3) and has HP > 50
 *   - A teammate within 400 units has HP < 30 and no medkit
 *   - 20-second cooldown between passes
 *
 * Behaviour:
 *   Phase 1 (approaching): Move toward the target teammate
 *   Phase 2 (dropping):    Switch to medkit slot and drop it
 *
 * Reference: VScript ai-item-share behaviour.
 */

#if defined _task_item_pass_included
    #endinput
#endif
#define _task_item_pass_included

// Item pass constants
#define ITEM_PASS_SEARCH_RANGE     400.0   // Max range to detect needy teammate
#define ITEM_PASS_DROP_RANGE       100.0   // Must be this close to drop
#define ITEM_PASS_COOLDOWN         20.0    // Seconds between passes
#define ITEM_PASS_SELF_HP_MIN      50      // Bot must have at least this HP to share
#define ITEM_PASS_TARGET_HP_MAX    30      // Teammate must be below this HP
#define ITEM_PASS_PILLS_HP_MAX     50      // Teammate HP threshold for pills sharing

// Pass type enum
#define PASS_TYPE_MEDKIT    1
#define PASS_TYPE_PILLS     2

// Per-bot scan throttle
static float g_fItemPassScanTime[MAXPLAYERS + 1];
static int g_iItemPassType[MAXPLAYERS + 1];  // What type of item being passed
#define ITEM_PASS_SCAN_INTERVAL    2.0

/**
 * Check if item pass task should activate for this bot.
 */
bool TaskItemPass_ShouldUpdate(int client) {
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;
    if (g_BotState[client].isInCombat) return false;
    if (g_BotState[client].isRescuing) return false;
    if (g_BotState[client].isHealing) return false;

    // If already in a pass sequence, continue
    if (g_BotState[client].itemPassPhase > 0) {
        // Validate target is still valid and needs help
        int target = g_BotState[client].itemPassTarget;
        if (target > 0 && target <= MaxClients && IsClientInGame(target) && IsPlayerAlive(target)) {
            return true;
        }
        // Target lost — reset
        TaskItemPass_Reset(client);
        return false;
    }

    // Cooldown check
    float curTime = GetGameTime();
    if (curTime < g_BotState[client].itemPassCooldown) return false;

    // Throttle scanning
    if ((curTime - g_fItemPassScanTime[client]) < ITEM_PASS_SCAN_INTERVAL) {
        return false;
    }
    g_fItemPassScanTime[client] = curTime;

    // === Priority 1: Medkit pass (HP > 50, teammate HP < 30 without medkit) ===
    int medkit = GetPlayerWeaponSlot(client, 3);
    if (medkit != -1 && g_BotState[client].health > ITEM_PASS_SELF_HP_MIN) {
        char clsname[64];
        GetEntityClassname(medkit, clsname, sizeof(clsname));
        if (StrEqual(clsname, "weapon_first_aid_kit")) {
            int bestTarget = -1;
            int lowestHP = ITEM_PASS_TARGET_HP_MAX + 1;

            for (int i = 1; i <= MaxClients; i++) {
                if (i == client) continue;
                if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
                if (GetClientTeam(i) != 2) continue;
                if (AI_IsPlayerIncapacitated(i)) continue;

                int tmHP = GetClientHealth(i);
                if (tmHP >= ITEM_PASS_TARGET_HP_MAX) continue;

                // Teammate must not already have a medkit
                if (GetPlayerWeaponSlot(i, 3) != -1) continue;

                float tmPos[3];
                GetClientAbsOrigin(i, tmPos);
                float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, tmPos);
                if (dist > ITEM_PASS_SEARCH_RANGE) continue;

                if (tmHP < lowestHP) {
                    lowestHP = tmHP;
                    bestTarget = i;
                }
            }

            if (bestTarget != -1) {
                g_BotState[client].itemPassTarget = bestTarget;
                g_BotState[client].itemPassPhase = 1;
                g_iItemPassType[client] = PASS_TYPE_MEDKIT;
                return true;
            }
        }
    }

    // === Priority 2: Pills/Adrenaline pass (teammate HP < 50 without pills) ===
    int pills = GetPlayerWeaponSlot(client, 4);
    if (pills != -1) {
        char pillsClass[64];
        GetEntityClassname(pills, pillsClass, sizeof(pillsClass));
        if (StrEqual(pillsClass, "weapon_pain_pills") || StrEqual(pillsClass, "weapon_adrenaline")) {
            int bestTarget = -1;
            int lowestHP = ITEM_PASS_PILLS_HP_MAX + 1;

            for (int i = 1; i <= MaxClients; i++) {
                if (i == client) continue;
                if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
                if (GetClientTeam(i) != 2) continue;
                if (AI_IsPlayerIncapacitated(i)) continue;

                int tmHP = GetClientHealth(i);
                if (tmHP >= ITEM_PASS_PILLS_HP_MAX) continue;

                // Teammate must not already have pills/adrenaline
                if (GetPlayerWeaponSlot(i, 4) != -1) continue;

                float tmPos[3];
                GetClientAbsOrigin(i, tmPos);
                float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, tmPos);
                if (dist > ITEM_PASS_SEARCH_RANGE) continue;

                if (tmHP < lowestHP) {
                    lowestHP = tmHP;
                    bestTarget = i;
                }
            }

            if (bestTarget != -1) {
                g_BotState[client].itemPassTarget = bestTarget;
                g_BotState[client].itemPassPhase = 1;
                g_iItemPassType[client] = PASS_TYPE_PILLS;
                return true;
            }
        }
    }

    return false;
}

/**
 * Execute item pass logic: approach teammate, switch to medkit slot, drop.
 * @return true while locked (approaching or dropping).
 */
bool TaskItemPass_Update(int client, int &buttons, float vel[3]) {
    int target = g_BotState[client].itemPassTarget;

    // Validate target
    if (target < 1 || target > MaxClients || !IsClientInGame(target) || !IsPlayerAlive(target)) {
        TaskItemPass_Reset(client);
        return false;
    }

    float targetPos[3];
    GetClientAbsOrigin(target, targetPos);
    float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, targetPos);

    // === Phase 1: Approach ===
    if (g_BotState[client].itemPassPhase == 1) {
        if (dist > ITEM_PASS_DROP_RANGE) {
            AI_ComputeMoveToward(client, targetPos, g_BotState[client].absOrigin,
                                  g_BotState[client].eyeAngles, vel);
            return true; // Stay locked while approaching
        }
        // Close enough — transition to drop phase
        g_BotState[client].itemPassPhase = 2;
    }

    // === Phase 2: Drop item ===
    if (g_BotState[client].itemPassPhase == 2) {
        // Look at teammate
        float lookPos[3];
        lookPos[0] = targetPos[0];
        lookPos[1] = targetPos[1];
        lookPos[2] = targetPos[2] + 40.0;
        AI_SnapViewToPosition(client, lookPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);

        // Switch to correct slot and drop based on pass type
        if (g_iItemPassType[client] == PASS_TYPE_PILLS) {
            FakeClientCommand(client, "slot5"); // slot5 = pills/adrenaline (0-indexed slot 4)
        } else {
            FakeClientCommand(client, "slot4"); // slot4 = medkit (0-indexed slot 3)
        }
        FakeClientCommand(client, "drop");

        // Mark cooldown and finish
        g_BotState[client].itemPassCooldown = GetGameTime() + ITEM_PASS_COOLDOWN;
        TaskItemPass_Reset(client);
        return false;
    }

    // Unexpected phase
    TaskItemPass_Reset(client);
    return false;
}

/**
 * Reset item pass state.
 */
void TaskItemPass_Reset(int client) {
    g_BotState[client].itemPassTarget = -1;
    g_BotState[client].itemPassPhase = 0;
    g_fItemPassScanTime[client] = 0.0;
    g_iItemPassType[client] = 0;
}

// ============================================================
// Throwable Exchange: Player USE on Bot → Bot drops throwable
// ============================================================

/**
 * Called when a human player presses USE on a bot.
 * If the bot has a throwable (slot 2), drop it for the player.
 * Hook this via SDKHook_UsePost in OnClientPutInServer.
 */
void ItemPass_OnPlayerUseEntity(int player, int used) {
    // Validate: player must be alive human survivor
    if (player < 1 || player > MaxClients) return;
    if (!IsClientInGame(player) || !IsPlayerAlive(player)) return;
    if (IsFakeClient(player)) return;
    if (GetClientTeam(player) != 2) return;

    // 'used' must be an alive survivor bot
    if (used < 1 || used > MaxClients) return;
    if (!IsClientInGame(used) || !IsPlayerAlive(used)) return;
    if (!IsFakeClient(used)) return;
    if (GetClientTeam(used) != 2) return;

    // Check if the bot has a throwable (slot 2)
    int botThrow = GetPlayerWeaponSlot(used, 2);
    if (botThrow == -1 || !IsValidEntity(botThrow)) return;

    // Verify it's actually a throwable weapon
    char cls[64];
    GetEntityClassname(botThrow, cls, sizeof(cls));
    if (!StrEqual(cls, "weapon_molotov") &&
        !StrEqual(cls, "weapon_pipe_bomb") &&
        !StrEqual(cls, "weapon_vomitjar")) {
        return;
    }

    // Check if the player already has a throwable
    int playerThrow = GetPlayerWeaponSlot(player, 2);
    if (playerThrow != -1) return;  // Player already has one, skip

    // Bot drops the throwable: remove from bot, give to player
    char throwClass[64];
    GetEntityClassname(botThrow, throwClass, sizeof(throwClass));

    // Remove "weapon_" prefix not needed — GivePlayerItem uses full classname
    RemovePlayerItem(used, botThrow);
    AcceptEntityInput(botThrow, "Kill");
    GivePlayerItem(player, throwClass);

    // Notify in chat
    char botName[MAX_NAME_LENGTH];
    GetClientName(used, botName, sizeof(botName));
    PrintToChat(player, "\x04[AI Bot]\x01 %s 将 %s 交给了你", botName, throwClass);
}
