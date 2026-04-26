/**
 * L4D2 AI Bot - Visual Configuration Menu System
 *
 * Provides !botmenu in-game menu for configuring all AI Bot ConVars.
 * Depends on ai_core/config.inc for ConVar handles and cached values.
 */

#if defined _menu_system_included
    #endinput
#endif
#define _menu_system_included

// ============================================================
// 主菜单
// ============================================================

void AIMenu_ShowMain(int client) {
    Menu menu = new Menu(AIMenu_MainHandler);
    menu.SetTitle("=== AI Bot 配置菜单 ===\n ");

    char buf[128];

    Format(buf, sizeof(buf), "Bot 战斗能力 [当前: Lv%d]", g_iBotSkillLevel + 1);
    menu.AddItem("skill", buf);

    menu.AddItem("modules", "任务模块开关");
    menu.AddItem("behavior", "行为配置");
    menu.AddItem("protection", "保护设置");
    menu.AddItem("damage", "伤害配置");

    Format(buf, sizeof(buf), "LLM 战术顾问 [%s]", g_bLLMEnabled ? "开" : "关");
    menu.AddItem("llm", buf);

    Format(buf, sizeof(buf), "调试模式 [%s]", g_bDebugMode ? "开" : "关");
    menu.AddItem("debug", buf);

    menu.Display(client, MENU_TIME_FOREVER);
}

public int AIMenu_MainHandler(Menu menu, MenuAction action, int param1, int param2) {
    if (action == MenuAction_Select) {
        char item[32];
        menu.GetItem(param2, item, sizeof(item));

        if (StrEqual(item, "skill"))            AIMenu_ShowSkill(param1);
        else if (StrEqual(item, "modules"))     AIMenu_ShowModules(param1);
        else if (StrEqual(item, "behavior"))    AIMenu_ShowBehavior(param1);
        else if (StrEqual(item, "protection"))  AIMenu_ShowProtection(param1);
        else if (StrEqual(item, "damage"))      AIMenu_ShowDamage(param1);
        else if (StrEqual(item, "llm")) {
            bool newVal = !g_bLLMEnabled;
            g_cvLLMEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_llm_enabled", newVal);
            PrintToChat(param1, "\x04[AI Bot]\x01 LLM 战术顾问: %s", newVal ? "开启" : "关闭");
            AIMenu_ShowMain(param1);
        }
        else if (StrEqual(item, "debug")) {
            bool newVal = !g_bDebugMode;
            g_cvDebugMode.SetBool(newVal);
            AIMenu_PersistBool("ai_debug", newVal);
            PrintToChat(param1, "\x04[AI Bot]\x01 调试模式: %s", newVal ? "开启" : "关闭");
            AIMenu_ShowMain(param1);
        }
    }
    else if (action == MenuAction_End) {
        delete menu;
    }
    return 0;
}

// ============================================================
// 技能等级子菜单
// ============================================================

static char g_sSkillDesc[][] = {
    "Lv1 - 接近原版，团队协作提升",
    "Lv2 - 视野提升，注意侧面僵尸",
    "Lv3 - 视野大幅提升，可应对专家模式",
    "Lv4 - 作弊级，远超原版",
    "Lv5 - 顶尖高手，单人≈3个玩家"
};

void AIMenu_ShowSkill(int client) {
    Menu menu = new Menu(AIMenu_SkillHandler);
    menu.SetTitle("=== Bot 战斗能力 ===\n当前: Lv%d\n ", g_iBotSkillLevel + 1);

    char info[8];
    for (int i = 0; i < 5; i++) {
        IntToString(i, info, sizeof(info));
        menu.AddItem(info, g_sSkillDesc[i]);
    }

    menu.ExitBackButton = true;
    menu.Display(client, MENU_TIME_FOREVER);
}

public int AIMenu_SkillHandler(Menu menu, MenuAction action, int param1, int param2) {
    if (action == MenuAction_Select) {
        char info[8];
        menu.GetItem(param2, info, sizeof(info));
        int level = StringToInt(info);
        g_cvBotSkillLevel.SetInt(level);
        AIMenu_PersistInt("ai_bot_skill", level);
        PrintToChat(param1, "\x04[AI Bot]\x01 战斗能力已设为 Lv%d", level + 1);
        AIMenu_ShowMain(param1);
    }
    else if (action == MenuAction_Cancel) {
        if (param2 == MenuCancel_ExitBack)
            AIMenu_ShowMain(param1);
    }
    else if (action == MenuAction_End) {
        delete menu;
    }
    return 0;
}

// ============================================================
// 任务模块子菜单
// ============================================================

void AIMenu_ShowModules(int client) {
    Menu menu = new Menu(AIMenu_ModulesHandler);
    menu.SetTitle("=== 任务模块开关 ===\n ");

    char buf[64];

    Format(buf, sizeof(buf), "救援模块 [%s]", g_bRescueEnabled ? "开" : "关");
    menu.AddItem("rescue", buf);

    Format(buf, sizeof(buf), "治疗模块 [%s]", g_bHealEnabled ? "开" : "关");
    menu.AddItem("heal", buf);

    Format(buf, sizeof(buf), "投掷物模块 [%s]", g_bThrowableEnabled ? "开" : "关");
    menu.AddItem("throw", buf);

    Format(buf, sizeof(buf), "近战模块 [%s]", g_bMeleeEnabled ? "开" : "关");
    menu.AddItem("melee", buf);

    Format(buf, sizeof(buf), "寻油模块 [%s]", g_bFindGasEnabled ? "开" : "关");
    menu.AddItem("gas", buf);

    Format(buf, sizeof(buf), "除颤器 [%s]", g_bDefibEnabled ? "开" : "关");
    menu.AddItem("defib", buf);

    Format(buf, sizeof(buf), "升级包 [%s]", g_bUpgradeEnabled ? "开" : "关");
    menu.AddItem("upgrade", buf);

    Format(buf, sizeof(buf), "背物资 [%s]", g_bBackpackEnabled ? "开" : "关");
    menu.AddItem("backpack", buf);

    Format(buf, sizeof(buf), "物资管理 [%s]", g_bSupplyEnabled ? "开" : "关");
    menu.AddItem("supply", buf);

    menu.AddItem("back", "返回主菜单");

    menu.ExitButton = false;
    menu.Display(client, MENU_TIME_FOREVER);
}

public int AIMenu_ModulesHandler(Menu menu, MenuAction action, int param1, int param2) {
    if (action == MenuAction_Select) {
        char item[32];
        menu.GetItem(param2, item, sizeof(item));

        if (StrEqual(item, "back")) {
            AIMenu_ShowMain(param1);
            return 0;
        }

        char name[32];
        if (StrEqual(item, "rescue")) {
            bool newVal = !g_bRescueEnabled;
            g_cvRescueEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_rescue_enabled", newVal);
            strcopy(name, sizeof(name), "救援模块");
        }
        else if (StrEqual(item, "heal")) {
            bool newVal = !g_bHealEnabled;
            g_cvHealEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_heal_enabled", newVal);
            strcopy(name, sizeof(name), "治疗模块");
        }
        else if (StrEqual(item, "throw")) {
            bool newVal = !g_bThrowableEnabled;
            g_cvThrowableEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_throwable_enabled", newVal);
            strcopy(name, sizeof(name), "投掷物模块");
        }
        else if (StrEqual(item, "melee")) {
            bool newVal = !g_bMeleeEnabled;
            g_cvMeleeEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_melee_enabled", newVal);
            strcopy(name, sizeof(name), "近战模块");
        }
        else if (StrEqual(item, "gas")) {
            bool newVal = !g_bFindGasEnabled;
            g_cvFindGasEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_findgas_enabled", newVal);
            strcopy(name, sizeof(name), "寻油模块");
        }
        else if (StrEqual(item, "defib")) {
            bool newVal = !g_bDefibEnabled;
            g_cvDefibEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_defib_enabled", newVal);
            strcopy(name, sizeof(name), "除颤器");
        }
        else if (StrEqual(item, "upgrade")) {
            bool newVal = !g_bUpgradeEnabled;
            g_cvUpgradeEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_upgrade_enabled", newVal);
            strcopy(name, sizeof(name), "升级包");
        }
        else if (StrEqual(item, "backpack")) {
            bool newVal = !g_bBackpackEnabled;
            g_cvBackpackEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_backpack_enabled", newVal);
            strcopy(name, sizeof(name), "背物资");
        }
        else if (StrEqual(item, "supply")) {
            bool newVal = !g_bSupplyEnabled;
            g_cvSupplyEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_supply_enabled", newVal);
            strcopy(name, sizeof(name), "物资管理");
        }

        PrintToChat(param1, "\x04[AI Bot]\x01 %s 已切换", name);
        AIMenu_ShowModules(param1);
    }
    else if (action == MenuAction_End) {
        delete menu;
    }
    return 0;
}

// ============================================================
// 行为配置子菜单
// ============================================================

void AIMenu_ShowBehavior(int client) {
    Menu menu = new Menu(AIMenu_BehaviorHandler);
    menu.SetTitle("=== 行为配置 ===\n ");

    char buf[64];

    Format(buf, sizeof(buf), "跟随范围 [当前: %.0f]", g_fFollowRange);
    menu.AddItem("follow", buf);

    Format(buf, sizeof(buf), "传送范围 [当前: %.0f]", g_fTeleportRange);
    menu.AddItem("teleport", buf);

    Format(buf, sizeof(buf), "救人传送时长 [当前: %.0fs]", g_fSaveTeleportTime);
    menu.AddItem("savetime", buf);

    Format(buf, sizeof(buf), "玩家死后继续跑图 [%s]", g_bKeepaliveEnabled ? "开" : "关");
    menu.AddItem("keepalive", buf);

    menu.AddItem("back", "返回主菜单");

    menu.ExitButton = false;
    menu.Display(client, MENU_TIME_FOREVER);
}

public int AIMenu_BehaviorHandler(Menu menu, MenuAction action, int param1, int param2) {
    if (action == MenuAction_Select) {
        char item[32];
        menu.GetItem(param2, item, sizeof(item));

        if (StrEqual(item, "back")) {
            AIMenu_ShowMain(param1);
        }
        else if (StrEqual(item, "follow")) {
            AIMenu_ShowPresetFloat(param1, "follow", "跟随范围", "100\n200\n300\n500\n800\n1000");
        }
        else if (StrEqual(item, "teleport")) {
            AIMenu_ShowPresetFloat(param1, "teleport", "传送范围", "500\n800\n1000\n2000\n5000");
        }
        else if (StrEqual(item, "savetime")) {
            AIMenu_ShowPresetFloat(param1, "savetime", "救人传送时长", "3\n5\n9\n15\n30");
        }
        else if (StrEqual(item, "keepalive")) {
            bool newVal = !g_bKeepaliveEnabled;
            g_cvKeepaliveEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_keepalive_enabled", newVal);
            PrintToChat(param1, "\x04[AI Bot]\x01 玩家死后继续跑图: %s", newVal ? "开启" : "关闭");
            AIMenu_ShowBehavior(param1);
        }
    }
    else if (action == MenuAction_End) {
        delete menu;
    }
    return 0;
}

// ============================================================
// 保护设置子菜单
// ============================================================

void AIMenu_ShowProtection(int client) {
    Menu menu = new Menu(AIMenu_ProtectionHandler);
    menu.SetTitle("=== 保护设置 ===\n ");

    char buf[64];

    Format(buf, sizeof(buf), "免疫秒杀 [%s]", g_bProtectEnabled ? "开" : "关");
    menu.AddItem("protect", buf);

    Format(buf, sizeof(buf), "免疫黑枪 [%s]", g_bImmunityEnabled ? "开" : "关");
    menu.AddItem("immunity", buf);

    Format(buf, sizeof(buf), "寻路开关 [%s]", g_bPathfindingEnabled ? "开" : "关");
    menu.AddItem("pathfind", buf);

    Format(buf, sizeof(buf), "卡住传送 [%s]", g_bUnstickEnabled ? "开" : "关");
    menu.AddItem("unstick", buf);

    menu.AddItem("back", "返回主菜单");

    menu.ExitButton = false;
    menu.Display(client, MENU_TIME_FOREVER);
}

public int AIMenu_ProtectionHandler(Menu menu, MenuAction action, int param1, int param2) {
    if (action == MenuAction_Select) {
        char item[32];
        menu.GetItem(param2, item, sizeof(item));

        if (StrEqual(item, "back")) {
            AIMenu_ShowMain(param1);
            return 0;
        }

        char name[32];
        if (StrEqual(item, "protect")) {
            bool newVal = !g_bProtectEnabled;
            g_cvProtectEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_protect_enabled", newVal);
            strcopy(name, sizeof(name), "免疫秒杀");
        }
        else if (StrEqual(item, "immunity")) {
            bool newVal = !g_bImmunityEnabled;
            g_cvImmunityEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_immunity_enabled", newVal);
            strcopy(name, sizeof(name), "免疫黑枪");
        }
        else if (StrEqual(item, "pathfind")) {
            bool newVal = !g_bPathfindingEnabled;
            g_cvPathfindingEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_pathfinding_enabled", newVal);
            strcopy(name, sizeof(name), "寻路开关");
        }
        else if (StrEqual(item, "unstick")) {
            bool newVal = !g_bUnstickEnabled;
            g_cvUnstickEnabled.SetBool(newVal);
            AIMenu_PersistBool("ai_unstick_enabled", newVal);
            strcopy(name, sizeof(name), "卡住传送");
        }

        PrintToChat(param1, "\x04[AI Bot]\x01 %s 已切换", name);
        AIMenu_ShowProtection(param1);
    }
    else if (action == MenuAction_End) {
        delete menu;
    }
    return 0;
}

// ============================================================
// 伤害配置子菜单
// ============================================================

void AIMenu_ShowDamage(int client) {
    Menu menu = new Menu(AIMenu_DamageHandler);
    menu.SetTitle("=== 伤害配置 ===\n ");

    char buf[64];

    Format(buf, sizeof(buf), "Witch 伤害 [当前: %.1fx]", g_fWitchDamage);
    menu.AddItem("witch", buf);

    Format(buf, sizeof(buf), "特感伤害 [当前: %.1fx]", g_fSpecialDamage);
    menu.AddItem("special", buf);

    Format(buf, sizeof(buf), "Tank 伤害 [当前: %.1fx]", g_fTankDamage);
    menu.AddItem("tank", buf);

    menu.AddItem("back", "返回主菜单");

    menu.ExitButton = false;
    menu.Display(client, MENU_TIME_FOREVER);
}

public int AIMenu_DamageHandler(Menu menu, MenuAction action, int param1, int param2) {
    if (action == MenuAction_Select) {
        char item[32];
        menu.GetItem(param2, item, sizeof(item));

        if (StrEqual(item, "back")) {
            AIMenu_ShowMain(param1);
        }
        else if (StrEqual(item, "witch")) {
            AIMenu_ShowPresetFloat(param1, "dmg_witch", "Witch 伤害倍率", "0.5\n1.0\n1.5\n2.0\n3.0\n5.0");
        }
        else if (StrEqual(item, "special")) {
            AIMenu_ShowPresetFloat(param1, "dmg_special", "特感伤害倍率", "0.5\n1.0\n1.5\n2.0\n3.0\n5.0");
        }
        else if (StrEqual(item, "tank")) {
            AIMenu_ShowPresetFloat(param1, "dmg_tank", "Tank 伤害倍率", "0.5\n1.0\n1.5\n2.0\n3.0\n5.0");
        }
    }
    else if (action == MenuAction_End) {
        delete menu;
    }
    return 0;
}

// ============================================================
// 通用预设值选择子菜单
// ============================================================

/**
 * Show a preset value selection menu.
 *
 * @param client    Client index.
 * @param type      Identifier like "follow", "teleport", "dmg_witch" etc.
 * @param title     Display title for the menu.
 * @param presets   Newline-separated preset values (e.g. "100\n200\n300").
 */
void AIMenu_ShowPresetFloat(int client, const char[] type, const char[] title, const char[] presets) {
    Menu menu = new Menu(AIMenu_PresetFloatHandler);

    char menuTitle[128];
    Format(menuTitle, sizeof(menuTitle), "=== %s ===\n选择新值:\n ", title);
    menu.SetTitle(menuTitle);

    // Parse newline-separated presets
    char values[16][16];
    int count = ExplodeString(presets, "\n", values, sizeof(values), sizeof(values[]));

    char info[64], display[32];
    for (int i = 0; i < count; i++) {
        TrimString(values[i]);
        // info = "type:value" so handler knows which ConVar to set
        Format(info, sizeof(info), "%s:%s", type, values[i]);
        Format(display, sizeof(display), "%s", values[i]);
        menu.AddItem(info, display);
    }

    menu.ExitBackButton = true;
    menu.Display(client, MENU_TIME_FOREVER);
}

public int AIMenu_PresetFloatHandler(Menu menu, MenuAction action, int param1, int param2) {
    if (action == MenuAction_Select) {
        char info[64];
        menu.GetItem(param2, info, sizeof(info));

        // Split "type:value"
        char parts[2][32];
        ExplodeString(info, ":", parts, sizeof(parts), sizeof(parts[]));

        float value = StringToFloat(parts[1]);

        // Apply value to the correct ConVar based on type
        if (StrEqual(parts[0], "follow")) {
            g_cvFollowRange.SetFloat(value);
            AIMenu_PersistFloat("ai_follow_range", value);
            PrintToChat(param1, "\x04[AI Bot]\x01 跟随范围已设为 %.0f", value);
            AIMenu_ShowBehavior(param1);
        }
        else if (StrEqual(parts[0], "teleport")) {
            g_cvTeleportRange.SetFloat(value);
            AIMenu_PersistFloat("ai_teleport_range", value);
            PrintToChat(param1, "\x04[AI Bot]\x01 传送范围已设为 %.0f", value);
            AIMenu_ShowBehavior(param1);
        }
        else if (StrEqual(parts[0], "savetime")) {
            g_cvSaveTeleportTime.SetFloat(value);
            AIMenu_PersistFloat("ai_save_teleport_time", value);
            PrintToChat(param1, "\x04[AI Bot]\x01 救人传送时长已设为 %.0fs", value);
            AIMenu_ShowBehavior(param1);
        }
        else if (StrEqual(parts[0], "dmg_witch")) {
            g_cvWitchDamage.SetFloat(value);
            AIMenu_PersistFloat("ai_witch_damage", value);
            PrintToChat(param1, "\x04[AI Bot]\x01 Witch 伤害倍率已设为 %.1fx", value);
            AIMenu_ShowDamage(param1);
        }
        else if (StrEqual(parts[0], "dmg_special")) {
            g_cvSpecialDamage.SetFloat(value);
            AIMenu_PersistFloat("ai_special_damage", value);
            PrintToChat(param1, "\x04[AI Bot]\x01 特感伤害倍率已设为 %.1fx", value);
            AIMenu_ShowDamage(param1);
        }
        else if (StrEqual(parts[0], "dmg_tank")) {
            g_cvTankDamage.SetFloat(value);
            AIMenu_PersistFloat("ai_tank_damage", value);
            PrintToChat(param1, "\x04[AI Bot]\x01 Tank 伤害倍率已设为 %.1fx", value);
            AIMenu_ShowDamage(param1);
        }
    }
    else if (action == MenuAction_Cancel) {
        if (param2 == MenuCancel_ExitBack) {
            // Try to determine parent from menu item info prefix
            char firstInfo[64];
            if (menu.ItemCount > 0) {
                menu.GetItem(0, firstInfo, sizeof(firstInfo));
                if (strncmp(firstInfo, "dmg_", 4) == 0)
                    AIMenu_ShowDamage(param1);
                else
                    AIMenu_ShowBehavior(param1);
            } else {
                AIMenu_ShowMain(param1);
            }
        }
    }
    else if (action == MenuAction_End) {
        delete menu;
    }
    return 0;
}

// ============================================================
// 配置持久化
// ============================================================

/**
 * 将 ConVar 修改持久化到 sourcemod_all_plugins.cfg
 * 确保换图后配置不丢失
 */
void AIMenu_PersistConVar(const char[] cvarName, const char[] value)
{
    char configPath[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, configPath, sizeof(configPath), "../../cfg/sourcemod/sourcemod_all_plugins.cfg");

    // 读取整个文件
    File file = OpenFile(configPath, "r");
    if (file == null)
    {
        LogError("[AI Bot] Cannot open config file for persistence: %s", configPath);
        return;
    }

    ArrayList lines = new ArrayList(ByteCountToCells(256));
    char buffer[256];
    bool found = false;

    while (file.ReadLine(buffer, sizeof(buffer)))
    {
        TrimString(buffer);

        // 检查这行是否是我们要更新的 ConVar
        char prefix[128];
        FormatEx(prefix, sizeof(prefix), "%s ", cvarName);
        if (strncmp(buffer, prefix, strlen(prefix), false) == 0)
        {
            // 替换这行
            FormatEx(buffer, sizeof(buffer), "%s \"%s\"", cvarName, value);
            found = true;
        }

        lines.PushString(buffer);
    }
    delete file;

    // 如果没找到，追加
    if (!found)
    {
        FormatEx(buffer, sizeof(buffer), "%s \"%s\"", cvarName, value);
        lines.PushString(buffer);
    }

    // 写回文件
    file = OpenFile(configPath, "w");
    if (file == null)
    {
        delete lines;
        LogError("[AI Bot] Cannot write config file for persistence: %s", configPath);
        return;
    }

    for (int i = 0; i < lines.Length; i++)
    {
        lines.GetString(i, buffer, sizeof(buffer));
        file.WriteLine("%s", buffer);
    }

    delete file;
    delete lines;
}

/**
 * 持久化布尔型 ConVar
 */
void AIMenu_PersistBool(const char[] cvarName, bool val)
{
    AIMenu_PersistConVar(cvarName, val ? "1" : "0");
}

/**
 * 持久化整型 ConVar
 */
void AIMenu_PersistInt(const char[] cvarName, int val)
{
    char buf[16];
    IntToString(val, buf, sizeof(buf));
    AIMenu_PersistConVar(cvarName, buf);
}

/**
 * 持久化浮点型 ConVar
 */
void AIMenu_PersistFloat(const char[] cvarName, float val)
{
    char buf[32];
    FloatToString(val, buf, sizeof(buf));
    AIMenu_PersistConVar(cvarName, buf);
}
