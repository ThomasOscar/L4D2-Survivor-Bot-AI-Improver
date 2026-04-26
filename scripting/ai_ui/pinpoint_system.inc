/**
 * L4D2 AI Bot - Pinpoint Command System (Menu Mode)
 *
 * Allows human players to issue commands to bots via !ping chat menu.
 * Menu options: move to crosshair, attack target, select bot, rally all, cancel.
 */

#if defined _pinpoint_system_included
 #endinput
#endif
#define _pinpoint_system_included

#define PING_MAX_DISTANCE 2000.0
#define PING_COMMAND_DURATION 8.0

// Per-player currently selected bot (-1 = none, commands go to nearest bot)
static int g_PingSelectedBot[MAXPLAYERS + 1] = {-1, ...};

// ============================================================
// Initialization
// ============================================================

/**
 * Initialize pinpoint command system.
 * Called from OnPluginStart.
 */
void Pinpoint_Init()
{
    RegConsoleCmd("sm_ping", Cmd_PingDirect, "Open pinpoint command menu");
}

// ============================================================
// Command Callback - Open Menu
// ============================================================

Action Cmd_PingDirect(int client, int args)
{
    if (client <= 0 || client > MaxClients || !IsClientInGame(client))
        return Plugin_Handled;
    if (!IsPlayerAlive(client) || GetClientTeam(client) != 2)
    {
        PrintToChat(client, "\x04[AI Bot]\x01 只有存活的生还者可以使用标点系统");
        return Plugin_Handled;
    }

    Pinpoint_ShowMenu(client);
    return Plugin_Handled;
}

// ============================================================
// Menu System
// ============================================================

void Pinpoint_ShowMenu(int client)
{
    Menu menu = new Menu(Pinpoint_MenuHandler);
    menu.SetTitle("=== 标点指挥系统 ===");
    menu.AddItem("move", "移动到准星位置");
    menu.AddItem("attack", "攻击准星目标");
    menu.AddItem("select", "选中/取消 Bot");
    menu.AddItem("rally", "全体集合到我身边");
    menu.AddItem("cancel", "取消所有指令");
    menu.Display(client, 20);
}

int Pinpoint_MenuHandler(Menu menu, MenuAction action, int param1, int param2)
{
    if (action == MenuAction_Select)
    {
        char item[32];
        menu.GetItem(param2, item, sizeof(item));

        if (StrEqual(item, "move"))
        {
            Pinpoint_ExecuteMove(param1);
        }
        else if (StrEqual(item, "attack"))
        {
            Pinpoint_ExecuteAttack(param1);
        }
        else if (StrEqual(item, "select"))
        {
            Pinpoint_ExecuteSelect(param1);
        }
        else if (StrEqual(item, "rally"))
        {
            Pinpoint_ExecuteRally(param1);
        }
        else if (StrEqual(item, "cancel"))
        {
            Pinpoint_ExecuteCancel(param1);
        }
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }
    return 0;
}

// ============================================================
// RayTrace Utility
// ============================================================

/**
 * Fire a ray from player's eye and return hit position & entity.
 * @return true if ray hit something.
 */
bool Pinpoint_DoRayTrace(int client, float hitPos[3], int &hitEntity)
{
    float eyePos[3], eyeAngles[3];
    GetClientEyePosition(client, eyePos);
    GetClientEyeAngles(client, eyeAngles);

    Handle trace = TR_TraceRayFilterEx(eyePos, eyeAngles, MASK_SHOT, RayType_Infinite, PingTraceFilter, client);

    if (!TR_DidHit(trace))
    {
        delete trace;
        hitEntity = -1;
        return false;
    }

    TR_GetEndPosition(hitPos, trace);
    hitEntity = TR_GetEntityIndex(trace);
    delete trace;
    return true;
}

// ============================================================
// Trace Filter
// ============================================================

bool PingTraceFilter(int entity, int contentsMask, any data)
{
    return entity != data;  // Skip the trace originator
}

// ============================================================
// Execute Functions
// ============================================================

/**
 * Move command: RayTrace to ground, send selected/nearest bot there.
 */
void Pinpoint_ExecuteMove(int client)
{
    float hitPos[3];
    int hitEntity;

    if (!Pinpoint_DoRayTrace(client, hitPos, hitEntity))
    {
        PrintToChat(client, "\x04[AI Bot]\x01 标点未命中任何目标");
        return;
    }

    // Check distance
    float eyePos[3];
    GetClientEyePosition(client, eyePos);
    float dist = GetVectorDistance(eyePos, hitPos);
    if (dist > PING_MAX_DISTANCE)
    {
        PrintToChat(client, "\x04[AI Bot]\x01 标点距离过远 (%.0f > %.0f)", dist, PING_MAX_DISTANCE);
        return;
    }

    // Find target bot (selected or nearest)
    int bot = Pinpoint_GetTargetBot(client);
    if (bot <= 0)
    {
        PrintToChat(client, "\x04[AI Bot]\x01 附近没有可用的 Bot");
        return;
    }

    Pinpoint_SetBotCommand(bot, -1, hitPos);
    PrintToChat(client, "\x04[AI Bot]\x01 指挥 \x03%N\x01 移动到标点位置", bot);
}

/**
 * Attack command: RayTrace to find an enemy, send bot to attack.
 */
void Pinpoint_ExecuteAttack(int client)
{
    float hitPos[3];
    int hitEntity;

    if (!Pinpoint_DoRayTrace(client, hitPos, hitEntity))
    {
        PrintToChat(client, "\x04[AI Bot]\x01 标点未命中任何目标");
        return;
    }

    // Check distance
    float eyePos[3];
    GetClientEyePosition(client, eyePos);
    float dist = GetVectorDistance(eyePos, hitPos);
    if (dist > PING_MAX_DISTANCE)
    {
        PrintToChat(client, "\x04[AI Bot]\x01 标点距离过远 (%.0f > %.0f)", dist, PING_MAX_DISTANCE);
        return;
    }

    // Check if hit an infected player (SI / Tank, team 3)
    bool isEnemy = false;
    if (hitEntity >= 1 && hitEntity <= MaxClients
        && IsClientInGame(hitEntity)
        && GetClientTeam(hitEntity) == 3
        && IsPlayerAlive(hitEntity))
    {
        isEnemy = true;
    }
    // Check if hit a non-player entity (Witch / Common infected)
    else if (hitEntity > MaxClients && IsValidEntity(hitEntity))
    {
        char classname[64];
        GetEntityClassname(hitEntity, classname, sizeof(classname));
        if (StrEqual(classname, "witch") || StrEqual(classname, "infected")
            || StrEqual(classname, "smoker") || StrEqual(classname, "boomer")
            || StrEqual(classname, "hunter") || StrEqual(classname, "spitter")
            || StrEqual(classname, "jockey") || StrEqual(classname, "charger")
            || StrEqual(classname, "tank") || StrContains(classname, "hulk") != -1)
        {
            isEnemy = true;
        }
    }

    if (!isEnemy)
    {
        PrintToChat(client, "\x04[AI Bot]\x01 准星未对准敌人");
        return;
    }

    // Find target bot (selected or nearest)
    int bot = Pinpoint_GetTargetBot(client);
    if (bot <= 0)
    {
        PrintToChat(client, "\x04[AI Bot]\x01 附近没有可用的 Bot");
        return;
    }

    Pinpoint_SetBotCommand(bot, hitEntity, hitPos);
    PrintToChat(client, "\x04[AI Bot]\x01 指挥 \x03%N\x01 攻击标记目标", bot);
}

/**
 * Select command: RayTrace to find a bot, toggle selection.
 */
void Pinpoint_ExecuteSelect(int client)
{
    float hitPos[3];
    int hitEntity;

    if (!Pinpoint_DoRayTrace(client, hitPos, hitEntity))
    {
        PrintToChat(client, "\x04[AI Bot]\x01 标点未命中任何目标");
        return;
    }

    // Check if hit a survivor bot
    if (hitEntity >= 1 && hitEntity <= MaxClients
        && IsClientInGame(hitEntity)
        && IsFakeClient(hitEntity)
        && GetClientTeam(hitEntity) == 2
        && IsPlayerAlive(hitEntity))
    {
        // Toggle selection
        if (g_PingSelectedBot[client] == hitEntity)
        {
            g_PingSelectedBot[client] = -1;
            PrintToChat(client, "\x04[AI Bot]\x01 已取消选中 \x03%N", hitEntity);
        }
        else
        {
            g_PingSelectedBot[client] = hitEntity;
            PrintToChat(client, "\x04[AI Bot]\x01 已选中 Bot: \x03%N", hitEntity);
        }
    }
    else
    {
        PrintToChat(client, "\x04[AI Bot]\x01 准星未对准 Bot，请瞄准一个存活的 Bot");
    }
}

/**
 * Rally command: All bots move to player's current position.
 */
void Pinpoint_ExecuteRally(int client)
{
    float playerPos[3];
    GetClientAbsOrigin(client, playerPos);

    int count = 0;
    for (int i = 1; i <= MaxClients; i++)
    {
        if (i == client) continue;
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (!IsFakeClient(i)) continue;
        if (GetClientTeam(i) != 2) continue;

        Pinpoint_SetBotCommand(i, -1, playerPos);
        count++;
    }

    if (count > 0)
    {
        PrintToChat(client, "\x04[AI Bot]\x01 已指挥 \x03%d\x01 个 Bot 集合到你身边", count);
    }
    else
    {
        PrintToChat(client, "\x04[AI Bot]\x01 没有可用的 Bot");
    }
}

/**
 * Cancel command: Clear all ping commands and reset selection.
 */
void Pinpoint_ExecuteCancel(int client)
{
    int count = 0;
    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (!IsFakeClient(i)) continue;
        if (GetClientTeam(i) != 2) continue;

        if (g_BotState[i].hasPingCommand)
        {
            g_BotState[i].hasPingCommand = false;
            count++;
        }
    }

    g_PingSelectedBot[client] = -1;
    PrintToChat(client, "\x04[AI Bot]\x01 已取消所有标点指令 (%d 个)", count);
}

// ============================================================
// Helper: Get target bot (selected or nearest)
// ============================================================

/**
 * Get the target bot for a command: use selected bot if valid, otherwise nearest.
 * @return Bot client index, or -1 if none found.
 */
int Pinpoint_GetTargetBot(int client)
{
    int selected = g_PingSelectedBot[client];

    // Validate selected bot
    if (selected > 0 && selected <= MaxClients)
    {
        if (IsClientInGame(selected) && IsPlayerAlive(selected)
            && IsFakeClient(selected) && GetClientTeam(selected) == 2)
        {
            return selected;
        }
        // Invalid selection, clear it
        g_PingSelectedBot[client] = -1;
    }

    return Pinpoint_FindNearestBot(client);
}

// ============================================================
// Command Issuance
// ============================================================

/**
 * Set ping command state on a bot.
 */
void Pinpoint_SetBotCommand(int bot, int hitEntity, float hitPos[3])
{
    g_BotState[bot].hasPingCommand = true;
    g_BotState[bot].pingCommandExpire = GetGameTime() + PING_COMMAND_DURATION;
    g_BotState[bot].pingTargetEntity = hitEntity;
    g_BotState[bot].pingMoveTarget[0] = hitPos[0];
    g_BotState[bot].pingMoveTarget[1] = hitPos[1];
    g_BotState[bot].pingMoveTarget[2] = hitPos[2];
}

// ============================================================
// Per-Frame Command Handler
// ============================================================

/**
 * Check and apply active ping command for a bot.
 * Called from OnPlayerRunCmd before normal task scheduling.
 *
 * @return true if a ping command is active and movement was overridden.
 */
bool Pinpoint_HandleCommand(int client, int &buttons, float vel[3], float angles[3])
{
    if (!g_BotState[client].hasPingCommand)
        return false;

    // Check expiration
    if (GetGameTime() > g_BotState[client].pingCommandExpire) {
        g_BotState[client].hasPingCommand = false;
        return false;
    }

    int target = g_BotState[client].pingTargetEntity;

    // --- Attack a player target (SI / Tank) ---
    if (target > 0 && target <= MaxClients && IsClientInGame(target) && IsPlayerAlive(target)) {
        float targetPos[3];
        GetClientAbsOrigin(target, targetPos);
        float myPos[3], myAngles[3];
        GetClientAbsOrigin(client, myPos);
        GetClientEyeAngles(client, myAngles);

        float dist = GetVectorDistance(myPos, targetPos);
        if (dist > 100.0) {
            AI_ComputeMoveToward(client, targetPos, myPos, myAngles, vel);
        }

        // Aim at target's eye position
        float eyePos[3], targetEye[3];
        GetClientEyePosition(client, eyePos);
        GetClientEyePosition(target, targetEye);
        AI_SnapViewToPosition(client, targetEye, eyePos, angles);

        // Fire at target
        buttons |= IN_ATTACK;
        return true;
    }
    // --- Attack a non-player entity (Witch / Common) ---
    else if (target > MaxClients && IsValidEntity(target)) {
        float entPos[3];
        GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", entPos);
        float myPos[3], myAngles[3];
        GetClientAbsOrigin(client, myPos);
        GetClientEyeAngles(client, myAngles);

        float dist = GetVectorDistance(myPos, entPos);
        if (dist > 80.0) {
            AI_ComputeMoveToward(client, entPos, myPos, myAngles, vel);
        }

        float eyePos[3];
        GetClientEyePosition(client, eyePos);
        AI_SnapViewToPosition(client, entPos, eyePos, angles);

        // Fire at target
        buttons |= IN_ATTACK;
        return true;
    }

    // --- Move to pinpoint position ---
    float moveTarget[3];
    moveTarget[0] = g_BotState[client].pingMoveTarget[0];
    moveTarget[1] = g_BotState[client].pingMoveTarget[1];
    moveTarget[2] = g_BotState[client].pingMoveTarget[2];

    if (moveTarget[0] != 0.0 || moveTarget[1] != 0.0 || moveTarget[2] != 0.0) {
        float myPos[3], myAngles[3];
        GetClientAbsOrigin(client, myPos);
        GetClientEyeAngles(client, myAngles);

        float dist = GetVectorDistance(myPos, moveTarget);
        if (dist > 50.0) {
            AI_ComputeMoveToward(client, moveTarget, myPos, myAngles, vel);
        }
        // Don't cancel on arrival — let the command expire naturally via pingCommandExpire
        // so the bot stays in position instead of immediately running off
        return true;
    }

    return false;
}

// ============================================================
// Utility Functions
// ============================================================

/**
 * Find the nearest alive survivor bot to a given player.
 * @return Bot client index, or -1 if none found.
 */
int Pinpoint_FindNearestBot(int client)
{
    float clientPos[3];
    GetClientAbsOrigin(client, clientPos);

    int bestBot = -1;
    float bestDist = 999999.0;

    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (!IsFakeClient(i)) continue;
        if (GetClientTeam(i) != 2) continue;

        float botPos[3];
        GetClientAbsOrigin(i, botPos);
        float dist = GetVectorDistance(clientPos, botPos);
        if (dist < bestDist) {
            bestDist = dist;
            bestBot = i;
        }
    }
    return bestBot;
}
