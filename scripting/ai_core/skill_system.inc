/**
 * L4D2 AI Bot - Skill System
 * 
 * 5-level skill configuration system (0-4).
 * Controls bot behavior parameters like reaction delay, aim accuracy, etc.
 * 
 * Levels:
 *   0 = Near-vanilla  (clumsy, slow)
 *   1 = Improved      (slightly better)
 *   2 = Standard      (default balanced)
 *   3 = Enhanced      (strong)
 *   4 = Expert        (top-tier player)
 */

#if defined _ai_skill_system_included
 #endinput
#endif
#define _ai_skill_system_included

#define SKILL_LEVEL_MIN 0
#define SKILL_LEVEL_MAX 4
#define SKILL_LEVEL_COUNT 5

/**
 * Skill configuration for each level.
 * Each task module can read these values to adjust its behavior.
 */
enum struct SkillConfig {
    float viewMultiplier;       // Vision range multiplier (1.0x - 5.0x)
    float reactionDelay;        // Reaction delay in seconds before acting
    float shoveSuccessRate;     // Shove success probability (0.0 - 1.0)
    float aimAccuracy;          // Aim deviation in degrees (lower = better)
    float damageMultiplier;     // Damage compensation factor (0.0 = none)
    float healThreshold;        // HP threshold to consider healing (lower = smarter)
    float throwableAccuracy;    // Throwable aim deviation in degrees
}

// Global skill config array — indexed by skill level 0..4
SkillConfig g_SkillConfigs[SKILL_LEVEL_COUNT];

/**
 * Initialize skill configurations for all 5 levels.
 * Called from OnPluginStart.
 */
void SkillSystem_Init() {
    // Level 0: Near-vanilla — clumsy, slow reactions
    g_SkillConfigs[0].viewMultiplier     = 1.0;
    g_SkillConfigs[0].reactionDelay      = 0.5;
    g_SkillConfigs[0].shoveSuccessRate   = 0.3;
    g_SkillConfigs[0].aimAccuracy        = 15.0;
    g_SkillConfigs[0].damageMultiplier   = 0.0;
    g_SkillConfigs[0].healThreshold      = 40.0;
    g_SkillConfigs[0].throwableAccuracy  = 20.0;

    // Level 1: Improved — slightly better than stock
    g_SkillConfigs[1].viewMultiplier     = 1.5;
    g_SkillConfigs[1].reactionDelay      = 0.3;
    g_SkillConfigs[1].shoveSuccessRate   = 0.5;
    g_SkillConfigs[1].aimAccuracy        = 10.0;
    g_SkillConfigs[1].damageMultiplier   = 0.0;
    g_SkillConfigs[1].healThreshold      = 35.0;
    g_SkillConfigs[1].throwableAccuracy  = 15.0;

    // Level 2: Standard — balanced default
    g_SkillConfigs[2].viewMultiplier     = 2.0;
    g_SkillConfigs[2].reactionDelay      = 0.2;
    g_SkillConfigs[2].shoveSuccessRate   = 0.7;
    g_SkillConfigs[2].aimAccuracy        = 5.0;
    g_SkillConfigs[2].damageMultiplier   = 0.0;
    g_SkillConfigs[2].healThreshold      = 30.0;
    g_SkillConfigs[2].throwableAccuracy  = 8.0;

    // Level 3: Enhanced — strong, fast reactions
    g_SkillConfigs[3].viewMultiplier     = 3.0;
    g_SkillConfigs[3].reactionDelay      = 0.1;
    g_SkillConfigs[3].shoveSuccessRate   = 0.9;
    g_SkillConfigs[3].aimAccuracy        = 2.0;
    g_SkillConfigs[3].damageMultiplier   = 0.1;
    g_SkillConfigs[3].healThreshold      = 25.0;
    g_SkillConfigs[3].throwableAccuracy  = 3.0;

    // Level 4: Expert — top-tier player, near-perfect
    g_SkillConfigs[4].viewMultiplier     = 5.0;
    g_SkillConfigs[4].reactionDelay      = 0.05;
    g_SkillConfigs[4].shoveSuccessRate   = 1.0;
    g_SkillConfigs[4].aimAccuracy        = 0.0;
    g_SkillConfigs[4].damageMultiplier   = 0.2;
    g_SkillConfigs[4].healThreshold      = 20.0;
    g_SkillConfigs[4].throwableAccuracy  = 0.0;

    LogMessage("[AI Bot] Skill system initialized with %d levels", SKILL_LEVEL_COUNT);
}

/**
 * Get the SkillConfig for the current ConVar skill level.
 * Clamped to valid range [0..4].
 *
 * @param out   SkillConfig struct to fill
 */
void GetCurrentSkillConfig(SkillConfig out) {
    int level = g_iBotSkillLevel;
    if (level < SKILL_LEVEL_MIN) level = SKILL_LEVEL_MIN;
    if (level > SKILL_LEVEL_MAX) level = SKILL_LEVEL_MAX;

    out.viewMultiplier    = g_SkillConfigs[level].viewMultiplier;
    out.reactionDelay     = g_SkillConfigs[level].reactionDelay;
    out.shoveSuccessRate  = g_SkillConfigs[level].shoveSuccessRate;
    out.aimAccuracy       = g_SkillConfigs[level].aimAccuracy;
    out.damageMultiplier  = g_SkillConfigs[level].damageMultiplier;
    out.healThreshold     = g_SkillConfigs[level].healThreshold;
    out.throwableAccuracy = g_SkillConfigs[level].throwableAccuracy;
}

/**
 * Get a specific float parameter for the current skill level.
 * Convenience helper to avoid copying the full struct.
 *
 * @param field  Name of the field (for switch dispatch)
 * @return       The value for the current skill level
 */
float GetSkillFloat(const char[] field) {
    int level = g_iBotSkillLevel;
    if (level < SKILL_LEVEL_MIN) level = SKILL_LEVEL_MIN;
    if (level > SKILL_LEVEL_MAX) level = SKILL_LEVEL_MAX;

    if (StrEqual(field, "view"))       return g_SkillConfigs[level].viewMultiplier;
    if (StrEqual(field, "reaction"))   return g_SkillConfigs[level].reactionDelay;
    if (StrEqual(field, "shove"))      return g_SkillConfigs[level].shoveSuccessRate;
    if (StrEqual(field, "aim"))        return g_SkillConfigs[level].aimAccuracy;
    if (StrEqual(field, "damage"))     return g_SkillConfigs[level].damageMultiplier;
    if (StrEqual(field, "heal"))       return g_SkillConfigs[level].healThreshold;
    if (StrEqual(field, "throwable"))  return g_SkillConfigs[level].throwableAccuracy;

    return 0.0;
}

/**
 * Check if a probabilistic action succeeds at current skill level.
 * Used for shove success, etc.
 *
 * @param rate  Success rate (0.0 - 1.0)
 * @return      true if the random roll succeeds
 */
bool SkillRollSuccess(float rate) {
    if (rate >= 1.0) return true;
    if (rate <= 0.0) return false;
    return GetRandomFloat(0.0, 1.0) < rate;
}

/**
 * Get a human-readable skill level name for logging.
 *
 * @param level     Skill level (0-4)
 * @param buffer    Output buffer
 * @param maxlen    Buffer size
 */
void GetSkillLevelName(int level, char[] buffer, int maxlen) {
    switch (level) {
        case 0: strcopy(buffer, maxlen, "Near-vanilla");
        case 1: strcopy(buffer, maxlen, "Improved");
        case 2: strcopy(buffer, maxlen, "Standard");
        case 3: strcopy(buffer, maxlen, "Enhanced");
        case 4: strcopy(buffer, maxlen, "Expert");
        default: strcopy(buffer, maxlen, "Unknown");
    }
}
