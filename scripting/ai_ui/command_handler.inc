/**
 * L4D2 AI Bot - Chat Command Handler
 *
 * Processes player chat commands (!botskill, !botmenu, etc.)
 * to control AI Bot configuration in-game.
 *
 * All ConVar handles and cached variables are defined in ai_core/config.inc
 * which is #included before this file.
 */

#if defined _command_handler_included
    #endinput
#endif
#define _command_handler_included

// ============================================================
// Chat Command Entry Point
// ============================================================

/**
 * Main chat command dispatcher.
 * Called from OnClientSayCommand in l4d2_ai_bot.sp.
 *
 * @param client    Client index who typed the command.
 * @param message   Full chat message (e.g. "!botskill 4").
 * @return true if command was handled (suppress chat output), false otherwise.
 */
bool AICommand_HandleChat(int client, const char[] message) {
    // Parse command and argument from message
    char cmd[64], arg[64];
    int spacePos = -1;

    // Find first space to split command from argument
    for (int i = 0; message[i] != '\0'; i++) {
        if (message[i] == ' ') {
            spacePos = i;
            break;
        }
    }

    if (spacePos == -1) {
        // No space — entire message is the command, no argument
        strcopy(cmd, sizeof(cmd), message);
        arg[0] = '\0';
    } else {
        // Copy command part (before space)
        strcopy(cmd, spacePos + 1 < sizeof(cmd) ? spacePos + 1 : sizeof(cmd), message);
        // Copy argument part (after space), trim leading spaces
        int argStart = spacePos + 1;
        while (message[argStart] == ' ') argStart++;
        strcopy(arg, sizeof(arg), message[argStart]);
    }

    // Quick reject: must start with '!'
    if (cmd[0] != '!') return false;

    // --------------------------------------------------------
    // Admin permission check
    // In multiplayer, only admins can use bot commands.
    // --------------------------------------------------------
    if (GetClientCount(true) > 1) {
        if (!CheckCommandAccess(client, "sm_admin", ADMFLAG_GENERIC)) {
            PrintToChat(client, "\x04[AI Bot]\x01 仅管理员可使用此命令");
            return true;
        }
    }

    // --------------------------------------------------------
    // Dispatch to command handlers
    // --------------------------------------------------------

    // Menu (Task #124)
    if (StrEqual(cmd, "!botmenu", false)) {
        AIMenu_ShowMain(client);
        return true;
    }

    // --- Parameter commands (value setters) ---
    if (StrEqual(cmd, "!botskill", false))
        return AICommand_Skill(client, arg);

    if (StrEqual(cmd, "!botfollow", false))
        return AICommand_FloatParam(client, arg, g_cvFollowRange, "跟随距离", 100.0, 999999.0);

    if (StrEqual(cmd, "!botteleport", false))
        return AICommand_FloatParam(client, arg, g_cvTeleportRange, "传送距离", 100.0, 999999.0);

    if (StrEqual(cmd, "!botwitchdamage", false))
        return AICommand_FloatParam(client, arg, g_cvWitchDamage, "Witch伤害倍率", -16.0, 16.0);

    if (StrEqual(cmd, "!botspecialdamage", false))
        return AICommand_FloatParam(client, arg, g_cvSpecialDamage, "特感伤害倍率", -16.0, 16.0);

    if (StrEqual(cmd, "!bottankdamage", false))
        return AICommand_FloatParam(client, arg, g_cvTankDamage, "Tank伤害倍率", -16.0, 16.0);

    if (StrEqual(cmd, "!botsaveteleport", false))
        return AICommand_FloatParam(client, arg, g_cvSaveTeleportTime, "救援传送超时(秒)", 0.0, 999.0);

    // --- Toggle commands (bool switches) ---
    if (StrEqual(cmd, "!botprotect", false))
        return AICommand_Toggle(client, g_cvProtectEnabled, "免疫秒杀");

    if (StrEqual(cmd, "!botfindgas", false))
        return AICommand_Toggle(client, g_cvFindGasEnabled, "寻油");

    if (StrEqual(cmd, "!botthrowmolotov", false))
        return AICommand_Toggle(client, g_cvThrowableEnabled, "投掷物");

    if (StrEqual(cmd, "!botthrowpipe", false))
        return AICommand_Toggle(client, g_cvThrowableEnabled, "投掷物");

    if (StrEqual(cmd, "!botmelee", false))
        return AICommand_Toggle(client, g_cvMeleeEnabled, "近战");

    if (StrEqual(cmd, "!botimmunity", false))
        return AICommand_Toggle(client, g_cvImmunityEnabled, "免疫黑枪");

    if (StrEqual(cmd, "!botpathfinding", false))
        return AICommand_Toggle(client, g_cvPathfindingEnabled, "寻路");

    if (StrEqual(cmd, "!botunstick", false))
        return AICommand_Toggle(client, g_cvUnstickEnabled, "卡住传送");

    if (StrEqual(cmd, "!botupgrades", false))
        return AICommand_Toggle(client, g_cvUpgradeEnabled, "升级包");

    if (StrEqual(cmd, "!botkeepalive", false))
        return AICommand_Toggle(client, g_cvKeepaliveEnabled, "玩家死后跑图");

    if (StrEqual(cmd, "!botbackpack", false))
        return AICommand_Toggle(client, g_cvBackpackEnabled, "背物资");

    if (StrEqual(cmd, "!botdefib", false))
        return AICommand_Toggle(client, g_cvDefibEnabled, "除颤器");

    if (StrEqual(cmd, "!botsupply", false))
        return AICommand_Toggle(client, g_cvSupplyEnabled, "物资管理");

    // --- Special commands ---
    if (StrEqual(cmd, "!botstop", false)) {
        PrintToChat(client, "\x04[AI Bot]\x01 Bot停止功能尚未实现");
        return true;
    }

    // --- Debug: show bot task states ---
    if (StrEqual(cmd, "!botinfo", false)) {
        AICommand_BotInfo(client);
        return true;
    }

    return false;
}

// ============================================================
// Skill Command Handler (special: user 1-5 maps to internal 0-4)
// ============================================================

static bool AICommand_Skill(int client, const char[] arg) {
    if (arg[0] == '\0') {
        // No argument — show current value (display as user-facing 1-5)
        PrintToChat(client, "\x04[AI Bot]\x01 当前技能等级: %d", g_iBotSkillLevel + 1);
        return true;
    }

    int val = StringToInt(arg);
    if (val < 1 || val > 5) {
        PrintToChat(client, "\x04[AI Bot]\x01 技能等级范围: 1-5");
        return true;
    }

    g_cvBotSkillLevel.SetInt(val - 1);  // User 1-5 → Internal 0-4
    AIMenu_PersistInt("ai_bot_skill", val - 1);
    PrintToChat(client, "\x04[AI Bot]\x01 技能等级已设为: %d", val);
    return true;
}

// ============================================================
// Float Parameter Command Handler (generic)
// ============================================================

/**
 * Handle a float-value parameter command.
 * Without argument: show current value.
 * With argument: validate range and set ConVar.
 *
 * @param client    Client index.
 * @param arg       Argument string (may be empty).
 * @param cv        Target ConVar handle.
 * @param name      Display name for chat messages.
 * @param minVal    Minimum allowed value.
 * @param maxVal    Maximum allowed value.
 * @return true always (command is handled).
 */
static bool AICommand_FloatParam(int client, const char[] arg, ConVar cv,
                                  const char[] name, float minVal, float maxVal) {
    if (arg[0] == '\0') {
        // No argument — show current value
        PrintToChat(client, "\x04[AI Bot]\x01 当前%s: %.1f", name, cv.FloatValue);
        return true;
    }

    float val = StringToFloat(arg);
    if (val < minVal || val > maxVal) {
        PrintToChat(client, "\x04[AI Bot]\x01 %s范围: %.1f - %.1f", name, minVal, maxVal);
        return true;
    }

    cv.SetFloat(val);

    // Persist to cfg file
    char cvarName[64];
    cv.GetName(cvarName, sizeof(cvarName));
    AIMenu_PersistFloat(cvarName, val);

    PrintToChat(client, "\x04[AI Bot]\x01 %s已设为: %.1f", name, val);
    return true;
}

// ============================================================
// Toggle Command Handler (generic bool flip)
// ============================================================

/**
 * Toggle a boolean ConVar and report new state.
 *
 * @param client    Client index.
 * @param cv        Target ConVar handle.
 * @param name      Display name for chat messages.
 * @return true always (command is handled).
 */
static bool AICommand_Toggle(int client, ConVar cv, const char[] name) {
    bool newVal = !cv.BoolValue;
    cv.SetBool(newVal);

    // Persist to cfg file
    char cvarName[64];
    cv.GetName(cvarName, sizeof(cvarName));
    AIMenu_PersistBool(cvarName, newVal);

    PrintToChat(client, "\x04[AI Bot]\x01 %s: %s", name, newVal ? "开启" : "关闭");
    return true;
}

// ============================================================
// Bot Info Command — debug display
// ============================================================

static void AICommand_BotInfo(int client) {
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i) || !IsFakeClient(i)) continue;
        if (GetClientTeam(i) != 2) continue;

        char name[32];
        GetClientName(i, name, sizeof(name));

        // Current task
        char taskName[32];
        AITask_GetName(view_as<AITaskId>(g_BotState[i].currentTaskId), taskName, sizeof(taskName));

        // Slot 3 item
        char slot3[48];
        slot3[0] = '\0';
        int w3 = GetPlayerWeaponSlot(i, 3);
        if (w3 != -1 && IsValidEntity(w3)) {
            GetEntityClassname(w3, slot3, sizeof(slot3));
        } else {
            strcopy(slot3, sizeof(slot3), "none");
        }

        // Slot 2 item (throwable)
        char slot2[48];
        slot2[0] = '\0';
        int w2 = GetPlayerWeaponSlot(i, 2);
        if (w2 != -1 && IsValidEntity(w2)) {
            GetEntityClassname(w2, slot2, sizeof(slot2));
        } else {
            strcopy(slot2, sizeof(slot2), "none");
        }

        int hp = GetClientHealth(i);
        bool alive = IsPlayerAlive(i);
        bool incap = (g_BotState[i].isIncapacitated);
        bool pinned = (g_BotState[i].controlledByType > 0);

        PrintToChat(client, "\x04[AI Bot]\x03 %s\x01 HP:%d %s%s%s Task:%s Slot3:%s Slot2:%s",
            name, hp,
            alive ? "alive" : "DEAD",
            incap ? " INCAP" : "",
            pinned ? " PINNED" : "",
            taskName,
            slot3, slot2);
    }
}
