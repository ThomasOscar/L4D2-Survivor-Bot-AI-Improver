/**
 * L4D2 AI Bot - ConVar Configuration
 * 
 * All plugin ConVars and their cached values.
 * Uses change hooks to keep cached values in sync.
 */

#if defined _ai_config_included
 #endinput
#endif
#define _ai_config_included

// ConVar handles
ConVar g_cvBotEnabled;
ConVar g_cvBotSkillLevel;
ConVar g_cvRescueEnabled;
ConVar g_cvHealEnabled;
ConVar g_cvThrowableEnabled;
ConVar g_cvLLMEnabled;
ConVar g_cvDebugMode;
ConVar g_cvFollowRange;
ConVar g_cvTeleportRange;
ConVar g_cvMeleeEnabled;
ConVar g_cvImmunityEnabled;
ConVar g_cvPathfindingEnabled;
ConVar g_cvUnstickEnabled;
ConVar g_cvUpgradeEnabled;
ConVar g_cvKeepaliveEnabled;
ConVar g_cvBackpackEnabled;
ConVar g_cvDefibEnabled;
ConVar g_cvFindGasEnabled;
ConVar g_cvProtectEnabled;
ConVar g_cvWitchDamage;
ConVar g_cvSpecialDamage;
ConVar g_cvTankDamage;
ConVar g_cvSaveTeleportTime;
ConVar g_cvSupplyEnabled;
ConVar g_cvSupplyGiveCooldown;

// Cached values for fast access (avoid native calls every frame)
bool g_bBotEnabled;
int g_iBotSkillLevel;
bool g_bRescueEnabled;
bool g_bHealEnabled;
bool g_bThrowableEnabled;
bool g_bLLMEnabled;
bool g_bDebugMode;
float g_fFollowRange;
float g_fTeleportRange;
bool g_bMeleeEnabled;
bool g_bImmunityEnabled;
bool g_bPathfindingEnabled;
bool g_bUnstickEnabled;
bool g_bUpgradeEnabled;
bool g_bKeepaliveEnabled;
bool g_bBackpackEnabled;
bool g_bDefibEnabled;
bool g_bFindGasEnabled;
bool g_bProtectEnabled;
float g_fWitchDamage;
float g_fSpecialDamage;
float g_fTankDamage;
float g_fSaveTeleportTime;
bool g_bSupplyEnabled;
float g_fSupplyGiveCooldown;

/**
 * Initialize all ConVars. Called from OnPluginStart.
 */
void AIConfig_Init() {
    g_cvBotEnabled      = CreateConVar("ai_bot_enabled",       "1", "Enable AI Bot system", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvBotSkillLevel   = CreateConVar("ai_bot_skill",         "3", "Bot skill level (0=noob, 4=expert)", FCVAR_NOTIFY, true, 0.0, true, 4.0);
    g_cvRescueEnabled   = CreateConVar("ai_rescue_enabled",    "1", "Enable rescue task", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvHealEnabled     = CreateConVar("ai_heal_enabled",      "1", "Enable heal task", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvThrowableEnabled = CreateConVar("ai_throwable_enabled","1", "Enable throwable task", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvLLMEnabled      = CreateConVar("ai_llm_enabled",       "0", "Enable LLM strategic advisor", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvDebugMode       = CreateConVar("ai_debug",             "0", "Debug mode (1=log task switches)", FCVAR_NOTIFY, true, 0.0, true, 1.0);

    // Extended ConVars — ranges & floats
    g_cvFollowRange       = CreateConVar("ai_follow_range",       "300.0", "Bot follow range", FCVAR_NOTIFY, true, 100.0, true, 999999.0);
    g_cvTeleportRange     = CreateConVar("ai_teleport_range",     "1000.0", "Bot teleport range", FCVAR_NOTIFY, true, 100.0, true, 999999.0);
    g_cvSaveTeleportTime  = CreateConVar("ai_save_teleport_time", "9.0", "Rescue teleport timeout (seconds)", FCVAR_NOTIFY, true, 0.0, true, 999.0);
    g_cvWitchDamage       = CreateConVar("ai_witch_damage",       "1.0", "Witch damage multiplier", FCVAR_NOTIFY, true, -16.0, true, 16.0);
    g_cvSpecialDamage     = CreateConVar("ai_special_damage",     "1.0", "Special infected damage multiplier", FCVAR_NOTIFY, true, -16.0, true, 16.0);
    g_cvTankDamage        = CreateConVar("ai_tank_damage",        "1.0", "Tank damage multiplier", FCVAR_NOTIFY, true, -16.0, true, 16.0);

    // Extended ConVars — boolean switches
    g_cvMeleeEnabled        = CreateConVar("ai_melee_enabled",        "1", "Enable melee combat", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvImmunityEnabled     = CreateConVar("ai_immunity_enabled",     "0", "Enable friendly fire immunity", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvPathfindingEnabled  = CreateConVar("ai_pathfinding_enabled",  "1", "Enable pathfinding", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvUnstickEnabled      = CreateConVar("ai_unstick_enabled",      "1", "Enable stuck teleport", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvUpgradeEnabled      = CreateConVar("ai_upgrade_enabled",      "1", "Enable upgrade packs", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvKeepaliveEnabled    = CreateConVar("ai_keepalive_enabled",    "0", "Enable bot run after player death", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvBackpackEnabled     = CreateConVar("ai_backpack_enabled",     "1", "Enable carry supplies", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvDefibEnabled        = CreateConVar("ai_defib_enabled",        "1", "Enable defibrillator use", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvFindGasEnabled      = CreateConVar("ai_findgas_enabled",      "1", "Enable gas can search", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvProtectEnabled     = CreateConVar("ai_protect_enabled",     "0", "Enable instant-kill protection", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvSupplyEnabled      = CreateConVar("ai_supply_enabled",      "1", "Enable supply management for bots", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvSupplyGiveCooldown = CreateConVar("ai_supply_give_cooldown","2.0", "Cooldown between giving items to players (seconds)", FCVAR_NOTIFY, true, 0.5, true, 30.0);

    // Auto-generate and load config file
    AutoExecConfig(true, "l4d2_ai_bot");

    // Hook changes for cache sync
    g_cvBotEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvBotSkillLevel.AddChangeHook(AIConfig_OnChanged);
    g_cvRescueEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvHealEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvThrowableEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvLLMEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvDebugMode.AddChangeHook(AIConfig_OnChanged);
    g_cvFollowRange.AddChangeHook(AIConfig_OnChanged);
    g_cvTeleportRange.AddChangeHook(AIConfig_OnChanged);
    g_cvSaveTeleportTime.AddChangeHook(AIConfig_OnChanged);
    g_cvWitchDamage.AddChangeHook(AIConfig_OnChanged);
    g_cvSpecialDamage.AddChangeHook(AIConfig_OnChanged);
    g_cvTankDamage.AddChangeHook(AIConfig_OnChanged);
    g_cvMeleeEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvImmunityEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvPathfindingEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvUnstickEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvUpgradeEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvKeepaliveEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvBackpackEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvDefibEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvFindGasEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvProtectEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvSupplyEnabled.AddChangeHook(AIConfig_OnChanged);
    g_cvSupplyGiveCooldown.AddChangeHook(AIConfig_OnChanged);

    // Initial cache population
    AIConfig_UpdateCache();
}

/**
 * ConVar change hook — refresh all cached values.
 */
void AIConfig_OnChanged(ConVar convar, const char[] oldValue, const char[] newValue) {
    AIConfig_UpdateCache();
}

/**
 * Update all cached ConVar values from their handles.
 */
void AIConfig_UpdateCache() {
    g_bBotEnabled       = g_cvBotEnabled.BoolValue;
    g_iBotSkillLevel    = g_cvBotSkillLevel.IntValue;
    g_bRescueEnabled    = g_cvRescueEnabled.BoolValue;
    g_bHealEnabled      = g_cvHealEnabled.BoolValue;
    g_bThrowableEnabled = g_cvThrowableEnabled.BoolValue;
    g_bLLMEnabled       = g_cvLLMEnabled.BoolValue;
    g_bDebugMode        = g_cvDebugMode.BoolValue;

    // Extended cached values
    g_fFollowRange        = g_cvFollowRange.FloatValue;
    g_fTeleportRange      = g_cvTeleportRange.FloatValue;
    g_fSaveTeleportTime   = g_cvSaveTeleportTime.FloatValue;
    g_fWitchDamage        = g_cvWitchDamage.FloatValue;
    g_fSpecialDamage      = g_cvSpecialDamage.FloatValue;
    g_fTankDamage         = g_cvTankDamage.FloatValue;
    g_bMeleeEnabled       = g_cvMeleeEnabled.BoolValue;
    g_bImmunityEnabled    = g_cvImmunityEnabled.BoolValue;
    g_bPathfindingEnabled = g_cvPathfindingEnabled.BoolValue;
    g_bUnstickEnabled     = g_cvUnstickEnabled.BoolValue;
    g_bUpgradeEnabled     = g_cvUpgradeEnabled.BoolValue;
    g_bKeepaliveEnabled   = g_cvKeepaliveEnabled.BoolValue;
    g_bBackpackEnabled    = g_cvBackpackEnabled.BoolValue;
    g_bDefibEnabled       = g_cvDefibEnabled.BoolValue;
    g_bFindGasEnabled     = g_cvFindGasEnabled.BoolValue;
    g_bProtectEnabled     = g_cvProtectEnabled.BoolValue;
    g_bSupplyEnabled      = g_cvSupplyEnabled.BoolValue;
    g_fSupplyGiveCooldown = g_cvSupplyGiveCooldown.FloatValue;
}
