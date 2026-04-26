/**
 * L4D2 AI Bot - Task Base Definitions
 * 
 * Defines the task system's core types, enums, and callback signatures.
 * Each task module implements ShouldUpdate/Update/Reset functions.
 */

#if defined _ai_task_base_included
 #endinput
#endif
#define _ai_task_base_included

// Maximum number of registered tasks
#define MAX_TASKS 16

/**
 * Task ID enumeration.
 * Order here does NOT determine priority — that is set via the 'order' field
 * when registering tasks in AITaskQueue_RegisterTask().
 */
enum AITaskId {
    TASK_NONE = -1,
    TASK_RESCUE = 0,        // Rescue pinned/incapped teammates (highest priority)
    TASK_COMBAT,            // Combat shooting
    TASK_THROWABLE,         // Throwable search and use
    TASK_HEAL,              // Healing (self/teammate)
    TASK_NAVIGATE,          // Navigation and following
    TASK_WEAPON,            // Weapon management (pickup/swap)
    TASK_PROP_CARRY,        // Prop carrying (gas cans, etc.)
    TASK_SEARCH,            // Item search/scavenging
    TASK_DAMAGE_PROTECT,    // Damage protection (fire/acid/fall — SDKHook driven)
    TASK_ITEM_PASS,         // Item passing to teammates
    TASK_TRIGGER_SEARCH,    // Trigger/button search and activation
    TASK_DEFIB,             // Defibrillator use (priority 2)
    TASK_UPGRADE,           // Upgrade pack use (priority 4)
    TASK_SUPPLY,            // Supply management (ensure bot has items, give to humans)
    TASK_MAX                // Total task count sentinel
}

/**
 * Task definition structure.
 * Stored in the global task registry and sorted by 'order' (ascending = higher priority).
 */
enum struct AITaskDef {
    AITaskId id;                // Task ID
    int order;                  // Priority order (0 = highest, 5 = lowest)
    char name[32];              // Task name (for debug logging)
    bool enabled;               // Whether this task is enabled
    float tickInterval;         // Check interval in seconds (0.0 = every frame)
    float lastTickTime;         // Internal: last time this task was checked
}

/**
 * Get a human-readable name for a task ID.
 */
void AITask_GetName(AITaskId taskId, char[] buffer, int maxlen) {
    switch (taskId) {
        case TASK_NONE:       strcopy(buffer, maxlen, "None");
        case TASK_RESCUE:     strcopy(buffer, maxlen, "Rescue");
        case TASK_COMBAT:     strcopy(buffer, maxlen, "Combat");
        case TASK_THROWABLE:  strcopy(buffer, maxlen, "Throwable");
        case TASK_HEAL:       strcopy(buffer, maxlen, "Heal");
        case TASK_NAVIGATE:   strcopy(buffer, maxlen, "Navigate");
        case TASK_WEAPON:     strcopy(buffer, maxlen, "Weapon");
        case TASK_PROP_CARRY: strcopy(buffer, maxlen, "PropCarry");
        case TASK_SEARCH:     strcopy(buffer, maxlen, "Search");
        case TASK_DAMAGE_PROTECT: strcopy(buffer, maxlen, "DamageProtect");
        case TASK_ITEM_PASS:  strcopy(buffer, maxlen, "ItemPass");
        case TASK_TRIGGER_SEARCH: strcopy(buffer, maxlen, "TriggerSearch");
        case TASK_DEFIB:      strcopy(buffer, maxlen, "Defib");
        case TASK_UPGRADE:    strcopy(buffer, maxlen, "Upgrade");
        case TASK_SUPPLY:     strcopy(buffer, maxlen, "Supply");
        default:              strcopy(buffer, maxlen, "Unknown");
    }
}
